// Client game state, for what upstream's ecl::ScriptExtender does with it:
// the menu line and the client's mod bootstraps as the game leaves
// LoadModule, GameStateChanged, and a client tick every frame.
//
// - The state is EoCClient->GameStateMachine->State, as upstream reads it.
// - LoadModule's Exit is ecl::GameStateLoadModule's vtable slot 7.
// - The per-frame call is the client's state-queue update, which EoCClient's
//   update calls once a frame; upstream's ticks come from the same place.
// - ecl::GameStateMachine::SetTargetState (it logs "ecl::GameStateQueued:
//   %s") is hooked only to log what is queued.

#include "game_state.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "hook.h"
#include "log.h"
#include "resolve.h"
#include "lua_host.h"
#include "mem.h"
#include "net.h"
#include "stackdump.h"
#include "targets.h"

namespace bg3le {

void translated_string_show_version(char const* suffix);
bool note_session_ended();

namespace {

// Addresses from targets.h: SetTargetState; ecl::GameStateLoadModule's vtable
// slot 7 (Exit) and what it holds (slot 4 is Enter, which starts the load; 5
// and 6 run every frame until it is done); ecl::GameStateMachine::Update, which
// runs the current state's per-frame slot and switches to the next queued state
// once a frame; and the state names, char const* names[35] in .data.rel.ro.
constexpr unsigned char kMachineUpdatePrologue[] = {
    0x41, 0x56, 0x53, 0x50, 0x80, 0x7f, 0x08, 0x00, 0x75, 0x08};
// names[] holds 35 on v4.76. The cap only bounds the index: each entry is
// checked to be a short printable name before it is used, so a table that
// changed length reads "Unknown", never a stray pointer.
constexpr int kStateLimit = 64;

constexpr std::size_t kStateId = 0x10;

// ecl::EoCClient* (target::EoCClient) and its GameStateMachine {CurrentState,
// State}: upstream's GetClientState. At +0x88 here, a field earlier than on Windows.
constexpr std::size_t kClientMachine = 0x88;
constexpr std::size_t kMachineState = 0x08;

using SetTargetStateProc = void (*)(void*, void*);
SetTargetStateProc g_set_target_state = nullptr;
// State methods may return a result -- Enter returns whether it succeeded,
// and dropping it made the engine reject the load order -- so arguments and
// result pass straight through.
using StateProc = std::uint64_t (*)(void*, void*, void*, void*);
StateProc g_load_module_exit = nullptr;
std::atomic<bool> g_left_load_module{false};

char const* state_name(std::uint32_t state) {
    if (state >= kStateLimit || target::StateNames() == 0) return "Unknown";
    auto* const* names = reinterpret_cast<char const* const*>(load_bias() + target::StateNames());
    char const* name = nullptr;
    char text[48];
    if (!safe_read(&names[state], &name, sizeof(name)) || name == nullptr
        || !safe_cstr(name, text, sizeof(text)) || text[0] == '\0') {
        return "Unknown";
    }
    for (char const* c = text; *c != '\0'; ++c) {
        if (*c < 0x20 || *c > 0x7e) return "Unknown";
    }
    return name;
}

bool state_id(void* state, std::uint32_t* out) {
    return state != nullptr
        && safe_read(static_cast<char*>(state) + kStateId, out, sizeof(*out));
}

// ecl::ScriptExtender::ShowVersionNumber, queued until the copyright
// string is in the repository.
void show_version_number() {
    std::string text = "\r\nbg3le loaded, Script Extender v32 API, built on " __DATE__
                       " " __TIME__ ".";
    if (log_previous_crash()[0] != '\0') {
        text += "\r\nThe last run crashed. Its log is ";
        text += log_previous_crash();
    }
    translated_string_show_version(text.c_str());
}

std::uint64_t load_module_exit_hook(void* state, void* a, void* b, void* c) {
    const std::uint64_t result = g_load_module_exit(state, a, b, c);
    if (!g_left_load_module.exchange(true)) {
        logf("gamestate: client left LoadModule");
        install_crash_handler();
        show_version_number();
        lua_load_client_scripts();
    }
    return result;
}

StateProc g_machine_update = nullptr;

// ---- auto-continue: queue LoadSession without a single input ------------------------
//
// The engine's own Continue path ends in GameStateMachine::SetTargetState(LoadSession).
// Rather than synthesise input (which needs window focus) or drive the menu, find the one
// state object whose vtable is LoadSession's and hand it to the machine directly. No keys,
// no dialogs, no focus. Enabled with BG3LE_AUTO_CONTINUE=1; the vtable offset is per build.
constexpr std::uintptr_t kLoadSessionVtable = 0x79e0b48;  // image+0x... on 25605617
std::atomic<bool> g_auto_continue_armed{true};

bool auto_continue_wanted() {
    static const bool on = std::getenv("BG3LE_AUTO_CONTINUE") != nullptr;
    return on;
}

// Scan this process's readable+writable mappings for a word equal to the state's vtable
// pointer; the object begins at that word. Bounded so a miss cannot stall the game.
void* find_state_object(std::uintptr_t vtable) {
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return nullptr;
    char line[512];
    std::size_t scanned = 0;
    constexpr std::size_t kBudget = 768ull * 1024 * 1024;
    while (std::fgets(line, sizeof line, maps) != nullptr && scanned < kBudget) {
        unsigned long long start = 0, end = 0;
        char perms[8] = {0};
        if (std::sscanf(line, "%llx-%llx %7s", &start, &end, perms) != 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;
        const std::size_t len = (std::size_t)(end - start);
        if (len == 0 || len > kBudget) continue;
        auto* p = reinterpret_cast<unsigned char const*>(start);
        scanned += len;
        for (std::size_t off = 0; off + 8 <= len; off += 8) {
            std::uintptr_t word = 0;
            std::memcpy(&word, p + off, sizeof word);
            if (word == vtable) { std::fclose(maps); return (void*)(start + off); }
        }
    }
    std::fclose(maps);
    return nullptr;
}

// Every frame is a client tick, and a state that differs from the last
// frame's is GameStateChanged -- upstream compares around its update call.
std::uint64_t machine_update_hook(void* machine, void* a, void* b, void* c) {
    static std::atomic<bool> first{true};
    if (first.exchange(false)) logf("gamestate: client frames are ticking");
    const std::uint64_t result = g_machine_update(machine, a, b, c);
    net_client_tick();

    static char const* last = nullptr;
    char const* now = client_game_state();
    if (auto_continue_wanted() && now != nullptr && std::strcmp(now, "Menu") == 0
        && g_auto_continue_armed.load() && g_set_target_state != nullptr) {
        g_auto_continue_armed.store(false);
        void* state = find_state_object(load_bias() + kLoadSessionVtable);
        if (state != nullptr) {
            logf("gamestate: auto-continue queueing LoadSession (%p) — no input", state);
            g_set_target_state(machine, state);
        } else {
            logf("gamestate: auto-continue could not find the LoadSession state object");
        }
    }

    static char const* last = nullptr;
    char const* now = client_game_state();
    if (now != nullptr && last != nullptr && now != last) {
        logf("gamestate: client %s -> %s", last, now);
        // Upstream's client resets on UnloadSession and loads again leaving
        // LoadMenu; its server resets there too and loads at the next
        // LoadSession, which is the story work here.
        if (std::strcmp(now, "UnloadSession") == 0 && note_session_ended()) {
            logf("gamestate: session unloaded; rebuilding the Lua states");
            lua_reset(false);
        }
        if (std::strcmp(last, "LoadMenu") == 0) {
            show_version_number();
            lua_load_client_scripts();
        }
        char const* from = last;
        last = now;
        lua_client_tick(from, now);
    } else {
        if (now != nullptr) last = now;
        lua_client_tick(nullptr, nullptr);
    }
    return result;
}

void set_target_state_hook(void* machine, void* state) {
    std::uint32_t to = 0;
    if (state_id(state, &to)) {
        void* vtable = *static_cast<void**>(state);
        logf("gamestate: client queued %s (vtable image+%#lx)", state_name(to),
             (unsigned long)(reinterpret_cast<std::uintptr_t>(vtable) - load_bias()));
    }
    g_set_target_state(machine, state);
}

}  // namespace

char const* client_state_name() {
    char const* state = client_game_state();
    return state != nullptr ? state : "unknown";
}

char const* client_game_state() {
    void* client = nullptr;
    void* machine = nullptr;
    std::uint32_t state = 0;
    if (target::EoCClient() == 0
        || !safe_read(reinterpret_cast<void*>(load_bias() + target::EoCClient()), &client,
                   sizeof(client))
        || client == nullptr
        || !safe_read(static_cast<char*>(client) + kClientMachine, &machine,
                      sizeof(machine))
        || machine == nullptr
        || !safe_read(static_cast<char*>(machine) + kMachineState, &state,
                      sizeof(state))) {
        return nullptr;
    }
    return state_name(state);
}

void install_game_state_hook() {
    void* original = nullptr;
    if (hook_slot(target::LoadModuleExitSlot(), target::LoadModuleExit(),
                  reinterpret_cast<void*>(&load_module_exit_hook), &original)) {
        g_load_module_exit = reinterpret_cast<StateProc>(original);
    } else {
        logf("gamestate: LoadModule's Exit is not where it was; the menu line "
             "and early client mods are off");
    }

    const std::uintptr_t machine_update = target::MachineUpdate();
    if (code_near(machine_update, kMachineUpdatePrologue) != 0
        && hook_call_sites(machine_update,
                           reinterpret_cast<void*>(&machine_update_hook),
                           &original) > 0) {
        g_machine_update = reinterpret_cast<StateProc>(original);
    } else {
        logf("gamestate: ecl::GameStateMachine::Update not found; the client "
             "ticks with the server");
    }

    // Its prologue loads a global rip-relatively, so the bytes differ in every
    // build; the target's pattern wildcards that.
    const std::uintptr_t set_target_state = target::SetTargetState();
    if (set_target_state == 0) {
        logf("gamestate: ecl::GameStateMachine::SetTargetState not found");
        return;
    }
    if (hook_call_sites(set_target_state,
                        reinterpret_cast<void*>(&set_target_state_hook),
                        &original) > 0) {
        g_set_target_state = reinterpret_cast<SetTargetStateProc>(original);
    }
}

}  // namespace bg3le
