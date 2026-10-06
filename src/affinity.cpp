// Undoes the engine's thread pinning.
//
// The engine pins its threads to one logical CPU each: ServerWorker,
// GameThread, WorkerThreadSch and the WT/High pool all get a single cpu. That
// core then becomes the frame gate -- a thread that needs cpu waits for its
// one assigned core while fifteen others idle. The same game under Proton runs
// every thread on 0-15, because Wine does not apply the requests, and that
// build never had the problem.
//
// Measured over three runs each of the startup window, which was the worst of
// it: p95 frametime 75.6ms -> 59.4ms, p99 133ms -> 77.8ms, frames over 100ms
// 9 -> 3.7, time lost above 50ms 4598ms -> 3333ms. The number of hitches
// barely moves (173 -> 175); each one gets much shorter. Pinning was not
// creating hitches, it was making every hitch longer.
//
// It does not fix the cutscene stutter, which is a separate problem.
//
// The interesting part is which call to hook. An earlier attempt at this
// hooked sched_setaffinity and pthread_setaffinity_np, measured as doing
// nothing, and was reverted -- but the engine sets its masks through
// pthread_attr_setaffinity_np before pthread_create, so every thread that
// mattered stayed pinned while the hook reported success. Both pthread calls
// are interposed here; the attribute one is the one that does the work.
//
// sched_setaffinity is left alone: only Wwise's threads call it, and widening
// them crashed a Steam Deck in JobManager_dispatchMultiple at startup.
//
// BG3LE_AFFINITY=off disables this.

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "log.h"

namespace {

using bg3le::logf;

bool enabled() {
    static const bool on = [] {
        const char* opt = std::getenv("BG3LE_AFFINITY");
        return opt == nullptr || std::strcmp(opt, "off") != 0;
    }();
    return on;
}

int cpu_count() {
    static const int n = [] {
        const long c = ::sysconf(_SC_NPROCESSORS_CONF);
        return (c > 0 && c <= CPU_SETSIZE) ? (int)c : CPU_SETSIZE;
    }();
    return n;
}

// True if the mask already allows every cpu, in which case there is nothing
// to widen and nothing worth logging.
bool unrestricted(const cpu_set_t* mask, std::size_t size) {
    for (int c = 0; c < cpu_count(); ++c) {
        if (!CPU_ISSET_S((std::size_t)c, size, mask)) return false;
    }
    return true;
}

int count_set(const cpu_set_t* mask, std::size_t size) {
    int n = 0;
    for (int c = 0; c < cpu_count(); ++c) {
        if (CPU_ISSET_S((std::size_t)c, size, mask)) ++n;
    }
    return n;
}

void fill(cpu_set_t* out, std::size_t size) {
    CPU_ZERO_S(size, out);
    for (int c = 0; c < cpu_count(); ++c) CPU_SET_S((std::size_t)c, size, out);
}

// Whether to replace the requested mask, and what with.
bool widen(const cpu_set_t* mask, std::size_t size, cpu_set_t* out,
           const char* api) {
    if (!enabled() || mask == nullptr) return false;
    if (unrestricted(mask, size)) return false;

    const int was = count_set(mask, size);
    fill(out, size);
    logf("affinity: %s asked for %d of %d cpus, gave it all %d", api, was,
         cpu_count(), cpu_count());
    return true;
}

using ThreadFn = int (*)(pthread_t, size_t, const cpu_set_t*);
using AttrFn = int (*)(pthread_attr_t*, size_t, const cpu_set_t*);

template <typename Fn>
Fn next(const char* name) {
    return reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, name));
}

}  // namespace

// The engine calls these directly and we are earlier in the search order, so
// defining them interposes; dlsym(RTLD_NEXT) reaches the real ones.

// This is the one the engine actually uses: a mask in the thread attributes,
// applied by pthread_create. Hooking only the two below is what made the
// first attempt at this look like a no-op.
extern "C" int pthread_attr_setaffinity_np(pthread_attr_t* attr,
                                           size_t cpusetsize,
                                           const cpu_set_t* mask) {
    static const auto real = next<AttrFn>("pthread_attr_setaffinity_np");
    if (real == nullptr) return -1;

    cpu_set_t all;
    if (widen(mask, cpusetsize, &all, "pthread_attr_setaffinity_np")) {
        return real(attr, cpusetsize, &all);
    }
    return real(attr, cpusetsize, mask);
}

extern "C" int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize,
                                      const cpu_set_t* mask) {
    static const auto real = next<ThreadFn>("pthread_setaffinity_np");
    if (real == nullptr) return -1;

    cpu_set_t all;
    if (widen(mask, cpusetsize, &all, "pthread_setaffinity_np")) {
        return real(thread, cpusetsize, &all);
    }
    return real(thread, cpusetsize, mask);
}

