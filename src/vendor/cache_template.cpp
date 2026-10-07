// esv::Item::CreateCacheTemplate, after upstream's GameHelpers.cpp and
// RootTemplates.inl (by Norbyte and the bg3se contributors): the item's root
// or global template is cloned into the server's cache manager, and the item
// switched to the clone. Everything is checked before anything is written; a
// local template, or anything that does not check out, returns null.
#include <stdafx.h>
#include <GameDefinitions/Components/All.h>
#include <GameDefinitions/EntitySystem.h>
#include <GameDefinitions/Item.h>
#include <GameDefinitions/RootTemplates.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#include <pthread.h>

#include "engine_containers.h"
#include "../ecs_types.h"
#include "../hook.h"
#include "../log.h"
#include "../mem.h"

extern "C" void* bg3le_templates_cache_manager();
extern "C" void* bg3le_entity_world(void* container);
extern "C" bool bg3le_fixed_string_create(char const* text, std::uint32_t* out);
extern "C" void bg3le_fixed_string_pin(std::uint32_t index);

namespace {

using bg3se::CacheTemplateManagerBase;
using bg3se::GameObjectTemplate;
using bg3se::TemplateType;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
constexpr std::size_t kTemplates = offsetof(CacheTemplateManagerBase, Templates);
constexpr std::size_t kByHandle = offsetof(CacheTemplateManagerBase, TemplatesByHandle);
constexpr std::size_t kRefCounts = offsetof(CacheTemplateManagerBase, RefCountsByHandle);
constexpr std::size_t kLock = offsetof(CacheTemplateManagerBase, Lock);
constexpr std::size_t kId = offsetof(GameObjectTemplate, Id);
constexpr std::size_t kTemplateName = offsetof(GameObjectTemplate, TemplateName);
constexpr std::size_t kParentId = offsetof(GameObjectTemplate, ParentTemplateId);
constexpr std::size_t kHandle = offsetof(GameObjectTemplate, TemplateHandle);
constexpr std::size_t kLevelName = offsetof(GameObjectTemplate, LevelName);
constexpr std::size_t kSystems = offsetof(bg3se::ecs::EntityWorld, Systems);
constexpr std::size_t kTemplateSwitch = offsetof(bg3se::esv::templates::ChangeSystem, TemplateSwitch);
#pragma clang diagnostic pop
static_assert(kLock == 0xd0, "templates.cpp reads the lock here");
static_assert(kParentId + 4 == kHandle, "the handle follows ParentTemplateId");

constexpr std::uint32_t kNull = 0xffffffffu;
constexpr std::size_t kVtSlotGetNewIndex = 5;  // D1, D0, Clear, ShouldSave, OnLoad
constexpr std::size_t kVtSlotClone = 15;

template <class T>
bool read(std::uintptr_t at, T* out) {
    return bg3le::safe_read((void const*)at, out, sizeof(T));
}

bool code_ptr(std::uintptr_t fn) {
    return fn > bg3le::load_bias() && bg3le::in_text(fn - bg3le::load_bias(), 1);
}

// The engine's lock as templates.cpp takes it: not at all on a thread that
// already holds it for writing, and the owner word left alone.
struct EngineLock {
    pthread_rwlock_t* Lock{nullptr};
    EngineLock(std::uintptr_t mgr, bool write) {
        std::uint64_t owner = ~0ull;
        if (read(mgr + kLock + sizeof(pthread_rwlock_t), &owner)
            && owner == (std::uint64_t)pthread_self()) {
            return;
        }
        Lock = (pthread_rwlock_t*)(mgr + kLock);
        for (int i = 0; i < 4001; ++i) {
            if ((write ? pthread_rwlock_trywrlock(Lock) : pthread_rwlock_tryrdlock(Lock)) == 0) return;
        }
        if (write) pthread_rwlock_wrlock(Lock);
        else pthread_rwlock_rdlock(Lock);
    }
    ~EngineLock() {
        if (Lock != nullptr) pthread_rwlock_unlock(Lock);
    }
};

// Where AddedTemplates starts past the lock. The engine's Linux lock is wider
// than the vendored SRWLock, so it is read off GetNewIndex, which addresses
// NextTemplateId: AddedTemplates, RemovedTemplates, EnableTemplateSync, then it.
std::size_t sync_offset(std::uintptr_t vtable) {
    static std::size_t found = 0;
    if (found != 0) return found;
    std::uintptr_t fn = 0;
    unsigned char code[64] = {};
    if (!read(vtable + kVtSlotGetNewIndex * 8, &fn) || !code_ptr(fn)
        || !bg3le::safe_read((void const*)fn, code, sizeof(code))) {
        return 0;
    }
    for (std::size_t lockSize : {std::size_t{64}, std::size_t{72}}) {
        const std::uint32_t next = (std::uint32_t)(kLock + lockSize + 36);
        for (std::size_t i = 0; i + 5 <= sizeof(code); ++i) {
            std::uint32_t disp = 0;
            std::memcpy(&disp, code + i + 1, 4);
            if ((code[i] & 0xc7) == 0x87 && disp == next) {
                found = kLock + lockSize;
                bg3le::logf("cache templates: GetNewIndex addresses NextTemplateId at %#x; "
                            "the lock is %zu bytes", next, lockSize);
                return found;
            }
        }
    }
    char hex[3 * 32 + 1] = {};
    for (int i = 0; i < 32; ++i) std::snprintf(hex + 3 * i, 4, "%02x ", code[i]);
    bg3le::logf("cache templates: GetNewIndex (image+%#lx) does not address NextTemplateId: %s",
                (unsigned long)(fn - bg3le::load_bias()), hex);
    return 0;
}

bool plausible_array(std::uintptr_t at) {
    bg3le::RawArray a{};
    return read(at, &a) && a.Size <= a.Capacity && a.Capacity < (1u << 20)
           && ((a.Buffer == nullptr) == (a.Capacity == 0));
}

// The ChangeSystem's TemplateSwitch map, or 0.
std::uintptr_t template_switch(void* container) {
    auto* world = (char*)bg3le_entity_world(container);
    const auto index = bg3le::ecs::index_of(bg3le::ecs::Context::System,
                                            "esv::templates::ChangeSystem");
    bg3le::RawArray systems{};
    if (world == nullptr || !index || !read((std::uintptr_t)world + kSystems + 8, &systems)
        || systems.Buffer == nullptr || (std::uint32_t)*index >= systems.Size) {
        bg3le::logf("cache templates: no ChangeSystem (index %d)", index ? *index : -1);
        return 0;
    }
    const auto entry = (std::uintptr_t)systems.Buffer + (std::size_t)*index * sizeof(bg3se::ecs::SystemTypeEntry);
    std::uintptr_t system = 0, vmt = 0;
    std::int32_t own = -1;
    if (!read(entry, &system) || !read(entry + 8, &own) || own != *index || system == 0
        || !read(system, &vmt) || vmt < bg3le::load_bias()) {
        bg3le::logf("cache templates: system entry %d does not check out (own index %d)", *index, own);
        return 0;
    }
    const auto at = system + kTemplateSwitch;
    bg3le::RawMap m{};
    if (!read(at, &m) || m.KeysSize > m.KeysCapacity || m.KeysSize > (1u << 20)) {
        bg3le::logf("cache templates: ChangeSystem's TemplateSwitch does not read as a map");
        return 0;
    }
    return at;
}

std::uint32_t new_guid_string() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uint8_t b[16];
    for (int i = 0; i < 16; i += 8) {
        const std::uint64_t word = rng();
        std::memcpy(b + i, &word, 8);
    }
    b[6] = (std::uint8_t)((b[6] & 0x0f) | 0x40);
    b[8] = (std::uint8_t)((b[8] & 0x3f) | 0x80);
    char text[37];
    std::snprintf(text, sizeof(text),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
                  b[12], b[13], b[14], b[15]);
    std::uint32_t id = kNull;
    return bg3le_fixed_string_create(text, &id) ? id : kNull;
}

