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

// src/vendor/stats.cpp: whether the stats array has been (re)built since the last
// StatsLoaded fire, per context, without recording it.
extern "C" bool bg3le_stats_pending(bool client);

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

// ---- auto-continue: queue LoadSession without a single input -----------------------
//
// The engine's own Continue ends in ecl::GameStateMachine::SetTargetState(machine,
// state), where state is an engine-owned object this shim neither creates nor owns.
// The v1 approach scanned the address space for the state's vtable word -- it stalled
// on a mapped device and was never proven. This one derives the object from evidence
// the hooks already hold, and verifies it before use:
//
// - every state the engine queues through SetTargetState is a (id -> object) sample;
// - state objects are constructed together, so samples fit obj = base + id*stride and
//   LoadSession's object reads off that same line;
// - a candidate is accepted only when its vtable pointer sits inside the game image
//   and its id field (+0x10) equals LoadSession's id -- the same two facts every
//   state_name/state_id read in this file already relies on;
// - when no two samples fit a line, a bounded cluster probe around a known state
//   object walks +/-256 KiB of heap for the same signature.
//
// BG3LE_AUTO_CONTINUE=1 arms it (fires once, the next time the client settles in
// Menu); BG3LE_STATE_DEBUG=1 logs the evidence either way, so one boot's log says
// what was seen, derived, and fired.

bool state_debug_wanted() {
    static const bool on = std::getenv("BG3LE_STATE_DEBUG") != nullptr;
    return on;
}

bool auto_continue_wanted() {
    static const bool on = std::getenv("BG3LE_AUTO_CONTINUE") != nullptr;
    return on;
}

// First queue of each distinct state id wins: a later queue may carry an object from
// an older generation (a second Continue after a return to menu), and the line fit
// must not mix generations.
std::atomic<void*> g_queued_state[kStateLimit];

void note_queued_state(std::uint32_t id, void* obj) {
    if (id >= kStateLimit || obj == nullptr) return;
    void* expected = nullptr;
    g_queued_state[id].compare_exchange_strong(expected, obj, std::memory_order_relaxed);
}

bool machine_state_id(void* machine, std::uint32_t* out) {
    return machine != nullptr
        && safe_read(static_cast<char*>(machine) + kMachineState, out, sizeof(*out));
}

// A candidate state object: a vtable pointer inside the game image and an id field
// that equals want_id. Cheap, and the same checks state_name() trusts.
bool looks_like_state_object(void* obj, std::uint32_t want_id) {
    if (obj == nullptr) return false;
    void* vtable = nullptr;
    if (!safe_read(obj, &vtable, sizeof(vtable)) || vtable == nullptr) return false;
    const std::uintptr_t bias = load_bias();
    const auto vt = reinterpret_cast<std::uintptr_t>(vtable);
    if (bias == 0 || vt < bias || vt - bias > (std::uintptr_t{1} << 32)) return false;
    std::uint32_t id = 0;
    return state_id(obj, &id) && id == want_id;
}

// The machine's own current state object: a pointer in its first 0x40 bytes whose
// target looks like a state object of the machine's current id. The anchor when no
// state has been queued through SetTargetState yet.
void* current_state_object(void* machine, std::uint32_t id) {
    std::uint64_t words[8] = {};
    if (!safe_read(machine, words, sizeof(words))) return nullptr;
    for (unsigned w = 0; w < sizeof(words) / sizeof(words[0]); ++w) {
        auto* candidate = reinterpret_cast<void*>(words[w]);
        if (looks_like_state_object(candidate, id)) return candidate;
    }
    return nullptr;
}

int state_id_by_name(char const* want) {
    for (std::uint32_t id = 0; id < kStateLimit; ++id) {
        char const* name = state_name(id);
        if (std::strcmp(name, want) == 0) return static_cast<int>(id);
    }
    return -1;
}

