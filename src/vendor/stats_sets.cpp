// Ext.Stats' submodules over the engine's own managers: RPGStats'
// TreasureTables and TreasureCategories, its EquipmentSet, SpellSet,
// ItemProgression and ItemCombination managers. The functions, their shapes
// and the serializer format are upstream's (Lua/Libs/Stats.inl and
// Lua/LuaSerializers.cpp, by Norbyte and the bg3se contributors) -- thank you.
//
// Every manager and element layout here was read back from the live game
// before being trusted; see field_280_bg3le in the vendored Stats.h. Writes
// build fresh engine allocations and leave the old ones where they are.

#include <stdafx.h>

#include <GameDefinitions/Stats/Stats.h>

#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "../log.h"
#include "../mem.h"
#include "../stats_sets.h"
#include "engine_containers.h"

extern "C" void* bg3le_rpgstats();
extern "C" char const* bg3le_fixed_string(std::uint32_t index, std::uint32_t* length);
extern "C" bool bg3le_fixed_string_index_of(char const* wanted, std::uint32_t* out);
extern "C" bool bg3le_fixed_string_intern(char const* text, std::uint32_t* out);
extern "C" bool bg3le_fixed_string_hash(std::uint32_t id, std::uint32_t* out);

namespace bg3le::stats_sets {
namespace {

using bg3se::stats::RPGStats;
namespace st = bg3se::stats;
using Manager = bg3se::stats::CNamedElementManager<st::SpellSet>;

static_assert(offsetof(Manager, Values) == 0x08);
static_assert(offsetof(Manager, NameToHandle) == 0x18);
static_assert(offsetof(Manager, NextHandle) == 0x58);
static_assert(offsetof(st::ItemCombinationManager, PreviewData) == 0x78);
static_assert(offsetof(st::ItemCombinationManager, Properties) == 0x88);
static_assert(offsetof(st::ItemProgressionManager, NameGroups) == 0x18);
static_assert(sizeof(st::TreasureSubTable) == 0x58);
static_assert(sizeof(st::TreasureSubTableCategory) == 0x28);
static_assert(sizeof(st::TreasureCategoryItem) == 0x20);
static_assert(sizeof(DropCount) == sizeof(st::TreasureSubTableDropCount));

constexpr std::uint32_t kNull = 0xffffffffu;

template <class T>
T read(void const* at) {
    T v{};
    safe_read(at, &v, sizeof(T));
    return v;
}

template <class T>
void put(void* at, T value) {
    std::memcpy(at, &value, sizeof(T));
}

char* rpg() { return static_cast<char*>(bg3le_rpgstats()); }

std::string text(std::uint32_t id) {
    if (id == kNull) return {};
    std::uint32_t len = 0;
    char const* s = bg3le_fixed_string(id, &len);
    return s != nullptr ? std::string(s, len) : std::string();
}

bool id_of(char const* name, std::uint32_t* out, bool intern) {
    if (name == nullptr || name[0] == '\0') {
        *out = kNull;
        return true;
    }
    if (bg3le_fixed_string_index_of(name, out)) return true;
    return intern && bg3le_fixed_string_intern(name, out);
}

// The named-element manager behind a kind, or null for the map-backed ones.
char* manager(Kind kind) {
    char* r = rpg();
    if (r == nullptr) return nullptr;
    switch (kind) {
    case Kind::TreasureTable: return r + offsetof(RPGStats, TreasureTables);
    case Kind::TreasureCategory: return r + offsetof(RPGStats, TreasureCategories);
    case Kind::EquipmentSet: return read<char*>(r + offsetof(RPGStats, EquipmentSetManager));
    case Kind::SpellSet: return read<char*>(r + offsetof(RPGStats, SpellSetManager));
    case Kind::ItemCombo: return read<char*>(r + offsetof(RPGStats, ItemCombinationManager));
    default: return nullptr;
    }
}

// A LegacyMap or LegacyRefMap<FixedString, T*>, and whether it is the Ref one.
char* legacy_map(Kind kind, bool* ref) {
    char* r = rpg();
    if (r == nullptr) return nullptr;
    *ref = kind == Kind::ItemComboPreview || kind == Kind::ItemComboProperty;
    if (*ref) {
        char* combos = read<char*>(r + offsetof(RPGStats, ItemCombinationManager));
        if (combos == nullptr) return nullptr;
        return combos + (kind == Kind::ItemComboPreview
                             ? offsetof(st::ItemCombinationManager, PreviewData)
                             : offsetof(st::ItemCombinationManager, Properties));
    }
    char* prog = read<char*>(r + offsetof(RPGStats, ItemProgressionManager));
    if (prog == nullptr) return nullptr;
    return prog + (kind == Kind::ItemGroup ? offsetof(st::ItemProgressionManager, ItemGroups)
                                           : offsetof(st::ItemProgressionManager, NameGroups));
}

struct LegacyHeader {
    std::uint32_t HashSize = 0;
    void** HashTable = nullptr;
    std::uint32_t ItemCount = 0;
};

bool legacy_header(char* map, bool ref, LegacyHeader* h) {
    if (map == nullptr) return false;
    if (ref) {
        h->ItemCount = read<std::uint32_t>(map);
        h->HashSize = read<std::uint32_t>(map + 4);
        h->HashTable = read<void**>(map + 8);
    } else {
        h->HashSize = read<std::uint32_t>(map);
        h->HashTable = read<void**>(map + 8);
        h->ItemCount = read<std::uint32_t>(map + 16);
    }
    return h->HashSize > 0 && h->HashSize < (1u << 20) && h->HashTable != nullptr;
}

// Each node of a legacy map: MapNode {Next, Key, Value}.
template <class Fn>
void each_node(char* map, bool ref, Fn fn) {
    LegacyHeader h;
    if (!legacy_header(map, ref, &h)) return;
    for (std::uint32_t b = 0; b < h.HashSize; ++b) {
        char* node = read<char*>(h.HashTable + b);
        for (std::uint32_t guard = 0; node != nullptr && guard < (1u << 20); ++guard) {
            if (!fn(b, read<std::uint32_t>(node + 8), read<void*>(node + 16))) return;
            node = read<char*>(node);
        }
    }
}

std::vector<void*> manager_values(char* mgr) {
    std::vector<void*> out;
    if (mgr == nullptr) return out;
    auto a = read<RawArray>(mgr + offsetof(Manager, Values));
    if (a.Buffer == nullptr || a.Size > (1u << 22)) return out;
    out.resize(a.Size);
    if (!safe_read(a.Buffer, out.data(), out.size() * sizeof(void*))) out.clear();
    return out;
}

// Element name: the first member of every named-element type here.
std::uint32_t name_of(void const* element) {
    return element != nullptr ? read<std::uint32_t>(element) : kNull;
}

// Which hash a legacy map buckets its FixedString keys by: the string
// table's, or the index itself. -1 if its nodes agree with neither.
int legacy_hash_rule(char* map, bool ref) {
    bool byHash = true, byIndex = true, any = false;
    LegacyHeader h;
    if (!legacy_header(map, ref, &h)) return -1;
    each_node(map, ref, [&](std::uint32_t bucket, std::uint32_t key, void*) {
        any = true;
        std::uint32_t hash = 0;
        if (!bg3le_fixed_string_hash(key, &hash) || hash % h.HashSize != bucket) byHash = false;
        if (key % h.HashSize != bucket) byIndex = false;
        return byHash || byIndex;
    });
    if (!any) return 0;  // empty: the string table's, as the vendored Hash does
    return byHash ? 0 : byIndex ? 1 : -1;
}

// Puts value under key in a legacy map, replacing an existing entry.
bool legacy_insert(char* map, bool ref, std::uint32_t key, void* value, std::string* why) {
    LegacyHeader h;
    if (!legacy_header(map, ref, &h)) {
        *why = "the map is not where this build has it";
        return false;
    }
    // An existing entry is overwritten in place.
    bool replaced = false;
    for (std::uint32_t b = 0; b < h.HashSize && !replaced; ++b) {
        char* node = read<char*>(h.HashTable + b);
        for (std::uint32_t guard = 0; node != nullptr && guard < (1u << 20); ++guard) {
            if (read<std::uint32_t>(node + 8) == key) {
                put<void*>(node + 16, value);
                replaced = true;
                break;
            }
            node = read<char*>(node);
        }
    }
    if (replaced) return true;

    const int rule = legacy_hash_rule(map, ref);
    if (rule < 0) {
        *why = "the map does not bucket its keys the way bg3le reads them";
        return false;
    }
    std::uint32_t hash = key;
    if (rule == 0 && !bg3le_fixed_string_hash(key, &hash)) {
        *why = "the name has no string-table hash";
        return false;
    }
    const std::uint32_t bucket = hash % h.HashSize;
    auto* node = static_cast<char*>(bg3se::GameAllocRaw(24));
    if (node == nullptr) {
        *why = "out of memory";
        return false;
    }
    std::memset(node, 0, 24);
    put<void*>(node, read<void*>(h.HashTable + bucket));
    put<std::uint32_t>(node + 8, key);
    put<void*>(node + 16, value);
    put<void*>(h.HashTable + bucket, node);
    put<std::uint32_t>(map + (ref ? 0 : 16), h.ItemCount + 1);
    return true;
}

// Upstream's CNamedElementManager::Insert, never freeing the element it
// replaces.
bool manager_insert(char* mgr, void* element, std::string* why) {
    const std::uint32_t name = name_of(element);
    auto values = manager_values(mgr);
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (name_of(values[i]) == name) {
            auto a = read<RawArray>(mgr + offsetof(Manager, Values));
            put<void*>(static_cast<char*>(a.Buffer) + i * sizeof(void*), element);
            return true;
        }
    }
    const auto next = read<std::int32_t>(mgr + offsetof(Manager, NextHandle));
    if (next != (std::int32_t)values.size()) {
        *why = "the manager's handle count does not match its values";
        return false;
    }
    auto before = read<RawArray>(mgr + offsetof(Manager, Values));
    if (!array_append<void*>(mgr + offsetof(Manager, Values), element)) {
        *why = "could not grow the manager's values";
        return false;
    }
    if (!fs_map_insert<std::int32_t>(mgr + offsetof(Manager, NameToHandle), name, next)) {
        std::memcpy(mgr + offsetof(Manager, Values), &before, sizeof(before));
        *why = "the manager's name map does not hash the way bg3le reads it";
        return false;
    }
    put<std::int32_t>(mgr + offsetof(Manager, NextHandle), next + 1);
    return true;
}

template <class T>
T* make_named(std::uint32_t name) {
    auto* e = bg3se::GameAlloc<T>();
    if (e != nullptr) std::memcpy(e, &name, sizeof(name));
    return e;
}

// A fresh engine array of n elements of T, or an empty one.
template <class T>
bool fill_array(void* header, std::vector<T> const& items) {
    RawArray a{nullptr, 0, 0};
    if (!items.empty()) {
        a.Buffer = bg3se::GameAllocRaw(items.size() * sizeof(T));
        if (a.Buffer == nullptr) return false;
        std::memcpy(a.Buffer, items.data(), items.size() * sizeof(T));
        a.Capacity = a.Size = (std::uint32_t)items.size();
    }
    std::memcpy(header, &a, sizeof(a));
    return true;
}

}  // namespace

