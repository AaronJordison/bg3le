// Ext.Audio over the engine's Wwise manager: upstream's ClientAudio.inl (by
// Norbyte and the bg3se contributors), called by vtable slot.
//
// The manager is the object the resource manager holds whose vtable is the
// Wwise manager's (image+0x7a04ac0). Its slots were mapped by the Wwise
// function each one calls -- AK::SoundEngine keeps its symbols -- because
// the declared order is MSVC's: this build has one slot more before
// SetSwitch, and overloads (PostEvent, LoadEvent, ...) sit in a different
// order. SetSwitch and SetState are checked before anything is called.

#include <stdafx.h>

#include <GameDefinitions/Symbols.h>

#include <cstdint>
#include <cstring>

#include "../log.h"
#include "../targets.h"
#include "../mem.h"

extern "C" void* bg3le_ls_resource_manager();

namespace bg3le {
std::uintptr_t load_bias();
}

namespace {


enum Slot : int {
    SetSwitch = 20,
    SetState = 21,
    SetRTPCValue = 22,
    GetRTPCValue = 23,
    ResetRTPCValue = 24,
    StopSounds = 29,
    StopAllSounds = 30,
    PauseAllSounds = 35,
    ResumeAllSounds = 36,
    GetIDFromString = 44,
    LoadEvent = 58,      // by id; 56 and 57 are the by-name pair
    UnloadEvent = 59,
    LoadBank = 64,
    UnloadBank = 65,
    PrepareBank = 68,
    PostEventByName = 78,
    PlayExternalSound = 80,
};

// The built-in sound objects, one per player, and the music handle, as the
// live manager holds them (upstream's names, this build's offsets).
struct Builtin {
    char const* Name;
    std::size_t Offset;
};
constexpr Builtin kBuiltins[] = {
    {"ControllerSpeakerListener", 0x48}, {"Listener", 0x68}, {"RumbleListener", 0x88},
    {"PlayerEmitter", 0xa8}, {"Ambient", 0xc8}, {"HUD", 0xe8}, {"CineHUD", 0x108},
};
constexpr std::size_t kMusicHandle = 0x128;

template <class T>
T read(std::uintptr_t at) {
    T v{};
    bg3le::safe_read(reinterpret_cast<void const*>(at), &v, sizeof(T));
    return v;
}

void* manager() {
    static std::size_t offset = 0;
    auto rm = reinterpret_cast<std::uintptr_t>(bg3le_ls_resource_manager());
    if (rm == 0) return nullptr;
    const std::uintptr_t bias = bg3le::load_bias();
    auto matches = [&](std::uintptr_t p) {
        if (p < 0x10000 || read<std::uintptr_t>(p) != bias + bg3le::target::WwiseVtable()) return false;
        const auto vt = bias + bg3le::target::WwiseVtable();
        return read<std::uintptr_t>(vt + SetSwitch * 8) == bias + bg3le::target::WwiseSetSwitch()
               && read<std::uintptr_t>(vt + SetState * 8) == bias + bg3le::target::WwiseSetState();
    };
    if (offset != 0) {
        auto p = read<std::uintptr_t>(rm + offset);
        if (matches(p)) return reinterpret_cast<void*>(p);
    }
    for (std::size_t off = 0; off < 0x1000; off += 8) {
        auto p = read<std::uintptr_t>(rm + off);
        if (matches(p)) {
            if (offset != off) bg3le::logf("audio: Wwise manager at ResourceManager+%#zx", off);
            offset = off;
            return reinterpret_cast<void*>(p);
        }
    }
    return nullptr;
}

template <class R, class... A>
R call(void* mgr, int slot, A... args) {
    auto** vt = *reinterpret_cast<void***>(mgr);
    return reinterpret_cast<R (*)(void*, A...)>(vt[slot])(mgr, args...);
}

}  // namespace

extern "C" bool bg3le_audio_ready() { return manager() != nullptr; }

// A built-in sound object by upstream's name and player index.
extern "C" bool bg3le_audio_builtin(char const* name, unsigned player, std::uint64_t* out) {
    auto mgr = reinterpret_cast<std::uintptr_t>(manager());
    if (mgr == 0 || name == nullptr || player > 3) return false;
    if (std::strcmp(name, "Music") == 0) {
        *out = read<std::uint64_t>(mgr + kMusicHandle);
        return true;
    }
    for (auto const& b : kBuiltins) {
        if (std::strcmp(name, b.Name) == 0) {
            *out = read<std::uint64_t>(mgr + b.Offset + player * 8);
            return true;
        }
    }
    return false;
}

extern "C" bool bg3le_audio_set_switch(std::uint64_t obj, char const* group, char const* state) {
    void* m = manager();
    return m && call<bool>(m, SetSwitch, group, state, obj);
}

extern "C" bool bg3le_audio_set_state(char const* group, char const* state) {
    void* m = manager();
    return m && call<bool>(m, SetState, group, state);
}

extern "C" bool bg3le_audio_set_rtpc(std::uint64_t obj, char const* name, float value, bool bypass) {
    void* m = manager();
    return m && call<bool>(m, SetRTPCValue, obj, name, value, bypass);
}

extern "C" float bg3le_audio_get_rtpc(std::uint64_t obj, char const* name) {
    void* m = manager();
    return m ? call<float>(m, GetRTPCValue, obj, name) : 0.0f;
}

extern "C" void bg3le_audio_reset_rtpc(std::uint64_t obj, char const* name) {
    if (void* m = manager()) call<void>(m, ResetRTPCValue, obj, name);
}

extern "C" void bg3le_audio_stop(bool all, std::uint64_t obj) {
    void* m = manager();
    if (m == nullptr) return;
    if (all) {
        call<void>(m, StopAllSounds);
    } else {
        call<void>(m, StopSounds, obj, (std::uint32_t)0);
    }
}

extern "C" void bg3le_audio_pause_all(bool pause) {
    if (void* m = manager()) call<void>(m, pause ? PauseAllSounds : ResumeAllSounds);
}

extern "C" bool bg3le_audio_post_event(std::uint64_t obj, char const* name, float position) {
    void* m = manager();
    return m && call<bool>(m, PostEventByName, obj, name, position, false, (void*)nullptr);
}

extern "C" bool bg3le_audio_event(char const* name, bool load) {
    void* m = manager();
    if (m == nullptr) return false;
    const auto id = call<std::int32_t>(m, GetIDFromString, name);
    return call<bool>(m, load ? LoadEvent : UnloadEvent, id);
}

extern "C" bool bg3le_audio_play_external(std::uint64_t obj, char const* event, char const* path,
                                          std::uint8_t codec, float position) {
    void* m = manager();
    if (m == nullptr) return false;
    const bg3se::STDString lsPath = bg3se::GetStaticSymbols().ToPath(path, bg3se::PathRootType::Data);
    const auto id = call<std::int32_t>(m, GetIDFromString, event);
    return call<bool>(m, PlayExternalSound, obj, id, &lsPath, codec, position, false, (void*)nullptr);
}

// Upstream's four bank calls. Its UnprepareBank unloads, and so does this.
extern "C" bool bg3le_audio_bank(char const* name, int op) {
    void* m = manager();
    if (m == nullptr) return false;
    switch (op) {
    case 0:
    case 2: {
        std::int32_t id = -1;
        call<std::int32_t*>(m, op == 0 ? LoadBank : PrepareBank, &id, name);
        return id != -1;
    }
    default: {
        const auto id = call<std::int32_t>(m, GetIDFromString, name);
        return call<bool>(m, UnloadBank, id);
    }
    }
}
