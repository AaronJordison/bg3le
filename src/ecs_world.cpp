// Captures the ECS storage pointer, which cannot be found by name.
//
// The engine has no EntityWorld symbol: of the ecs:: namespace only the
// type-id contexts survive in .symtab, because those appear as template
// arguments of the named TypeId statics. bg3se reaches the world through
// GetEoCServer()->EntityWorld, and both are anonymous file-scope statics here.
//
// What we do have is a component lookup function, found by disassembling the
// code that reads a component's type-index static:
//
//   mov  r15d, [rip+...]     ; ls::TypeId<eoc::HealthComponent, ...>::m_TypeIndex
//   mov  rdi,  [rbp+0x348]   ; the object we want
//   and  edx,  0x7fff        ; index masked to 15 bits
//   call <lookup>            ; (object, entity, index)
//
// That lookup led to the one we actually want, called just after it:
//
//   cmp   [rcx+rax*8], esi          ; salt check
//   movzx eax, WORD [rcx+rax*8+4]   ; -> 16-bit storage index
//   mov   rsi, [rdi]                ; [rdi] is an array of storage pointers
//   mov   rax, [rsi+rax*8]          ; storages[index]
//   ret
//
// So it is EntityStorageContainer::GetEntityStorage(EntityHandle), and its
// first argument is the container. bg3se agrees: EntityStorageContainer begins
// with Array<EntityStorageData*> Storages, and its Array is {T* buf; uint32
// size}, so the buffer pointer sits at offset 0 exactly as the code expects.
//
// The container is what maps an EntityHandle to its storage, which is the
// entry point for reaching a component. It is reachable without the
// EntityWorld, which has no symbol and no obvious capture point.
//
// Rather than guess the prototype, the hook is a naked thunk: it records rdi
// and jumps to the original with every register untouched, so the real
// signature does not matter and there is no ABI risk.

#include "ecs_world.h"

#include "hook.h"
#include "log.h"
#include "targets.h"

namespace bg3le {
namespace ecs {

// Hidden, so the thunk can reach them with a PC-relative access. A default
// visibility symbol in a shared object is preemptible, and the linker will not
// accept R_X86_64_PC32 against one.
extern "C" {
__attribute__((visibility("hidden"))) void* bg3le_ecs_lookup_original = nullptr;
__attribute__((visibility("hidden"))) void* bg3le_ecs_storage = nullptr;
__attribute__((visibility("hidden"))) void* bg3le_ecs_storage_alt = nullptr;
__attribute__((visibility("hidden"))) unsigned long long bg3le_ecs_last_entity = 0;
}

namespace {

// Records the first argument into the first free of two slots, then jumps to
// the original. No prologue, no clobbers, nothing touched but rax -- which is
// caller-saved and about to hold the return value anyway.
//
// Two slots rather than one because the client and server worlds have a
// container each and both come through here; keeping only the first meant
// whichever the engine touched first was the only one we could ever reach.
extern "C" void bg3le_ecs_capture_thunk();
__asm__(
    ".text\n"
    ".globl bg3le_ecs_capture_thunk\n"
    ".hidden bg3le_ecs_capture_thunk\n"
    "bg3le_ecs_capture_thunk:\n"
    // Only record a container whose Storages buffer has been allocated.
    "  mov (%rdi), %rax\n"
    "  test %rax, %rax\n"
    "  je 9f\n"
    "  mov bg3le_ecs_storage(%rip), %rax\n"
    "  test %rax, %rax\n"
    "  je 1f\n"
    // Slot one is taken. If it is this same container there is nothing to do,
    // otherwise this is the other world and belongs in slot two.
    "  cmp %rdi, %rax\n"
    "  je 9f\n"
    "  mov bg3le_ecs_storage_alt(%rip), %rax\n"
    "  test %rax, %rax\n"
    "  jne 9f\n"
    "  mov %rdi, bg3le_ecs_storage_alt(%rip)\n"
    "  jmp 9f\n"
    "1:\n"
    "  mov %rdi, bg3le_ecs_storage(%rip)\n"
    "9:\n"
    // Record the handle every time, so there is always a live entity to test
    // against while UUID -> handle is still missing.
    "  mov %rsi, bg3le_ecs_last_entity(%rip)\n"
    "  jmp *bg3le_ecs_lookup_original(%rip)\n");

}  // namespace

bool install_container_capture() {
    // EntityStorageContainer::GetEntityStorage(EntityHandle), from the
    // disassembly above; hook_call_sites verifies each site before patching.
    const std::uintptr_t lookup = target::EntityStorageLookup();
    void* original = nullptr;
    const std::size_t patched = lookup == 0 ? 0 : hook_call_sites(
        lookup, reinterpret_cast<void*>(&bg3le_ecs_capture_thunk), &original);
    if (patched == 0) {
        logf("ecs: no call sites patched for the entity storage lookup at "
             "%#lx; the container pointer will stay unknown", (unsigned long)lookup);
        return false;
    }
    bg3le_ecs_lookup_original = original;
    logf("ecs: watching %zu call sites of the entity storage lookup", patched);
    return true;
}

void* container() { return bg3le_ecs_storage; }

void* container_alt() { return bg3le_ecs_storage_alt; }

unsigned long long last_entity() { return bg3le_ecs_last_entity; }

}  // namespace ecs
}  // namespace bg3le