// Fit obj = base + id*stride through two samples, then read LoadSession off the line.
void* state_object_from_samples(std::uint32_t want_id) {
    for (int a = 0; a < kStateLimit; ++a) {
        void* oa = g_queued_state[a].load(std::memory_order_relaxed);
        if (oa == nullptr) continue;
        for (int b = a + 1; b < kStateLimit; ++b) {
            void* ob = g_queued_state[b].load(std::memory_order_relaxed);
            if (ob == nullptr) continue;
            const auto diff = reinterpret_cast<std::intptr_t>(ob)
                            - reinterpret_cast<std::intptr_t>(oa);
            const auto did = static_cast<std::intptr_t>(b - a);
            if (diff <= 0 || diff % did != 0) continue;
            const auto stride = diff / did;
            if (stride % static_cast<std::intptr_t>(alignof(void*)) != 0) continue;
            const auto base = reinterpret_cast<std::intptr_t>(oa)
                            - static_cast<std::intptr_t>(a) * stride;
            auto* candidate = reinterpret_cast<void*>(
                base + static_cast<std::intptr_t>(want_id) * stride);
            if (looks_like_state_object(candidate, want_id)) return candidate;
        }
    }
    return nullptr;
}

// Bounded heap walk around a known state object. process_vm_readv (safe_read) cannot
// fault or stall whatever it touches, so the window bounds cost, not danger.
void* state_object_from_cluster(void* anchor, std::uint32_t want_id) {
    if (anchor == nullptr) return nullptr;
    constexpr std::uintptr_t kWindow = std::uintptr_t{1} << 18;  // 256 KiB each way
    const std::uintptr_t bias = load_bias();
    const auto page = reinterpret_cast<std::uintptr_t>(anchor) & ~std::uintptr_t{0xFFF};
    for (std::uintptr_t lo = page - kWindow; lo < page + kWindow; lo += 4096) {
        unsigned char block[4096];
        const std::size_t got =
            safe_read_some(reinterpret_cast<void*>(lo), block, sizeof(block));
        for (std::size_t off = 0; off + sizeof(std::uintptr_t) <= got;
             off += alignof(std::uintptr_t)) {
            std::uintptr_t word = 0;
            std::memcpy(&word, block + off, sizeof(word));
            if (bias == 0 || word < bias || word - bias > (std::uintptr_t{1} << 32)) {
                continue;
            }
            auto* candidate = reinterpret_cast<void*>(lo + off);
            if (candidate == anchor) continue;
            if (looks_like_state_object(candidate, want_id)) return candidate;
        }
    }
    return nullptr;
}

std::atomic<bool> g_auto_continue_done{false};
std::atomic<unsigned> g_menu_frames{0};
std::atomic<unsigned> g_auto_continue_attempts{0};

// Called from machine_update_hook after the engine's own Update returned: between
// frames is where the engine queues input-driven transitions anyway.
void maybe_auto_continue(void* machine, char const* now) {
    if (!auto_continue_wanted() || g_auto_continue_done.load(std::memory_order_relaxed)) {
        return;
    }
    if (machine == nullptr || now == nullptr || std::strcmp(now, "Menu") != 0) return;
    if (g_set_target_state == nullptr) return;

    // Dwell past the menu's first frames: Menu is still entering (UI building, the
    // splash up), and queueing a state mid-enter is the kind of race a test loop
    // cannot debug afterwards. ~1.5 s at 60 fps is nothing to a boot.
    const unsigned frames = g_menu_frames.fetch_add(1, std::memory_order_relaxed);
    if (frames < 90 || frames % 120 != 0) return;

    // One attempt every ~2 s, thirty attempts, then leave it to the pad cascade.
    const unsigned attempt = g_auto_continue_attempts.fetch_add(1, std::memory_order_relaxed) + 1;
    if (attempt > 30) {
        if (g_auto_continue_done.exchange(true)) return;
        logf("gamestate: auto-continue: no derivation after 30 attempts; the pad "
             "cascade stays in charge");
        return;
    }

    const int want = state_id_by_name("LoadSession");
    if (want < 0) {
        logf("gamestate: auto-continue: no LoadSession in the state-name table");
        g_auto_continue_done.store(true, std::memory_order_relaxed);
        return;
    }

    void* obj = state_object_from_samples(static_cast<std::uint32_t>(want));
    char const* how = "samples";
    if (obj == nullptr) {
        std::uint32_t menu_id = 0;
        void* anchor = machine_state_id(machine, &menu_id)
                           ? current_state_object(machine, menu_id)
                           : nullptr;
        for (int id = 0; id < kStateLimit && anchor == nullptr; ++id) {
            anchor = g_queued_state[id].load(std::memory_order_relaxed);
        }
        how = "cluster";
        obj = state_object_from_cluster(anchor, static_cast<std::uint32_t>(want));
    }

    if (state_debug_wanted()) {
        for (int id = 0; id < kStateLimit; ++id) {
            void* sample = g_queued_state[id].load(std::memory_order_relaxed);
            if (sample != nullptr) {
                logf("gamestate: [sdbg] sample id=%d obj=%p", id, sample);
            }
        }
        std::uint32_t mid = 0;
        void* cur = machine_state_id(machine, &mid)
                        ? current_state_object(machine, mid)
                        : nullptr;
        logf("gamestate: [sdbg] want=%d derived(%s)=%p current_obj=%p machine=%p",
             want, how, obj, cur, machine);
    }

    if (obj == nullptr) {
        if (attempt == 1 || attempt % 10 == 0) {
            logf("gamestate: auto-continue: derivation not ready (attempt %u)", attempt);
        }
        return;
    }

    if (g_auto_continue_done.exchange(true)) return;
    logf("gamestate: auto-continue queueing LoadSession obj=%p via %s -- no input",
         obj, how);
    g_set_target_state(machine, obj);
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
    // Every module load -- including the save's reload -- ends here, with the
    // engine's stats freshly parsed. Fire StatsLoaded now, as upstream does from
    // RPGStats::Load, so a mod that rewires progressions lands before LoadSession/
    // LoadLevel build the character. Idempotent per rebuild (StatsTakeLoaded).
    lua_fire_stats_loaded();
    return result;
}