void set_fs(std::uintptr_t at, std::uint32_t id) {
    if (id != kNull) bg3le_fixed_string_pin(id);
    std::memcpy((void*)at, &id, 4);
}

// CacheTemplateManagerBase::CacheTemplate and RegisterTemplate.
std::uintptr_t cache_template(std::uintptr_t mgr, std::size_t sync, std::uintptr_t tmpl,
                              std::uint32_t templateId) {
    auto* manager = (CacheTemplateManagerBase*)mgr;
    std::uint8_t type = 0;
    read(mgr + 8, &type);
    const std::uint32_t handle = manager->GetNewIndex() | ((std::uint32_t)type << 29);

    std::uintptr_t vmt = 0, clone = 0, cloneVmt = 0, cloneFn = 0;
    if (!read(tmpl, &vmt) || !read(vmt + kVtSlotClone * 8, &cloneFn) || !code_ptr(cloneFn)) {
        bg3le::logf("cache templates: the template's Clone slot is not code");
        return 0;
    }
    clone = (std::uintptr_t)((GameObjectTemplate*)tmpl)->Clone();
    if (clone == 0 || !read(clone, &cloneVmt) || cloneVmt != vmt) {
        bg3le::logf("cache templates: Clone returned %#lx (vtable %#lx, expected %#lx)",
                    (unsigned long)clone, (unsigned long)cloneVmt, (unsigned long)vmt);
        return 0;
    }

    // The game requires a cache template to have a root template.
    std::uint32_t name = kNull, id = kNull;
    read(clone + kTemplateName, &name);
    read(clone + kId, &id);
    if (name == kNull) {
        set_fs(clone + kTemplateName, id);
        set_fs(clone + kParentId, kNull);
    }
    set_fs(clone + kId, templateId);

    bool ok = false;
    std::uint8_t enableSync = 0;
    {
        const EngineLock held(mgr, true);
        ok = bg3le::int_map_insert<std::uint32_t, std::uintptr_t>((void*)(mgr + kByHandle), handle, clone);
        std::memcpy((void*)(clone + kHandle), &handle, 4);
        ok = ok && bg3le::fs_map_insert<std::uintptr_t>((void*)(mgr + kTemplates), templateId, clone);
        ok = ok && bg3le::int_map_insert<std::uint32_t, std::uint32_t>((void*)(mgr + kRefCounts), handle, 0u);
        read(mgr + sync + 32, &enableSync);
        if (ok && enableSync == 1) ok = bg3le::array_append<std::uintptr_t>((void*)(mgr + sync), clone);
    }
    if (!ok) bg3le::logf("cache templates: registering %#x in the cache manager failed", handle);
    return ok ? clone : 0;
}