char const* type_name(Kind kind) {
    switch (kind) {
    case Kind::SpellSet: return "stats::SpellSet";
    case Kind::EquipmentSet: return "stats::EquipmentSet";
    case Kind::TreasureTable: return "stats::TreasureTable";
    case Kind::TreasureCategory: return "stats::TreasureCategory";
    case Kind::ItemCombo: return "stats::ItemCombination";
    case Kind::ItemComboPreview: return "stats::ItemCombinationPreviewData";
    case Kind::ItemComboProperty: return "stats::ItemCombinationProperty";
    case Kind::ItemGroup: return "stats::ItemGroup";
    case Kind::NameGroup: return "stats::NameGroup";
    }
    return "";
}

std::vector<void*> all(Kind kind) {
    if (char* mgr = manager(kind)) return manager_values(mgr);
    std::vector<void*> out;
    bool ref = false;
    each_node(legacy_map(kind, &ref), ref, [&](std::uint32_t, std::uint32_t, void* v) {
        if (v != nullptr) out.push_back(v);
        return true;
    });
    return out;
}

void* get(Kind kind, char const* name) {
    std::uint32_t id = 0;
    if (!id_of(name, &id, true) || id == kNull) return nullptr;
    if (char* mgr = manager(kind)) {
        for (void* e : manager_values(mgr)) {
            if (name_of(e) == id) return e;
        }
        return nullptr;
    }
    void* found = nullptr;
    bool ref = false;
    each_node(legacy_map(kind, &ref), ref, [&](std::uint32_t, std::uint32_t k, void* v) {
        if (k == id) found = v;
        return found == nullptr;
    });
    return found;
}