StateProc g_machine_update = nullptr;

// Every frame is a client tick, and a state that differs from the last
// frame's is GameStateChanged -- upstream compares around its update call.
std::uint64_t machine_update_hook(void* machine, void* a, void* b, void* c) {
    static std::atomic<bool> first{true};
    if (first.exchange(false)) logf("gamestate: client frames are ticking");
    const std::uint64_t result = g_machine_update(machine, a, b, c);
    net_client_tick();

    static char const* last = nullptr;
    char const* now = client_game_state();

    // Fire StatsLoaded the instant the engine's stats are rebuilt -- inside
    // LoadModule, seconds before LoadSession deserialises the character -- rather
    // than from LoadMods at session time. Upstream fires it from RPGStats::Load.
    // Gated by the cheap C check; the Lua side fires once per rebuild.
    if (now != nullptr && (bg3le_stats_pending(false) || bg3le_stats_pending(true))) {
        lua_fire_stats_loaded();
    }
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
    maybe_auto_continue(machine, now);
    return result;
}

void set_target_state_hook(void* machine, void* state) {
    std::uint32_t to = 0;
    if (state_id(state, &to)) {
        void* vtable = *static_cast<void**>(state);
        logf("gamestate: client queued %s (vtable image+%#lx)", state_name(to),
             (unsigned long)(reinterpret_cast<std::uintptr_t>(vtable) - load_bias()));
        note_queued_state(to, state);
        if (state_debug_wanted()) {
            logf("gamestate: [sdbg] queued %s obj=%p machine=%p", state_name(to), state,
                 machine);
        }
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

// esv::GameState, in upstream's order (Enumerations/Engine.inl).
constexpr char const* kServerStates[] = {
    "Unknown", "Uninitialized", "Init", "Idle", "Exit", "LoadLevel",
    "LoadModule", "LoadSession", "UnloadLevel", "UnloadModule",
    "UnloadSession", "Sync", "Paused", "Running", "Save", "Disconnect",
    "BuildStory", "ReloadStory"};
// EoCServer::GameStateMachine, and its State, as upstream lays them out;
// checked live (Running reads 13).
constexpr std::size_t kServerMachine = 0xa0;
constexpr std::size_t kServerMachineState = 0x10;

char const* server_game_state() {
    void* server = nullptr;
    void* machine = nullptr;
    std::uint32_t state = 0;
    if (target::EoCServer() == 0
        || !safe_read(reinterpret_cast<void*>(load_bias() + target::EoCServer()), &server,
                      sizeof(server))
        || server == nullptr
        || !safe_read(static_cast<char*>(server) + kServerMachine, &machine,
                      sizeof(machine))
        || machine == nullptr
        || !safe_read(static_cast<char*>(machine) + kServerMachineState, &state,
                      sizeof(state))
        || state >= sizeof(kServerStates) / sizeof(kServerStates[0])) {
        return nullptr;
    }
    return kServerStates[state];
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