void inc_ref(std::uintptr_t mgr, std::uint32_t handle) {
    const EngineLock held(mgr, false);
    if (auto* refs = bg3le::int_map_find<std::uint32_t, std::uint32_t>((void*)(mgr + kRefCounts), handle)) {
        ++*refs;
    }
}

}  // namespace

// The item's template after caching it, or null; type is the clone's storage
// type. Root and global templates, as upstream's TryToCacheTemplate.
extern "C" void* bg3le_item_create_cache_template(void* container, void* itemPtr,
                                                  std::uint64_t entity) {
    auto* item = (bg3se::esv::Item*)itemPtr;
    const auto mgr = (std::uintptr_t)bg3le_templates_cache_manager();
    std::uintptr_t tmpl = 0, original = 0, mgrVmt = 0;
    std::uint64_t ownHandle = 0;
    if (item == nullptr || mgr == 0 || !read((std::uintptr_t)&item->Template, &tmpl) || tmpl == 0
        || !read((std::uintptr_t)&item->OriginalTemplate, &original)
        || !read((std::uintptr_t)&item->field_10, &ownHandle) || !read(mgr, &mgrVmt)) {
        bg3le::logf("cache templates: the item or the cache manager is not readable");
        return nullptr;
    }
    if (ownHandle != entity) {
        bg3le::logf("cache templates: esv::Item.field_10 is %#llx, not the entity %#llx",
                    (unsigned long long)ownHandle, (unsigned long long)entity);
        return nullptr;
    }

    std::uint32_t handle = 0, id = kNull, levelName = kNull;
    read(tmpl + kHandle, &handle);
    read(tmpl + kId, &id);
    read(tmpl + kLevelName, &levelName);
    const auto storage = (TemplateType)(handle >> 29);
    if (storage == TemplateType::CacheTemplate || storage == TemplateType::LevelCacheTemplate) {
        bg3le::logf("cache templates: cannot cache a template that already is one");
        return (void*)tmpl;
    }
    if (storage != TemplateType::RootTemplate && storage != TemplateType::GlobalTemplate) {
        bg3le::logf("cache templates: storage type %u is not supported yet", (unsigned)storage);
        return nullptr;
    }

    const std::size_t sync = sync_offset(mgrVmt);
    const std::uintptr_t switches = template_switch(container);
    if (sync == 0 || !plausible_array(mgr + sync) || !plausible_array(mgr + sync + 16)
        || switches == 0) {
        bg3le::logf("cache templates: the cache manager or ChangeSystem does not check out");
        return nullptr;
    }

    std::uintptr_t cached = 0;
    if (storage == TemplateType::GlobalTemplate) {
        // Only one cached copy of a global template can exist.
        const EngineLock held(mgr, false);
        std::vector<std::uint32_t> keys;
        bg3le::RawMap m{};
        if (read(mgr + kTemplates, &m) && m.KeysSize < (1u << 22)) {
            for (std::uint32_t i = 0; i < m.KeysSize; ++i) {
                std::uint32_t k = kNull;
                if (read((std::uintptr_t)(m.Keys + i), &k) && k == id) {
                    read((std::uintptr_t)m.Values + (std::uintptr_t)i * 8, &cached);
                    break;
                }
            }
        }
    }
    if (cached == 0) {
        const std::uint32_t templateId = storage == TemplateType::RootTemplate ? new_guid_string() : id;
        if (templateId == kNull) return nullptr;
        cached = cache_template(mgr, sync, tmpl, templateId);
        if (cached == 0) return nullptr;
    }
    if (cached == tmpl) return (void*)tmpl;

    // Root and global templates are not counted, so only the clone's count moves.
    std::uint32_t newHandle = 0, newId = kNull;
    read(cached + kHandle, &newHandle);
    read(cached + kId, &newId);
    inc_ref(mgr, newHandle);
    std::memcpy((void*)&item->Template, &cached, 8);

    struct {
        std::uint32_t TemplateId;
        std::uint8_t TemplateType;
        std::uint8_t Pad[3];
    } info{newId, (std::uint8_t)(newHandle >> 29), {}};
    static_assert(sizeof(info) == sizeof(bg3se::TemplateInfo));
    bg3le_fixed_string_pin(newId);
    if (!bg3le::int_map_insert<std::uint64_t, decltype(info)>((void*)switches, entity, info)) {
        bg3le::logf("cache templates: recording the template switch failed");
    }

    if (original == tmpl) {
        inc_ref(mgr, newHandle);
        std::memcpy((void*)&item->OriginalTemplate, &cached, 8);
    }
    return (void*)cached;
}