void* create(Kind kind, char const* name, std::string* why) {
    std::uint32_t id = 0;
    if (!id_of(name, &id, true) || id == kNull) {
        *why = "the name could not be interned";
        return nullptr;
    }
    void* element = nullptr;
    switch (kind) {
    case Kind::SpellSet: element = make_named<st::SpellSet>(id); break;
    case Kind::EquipmentSet: element = make_named<st::EquipmentSet>(id); break;
    case Kind::ItemCombo: element = make_named<st::ItemCombination>(id); break;
    case Kind::ItemComboPreview: element = make_named<st::ItemCombinationPreviewData>(id); break;
    case Kind::ItemComboProperty: element = make_named<st::ItemCombinationProperty>(id); break;
    default:
        *why = "upstream has no Create for this kind";
        return nullptr;
    }
    if (element == nullptr) {
        *why = "out of memory";
        return nullptr;
    }
    bool ok = false;
    if (char* mgr = manager(kind)) {
        ok = manager_insert(mgr, element, why);
    } else {
        bool ref = false;
        ok = legacy_insert(legacy_map(kind, &ref), ref, id, element, why);
    }
    if (!ok) return nullptr;
    logf("stats: created %s %s", type_name(kind), name);
    return element;
}

std::vector<std::string> rarities() {
    std::vector<std::string> out;
    char* r = rpg();
    if (r == nullptr) return out;
    for (int i = 0; i < 7; ++i) {
        out.push_back(text(read<std::uint32_t>(r + offsetof(RPGStats, TreasureRarities) + i * 4)));
    }
    return out;
}

