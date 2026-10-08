// MoveableObject's Lua methods (upstream's P_FUN SetWorldTranslate, SetWorldRotate
// and SetWorldScale), called through upstream's own helpers. The vendored class's
// virtuals map to the Linux vtable in declaration order: GetRTTI, the two
// Itanium destructors, then the three setters.
#include <stdafx.h>
#include <GameDefinitions/Components/All.h>
#include <GameDefinitions/Render.h>

#include <cstdint>

#include "../hook.h"
#include "../mem.h"

// what: 0 translate (x, y, z), 1 rotate (x, y, z, w), 2 scale (x, y, z).
extern "C" bool bg3le_moveable_set(void* object, int what, float const* v) {
    if (object == nullptr || what < 0 || what > 2) return false;
    void* vtable = nullptr;
    if (!bg3le::safe_read(object, &vtable, sizeof(vtable)) || vtable == nullptr) return false;
    std::uintptr_t fn = 0;
    if (!bg3le::safe_read(static_cast<char*>(vtable) + (3 + what) * sizeof(void*), &fn, sizeof(fn))
        || fn < bg3le::load_bias() || !bg3le::in_text(fn - bg3le::load_bias(), 1)) {
        return false;
    }
    auto* moveable = static_cast<bg3se::MoveableObject*>(object);
    switch (what) {
    case 0: moveable->LuaSetWorldTranslate(glm::vec3(v[0], v[1], v[2])); break;
    case 1: moveable->LuaSetWorldRotate(glm::quat(v[3], v[0], v[1], v[2])); break;
    default: moveable->LuaSetWorldScale(glm::vec3(v[0], v[1], v[2])); break;
    }
    return true;
}