bool read_table(char const* name, Table* out) {
    auto* t = static_cast<char*>(get(Kind::TreasureTable, name));
    if (t == nullptr) return false;
    auto tables = manager_values(manager(Kind::TreasureTable));
    auto categories = manager_values(manager(Kind::TreasureCategory));

    out->Name = text(name_of(t));
    out->MinLevel = read<int>(t + offsetof(st::TreasureTable, MinLevel));
    out->MaxLevel = read<int>(t + offsetof(st::TreasureTable, MaxLevel));
    out->IgnoreLevelDiff = read<bool>(t + offsetof(st::TreasureTable, IgnoreLevelDiff));
    out->UseTreasureGroupContainers = read<bool>(t + offsetof(st::TreasureTable, UseTreasureGroupContainers));
    out->CanMerge = read<bool>(t + offsetof(st::TreasureTable, CanMerge));

    auto subs = read<RawArray>(t + offsetof(st::TreasureTable, SubTables));
    for (std::uint32_t s = 0; s < subs.Size && s < 4096; ++s) {
        auto* sub = read<char*>(static_cast<char*>(subs.Buffer) + s * sizeof(void*));
        if (sub == nullptr) continue;
        SubTable st_out;
        st_out.TotalCount = read<int>(sub + offsetof(st::TreasureSubTable, TotalCount));
        st_out.StartLevel = read<int>(sub + offsetof(st::TreasureSubTable, StartLevel));
        st_out.EndLevel = read<int>(sub + offsetof(st::TreasureSubTable, EndLevel));

        auto cats = read<RawArray>(sub + offsetof(st::TreasureSubTable, Categories));
        for (std::uint32_t c = 0; c < cats.Size && c < 4096; ++c) {
            auto* cat = read<char*>(static_cast<char*>(cats.Buffer) + c * sizeof(void*));
            if (cat == nullptr) continue;
            CategoryRef ref;
            auto v = read<st::TreasureSubTableCategory>(cat);
            ref.Frequency = v.Frequency;
            ref.IsTable = v.IsTreasureTable;
            auto const& pool = ref.IsTable ? tables : categories;
            if (v.Index >= 0 && (std::size_t)v.Index < pool.size()) ref.Name = text(name_of(pool[v.Index]));
            for (int i = 0; i < 7; ++i) ref.Frequencies[i] = v.Frequencies[i];
            st_out.Categories.push_back(ref);
        }

        auto drops = read<RawArray>(sub + offsetof(st::TreasureSubTable, DropCounts));
        for (std::uint32_t d = 0; d < drops.Size && d < 4096; ++d) {
            st_out.DropCounts.push_back(read<DropCount>(static_cast<char*>(drops.Buffer) + d * sizeof(DropCount)));
        }
        out->SubTables.push_back(std::move(st_out));
    }
    return true;
}

bool read_category(char const* name, Category* out) {
    auto* c = static_cast<char*>(get(Kind::TreasureCategory, name));
    if (c == nullptr) return false;
    out->Name = text(name_of(c));
    // Vector<TreasureCategoryItem*>: begin, end, capacity.
    auto* begin = read<char**>(c + offsetof(st::TreasureCategory, Items));
    auto* end = read<char**>(c + offsetof(st::TreasureCategory, Items) + 8);
    const std::size_t n = (begin != nullptr && end >= begin) ? (std::size_t)(end - begin) : 0;
    for (std::size_t i = 0; i < n && i < (1u << 20); ++i) {
        auto* item = read<char*>(begin + i);
        if (item == nullptr) continue;
        using I = st::TreasureCategoryItem;
        CategoryItem ci;
        ci.Name = text(read<std::uint32_t>(item + offsetof(I, Name)));
        ci.Priority = read<int>(item + offsetof(I, Priority));
        ci.MinAmount = read<int>(item + offsetof(I, MinAmount));
        ci.MaxAmount = read<int>(item + offsetof(I, MaxAmount));
        ci.ActPart = read<int>(item + offsetof(I, ActPart));
        ci.Unique = read<int>(item + offsetof(I, Unique));
        ci.MinLevel = read<int>(item + offsetof(I, MinLevel));
        ci.MaxLevel = read<int>(item + offsetof(I, MaxLevel));
        out->Items.push_back(std::move(ci));
    }
    return true;
}

bool update_table(Table const& in, std::string* why) {
    std::uint32_t nameId = 0;
    if (!id_of(in.Name.c_str(), &nameId, true) || nameId == kNull) {
        *why = "the table's Name could not be interned";
        return false;
    }
    auto tables = manager_values(manager(Kind::TreasureTable));
    auto categories = manager_values(manager(Kind::TreasureCategory));
    auto handle_of = [](std::vector<void*> const& pool, std::string const& n) -> int {
        std::uint32_t id = 0;
        if (!id_of(n.c_str(), &id, true) || id == kNull) return -1;
        for (std::size_t i = 0; i < pool.size(); ++i) {
            if (name_of(pool[i]) == id) return (int)i;
        }
        return -1;
    };

    // Everything is built before anything is swapped in, so an unknown
    // category leaves the table as it was.
    std::vector<void*> subs;
    for (auto const& s : in.SubTables) {
        auto* sub = bg3se::GameAlloc<st::TreasureSubTable>();
        if (sub == nullptr) {
            *why = "out of memory";
            return false;
        }
        std::vector<void*> cats;
        std::vector<std::int32_t> freqs;
        int total = 0;
        for (auto const& c : s.Categories) {
            const int index = handle_of(c.IsTable ? tables : categories, c.Name);
            if (index < 0) {
                *why = std::string(c.IsTable ? "Treasure table '" : "Treasure category '") + c.Name
                       + "' does not exist!";
                return false;
            }
            auto* cat = bg3se::GameAlloc<st::TreasureSubTableCategory>();
            if (cat == nullptr) {
                *why = "out of memory";
                return false;
            }
            std::memset(cat, 0, sizeof(*cat));
            cat->Index = index;
            cat->Frequency = c.Frequency;
            for (int i = 0; i < 7; ++i) cat->Frequencies[i] = c.Frequencies[i];
            cat->IsTreasureTable = cat->IsTreasureTable2 = c.IsTable;
            cats.push_back(cat);
            freqs.push_back(c.Frequency);
            total += c.Frequency;
        }
        std::vector<DropCount> drops = s.DropCounts;
        std::vector<std::int32_t> amounts;
        int count = s.TotalCount;
        // Upstream's rule: a negative TotalCount with no drops is a guaranteed drop.
        if (!drops.empty() || count > 0) {
            count = 0;
            for (auto const& d : drops) count += d.Amount;
        }
        for (auto const& d : drops) amounts.push_back(d.Amount);

        char* raw = reinterpret_cast<char*>(sub);
        if (!fill_array(raw + offsetof(st::TreasureSubTable, Categories), cats)
            || !fill_array(raw + offsetof(st::TreasureSubTable, CategoryFrequencies), freqs)
            || !fill_array(raw + offsetof(st::TreasureSubTable, DropCounts), drops)
            || !fill_array(raw + offsetof(st::TreasureSubTable, Amounts), amounts)) {
            *why = "out of memory";
            return false;
        }
        sub->TotalFrequency = total;
        sub->TotalCount = count;
        sub->StartLevel = s.StartLevel;
        sub->EndLevel = s.EndLevel;
        subs.push_back(sub);
    }

    auto* table = static_cast<char*>(get(Kind::TreasureTable, in.Name.c_str()));
    const bool isNew = table == nullptr;
    if (isNew) {
        table = reinterpret_cast<char*>(make_named<st::TreasureTable>(nameId));
        if (table == nullptr) {
            *why = "out of memory";
            return false;
        }
    }
    put<int>(table + offsetof(st::TreasureTable, MinLevel), in.MinLevel);
    put<int>(table + offsetof(st::TreasureTable, MaxLevel), in.MaxLevel);
    put<bool>(table + offsetof(st::TreasureTable, IgnoreLevelDiff), in.IgnoreLevelDiff);
    put<bool>(table + offsetof(st::TreasureTable, UseTreasureGroupContainers), in.UseTreasureGroupContainers);
    put<bool>(table + offsetof(st::TreasureTable, CanMerge), in.CanMerge);
    if (!fill_array(table + offsetof(st::TreasureTable, SubTables), subs)) {
        *why = "out of memory";
        return false;
    }
    if (isNew && !manager_insert(manager(Kind::TreasureTable), table, why)) return false;
    return true;
}

bool update_category(char const* name, Category const& in, std::string* why) {
    std::uint32_t nameId = 0;
    if (!id_of(name, &nameId, true) || nameId == kNull) {
        *why = "the category's name could not be interned";
        return false;
    }
    std::vector<void*> items;
    for (auto const& i : in.Items) {
        auto* item = bg3se::GameAlloc<st::TreasureCategoryItem>();
        std::uint32_t itemName = 0;
        if (item == nullptr || !id_of(i.Name.c_str(), &itemName, true)) {
            *why = "out of memory";
            return false;
        }
        std::memset(item, 0, sizeof(*item));
        std::memcpy(&item->Name, &itemName, sizeof(itemName));
        item->Priority = i.Priority;
        item->MinAmount = i.MinAmount;
        item->MaxAmount = i.MaxAmount;
        item->ActPart = i.ActPart;
        item->Unique = i.Unique;
        item->MinLevel = i.MinLevel;
        item->MaxLevel = i.MaxLevel;
        items.push_back(item);
    }
    void** buffer = nullptr;
    if (!items.empty()) {
        buffer = static_cast<void**>(bg3se::GameAllocRaw(items.size() * sizeof(void*)));
        if (buffer == nullptr) {
            *why = "out of memory";
            return false;
        }
        std::memcpy(buffer, items.data(), items.size() * sizeof(void*));
    }

    auto* category = static_cast<char*>(get(Kind::TreasureCategory, name));
    const bool isNew = category == nullptr;
    if (isNew) {
        category = reinterpret_cast<char*>(make_named<st::TreasureCategory>(nameId));
        if (category == nullptr) {
            *why = "out of memory";
            return false;
        }
    }
    char* vec = category + offsetof(st::TreasureCategory, Items);
    put<void**>(vec, buffer);
    put<void**>(vec + 8, buffer + items.size());
    put<void**>(vec + 16, buffer + items.size());
    if (isNew && !manager_insert(manager(Kind::TreasureCategory), category, why)) return false;
    return true;
}

}  // namespace bg3le::stats_sets
