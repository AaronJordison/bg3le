// Every method a widget offers, dispatched by name.
//
// bg3se declares these with P_FUN, and P_FUN is exactly what bg3le's field
// tables leave out -- a method has no offset to record. So the field
// machinery reads and writes a widget's properties and this file supplies
// the rest of the object: the thirty-odd Add* on a container, the window
// setters, the style accessors, the child list.
//
// Arguments arrive as a small tagged array rather than one entry point per
// signature, because the shapes are all drawn from the same handful of types
// and because the Lua side has to marshal them anyway. Where upstream takes
// std::optional, an argument that was not passed is simply absent, which is
// the same thing.
//
// The methods are bg3se's, by Norbyte and the bg3se contributors
// (https://github.com/Norbyte/bg3se); the dispatch is ours.

#include <stdafx.h>

#include <Extender/Client/IMGUI/IMGUI.h>
#include <Extender/Client/IMGUI/Objects.h>

#include <cstring>
#include <optional>
#include <string>

#include "../log.h"
#include "imgui_args.h"

extern "C" bool bg3le_meta_enum_label_value(char const* enumName,
                                            char const* label,
                                            std::uint64_t* value);

namespace bg3le {

bg3se::extui::Renderable* imgui_renderable(std::uint64_t handle);

namespace {

using namespace bg3se;
using namespace bg3se::extui;
using bg3se::lua::ImguiHandle;

// ---- reading the arguments -------------------------------------------------

ImguiArg const* arg_at(ImguiArg const* args, std::size_t count,
                       std::size_t index) {
    if (index >= count) return nullptr;
    if (args[index].Kind == kImguiArgNone) return nullptr;
    return &args[index];
}

char const* as_text(ImguiArg const* args, std::size_t count,
                    std::size_t index) {
    ImguiArg const* at = arg_at(args, count, index);
    if (at == nullptr || at->Text == nullptr) return "";
    return at->Text;
}

std::optional<bool> as_bool(ImguiArg const* args, std::size_t count,
                            std::size_t index) {
    ImguiArg const* at = arg_at(args, count, index);
    if (at == nullptr) return {};
    return at->Bool;
}

std::optional<int> as_int(ImguiArg const* args, std::size_t count,
                          std::size_t index) {
    ImguiArg const* at = arg_at(args, count, index);
    if (at == nullptr) return {};
    return at->Int;
}

std::optional<float> as_float(ImguiArg const* args, std::size_t count,
                              std::size_t index) {
    ImguiArg const* at = arg_at(args, count, index);
    if (at == nullptr) return {};
    return (float)at->Number;
}

std::optional<glm::vec2> as_vec2(ImguiArg const* args, std::size_t count,
                                 std::size_t index) {
    ImguiArg const* at = arg_at(args, count, index);
    if (at == nullptr) return {};
    return glm::vec2(at->Vec[0], at->Vec[1]);
}

std::optional<glm::vec3> as_vec3(ImguiArg const* args, std::size_t count,
                                 std::size_t index) {
    ImguiArg const* at = arg_at(args, count, index);
    if (at == nullptr) return {};
    return glm::vec3(at->Vec[0], at->Vec[1], at->Vec[2]);
}

std::optional<glm::vec4> as_vec4(ImguiArg const* args, std::size_t count,
                                 std::size_t index) {
    ImguiArg const* at = arg_at(args, count, index);
    if (at == nullptr) return {};
    return glm::vec4(at->Vec[0], at->Vec[1], at->Vec[2], at->Vec[3]);
}

ImguiHandle as_widget(ImguiArg const* args, std::size_t count,
                      std::size_t index) {
    ImguiArg const* at = arg_at(args, count, index);
    if (at == nullptr) return ImguiHandle();
    return ImguiHandle(at->Handle);
}

bg3se::FixedString as_fixed_string(ImguiArg const* args, std::size_t count,
                                   std::size_t index) {
    return bg3se::FixedString(as_text(args, count, index));
}

// An enum argument -- a style variable, a colour, a condition, a flag set.
//
// Upstream takes either the label or the number, so both are accepted here.
// A name that is not a label of that enum is reported and treated as absent
// rather than read as zero, which would silently mean whatever the first
// label happens to be.
template <class T>
std::optional<T> as_enum(ImguiArg const* args, std::size_t count,
                         std::size_t index, char const* enumName) {
    ImguiArg const* at = arg_at(args, count, index);
    if (at == nullptr) return {};

    if (at->Kind == kImguiArgText) {
        std::uint64_t value = 0;
        if (at->Text == nullptr
            || !bg3le_meta_enum_label_value(enumName, at->Text, &value)) {
            logf("imgui: \"%s\" is not a %s",
                 at->Text != nullptr ? at->Text : "", enumName);
            return {};
        }
        return (T)value;
    }

    return (T)at->Int;
}

// ---- writing the result ----------------------------------------------------

void give_nothing(ImguiArg* out) { out->Kind = kImguiArgNone; }

void give_handle(ImguiArg* out, ImguiHandle handle) {
    out->Kind = kImguiArgHandle;
    out->Handle = handle.Handle;
}

void give_bool(ImguiArg* out, bool value) {
    out->Kind = kImguiArgBool;
    out->Bool = value;
}

void give_float(ImguiArg* out, std::optional<float> value) {
    if (!value) {
        give_nothing(out);
        return;
    }
    out->Kind = kImguiArgNumber;
    out->Number = *value;
}

// Held until the caller has copied it, which it does before the next call.
void give_text(ImguiArg* out, STDString const& value) {
    thread_local std::string held;
    held.assign(value.data(), value.size());
    out->Kind = kImguiArgText;
    out->Text = held.c_str();
}

void give_vec4(ImguiArg* out, std::optional<glm::vec4> value) {
    if (!value) {
        give_nothing(out);
        return;
    }
    out->Kind = kImguiArgVec4;
    for (int i = 0; i < 4; ++i) out->Vec[i] = (*value)[i];
}

}  // namespace

// ---- the dispatch ----------------------------------------------------------

// Adds a child of the named kind. Zero is a real handle, so failure is
// InvalidHandle.
std::uint64_t imgui_add_child(TreeParent* parent, char const* kind,
                              ImguiArg const* args, std::size_t count) {
#define BG3LE_IS(name) (std::strcmp(kind, name) == 0)
#define TEXT(n) as_text(args, count, n)

    // Containers.
    if (BG3LE_IS("Group")) return parent->AddGroup(TEXT(0)).Handle;
    if (BG3LE_IS("CollapsingHeader")) {
        return parent->AddCollapsingHeader(TEXT(0)).Handle;
    }
    if (BG3LE_IS("TabBar")) return parent->AddTabBar(TEXT(0)).Handle;
    if (BG3LE_IS("Tree")) return parent->AddTree(TEXT(0)).Handle;
    if (BG3LE_IS("Table")) {
        const auto columns = as_int(args, count, 1);
        return parent->AddTable(TEXT(0), columns ? (std::uint32_t)*columns : 1u)
            .Handle;
    }
    if (BG3LE_IS("Popup")) return parent->AddPopup(TEXT(0)).Handle;
    if (BG3LE_IS("ChildWindow")) {
        return parent->AddChildWindow(TEXT(0)).Handle;
    }
    if (BG3LE_IS("Menu")) return parent->AddMenu(TEXT(0)).Handle;

    // Text.
    if (BG3LE_IS("Text")) return parent->AddText(TEXT(0)).Handle;
    if (BG3LE_IS("TextLink")) return parent->AddTextLink(TEXT(0)).Handle;
    if (BG3LE_IS("BulletText")) return parent->AddBulletText(TEXT(0)).Handle;
    if (BG3LE_IS("SeparatorText")) {
        return parent->AddSeparatorText(TEXT(0)).Handle;
    }

    // Layout.
    if (BG3LE_IS("Spacing")) return parent->AddSpacing().Handle;
    if (BG3LE_IS("NewLine")) return parent->AddNewLine().Handle;
    if (BG3LE_IS("Separator")) return parent->AddSeparator().Handle;
    if (BG3LE_IS("Dummy")) {
        const auto width = as_float(args, count, 0);
        const auto height = as_float(args, count, 1);
        return parent->AddDummy(width ? *width : 0.0f,
                                height ? *height : 0.0f)
            .Handle;
    }

    // Buttons and selection.
    if (BG3LE_IS("Button")) return parent->AddButton(TEXT(0)).Handle;
    if (BG3LE_IS("Selectable")) {
        return parent
            ->AddSelectable(TEXT(0), as_enum<GuiSelectableFlags>(args, count, 1, "GuiSelectableFlags"),
                            as_vec2(args, count, 2))
            .Handle;
    }
    if (BG3LE_IS("ImageButton")) {
        return parent
            ->AddImageButton(TEXT(0), as_fixed_string(args, count, 1),
                             as_vec2(args, count, 2), as_vec2(args, count, 3),
                             as_vec2(args, count, 4))
            .Handle;
    }
    if (BG3LE_IS("Checkbox")) {
        return parent->AddCheckbox(TEXT(0), as_bool(args, count, 1)).Handle;
    }
    if (BG3LE_IS("RadioButton")) {
        return parent->AddRadioButton(TEXT(0), as_bool(args, count, 1)).Handle;
    }
    if (BG3LE_IS("Combo")) return parent->AddCombo(TEXT(0)).Handle;

    // Images.
    if (BG3LE_IS("Image")) {
        return parent
            ->AddImage(as_fixed_string(args, count, 0), as_vec2(args, count, 1),
                       as_vec2(args, count, 2), as_vec2(args, count, 3))
            .Handle;
    }
    if (BG3LE_IS("Icon")) {
        return parent
            ->AddIcon(as_fixed_string(args, count, 0), as_vec2(args, count, 1))
            .Handle;
    }

    // Numbers and text entry.
    if (BG3LE_IS("InputText")) {
        std::optional<bg3se::STDString> value;
        if (arg_at(args, count, 1) != nullptr) value = TEXT(1);
        return parent->AddInputText(TEXT(0), value).Handle;
    }
    if (BG3LE_IS("Drag")) {
        return parent
            ->AddDrag(TEXT(0), as_float(args, count, 1),
                      as_float(args, count, 2), as_float(args, count, 3))
            .Handle;
    }
    if (BG3LE_IS("DragInt")) {
        return parent
            ->AddDragInt(TEXT(0), as_int(args, count, 1),
                         as_int(args, count, 2), as_int(args, count, 3))
            .Handle;
    }
    if (BG3LE_IS("Slider")) {
        return parent
            ->AddSlider(TEXT(0), as_float(args, count, 1),
                        as_float(args, count, 2), as_float(args, count, 3))
            .Handle;
    }
    if (BG3LE_IS("SliderInt")) {
        return parent
            ->AddSliderInt(TEXT(0), as_int(args, count, 1),
                           as_int(args, count, 2), as_int(args, count, 3))
            .Handle;
    }
    if (BG3LE_IS("InputScalar")) {
        return parent->AddInputScalar(TEXT(0), as_float(args, count, 1))
            .Handle;
    }
    if (BG3LE_IS("InputInt")) {
        return parent->AddInputInt(TEXT(0), as_int(args, count, 1)).Handle;
    }
    if (BG3LE_IS("ColorEdit")) {
        return parent->AddColorEdit(TEXT(0), as_vec3(args, count, 1)).Handle;
    }
    if (BG3LE_IS("ColorPicker")) {
        return parent->AddColorPicker(TEXT(0), as_vec3(args, count, 1))
            .Handle;
    }
    if (BG3LE_IS("ProgressBar")) return parent->AddProgressBar().Handle;

    return InvalidHandle;
#undef TEXT
#undef BG3LE_IS
}

// Calls the named method. False if the widget has no such method, which is
// how Ext.IMGUI tells "not a method" from "a method that returned nothing".
bool imgui_call_method(Renderable* object, char const* name,
                       ImguiArg const* args, std::size_t count,
                       ImguiArg* out) {
    give_nothing(out);

#define BG3LE_IS(name_) (std::strcmp(name, name_) == 0)

    // Renderable.
    if (BG3LE_IS("Destroy")) {
        object->Destroy();
        return true;
    }

    auto* styled = dynamic_cast<StyledRenderable*>(object);
    if (styled != nullptr) {
        if (BG3LE_IS("GetStyle")) {
            const auto var = as_enum<GuiStyleVar>(args, count, 0, "GuiStyleVar");
            if (!var) return true;
            give_float(out, styled->GetStyleVar(*var));
            return true;
        }
        if (BG3LE_IS("SetStyle")) {
            const auto var = as_enum<GuiStyleVar>(args, count, 0, "GuiStyleVar");
            const auto value = as_float(args, count, 1);
            if (!var || !value) return true;
            styled->SetStyleVar(*var, *value, as_float(args, count, 2));
            return true;
        }
        if (BG3LE_IS("GetColor")) {
            const auto color = as_enum<GuiColor>(args, count, 0, "GuiColor");
            if (!color) return true;
            give_vec4(out, styled->GetStyleColor(*color));
            return true;
        }
        if (BG3LE_IS("SetColor")) {
            const auto color = as_enum<GuiColor>(args, count, 0, "GuiColor");
            const auto value = as_vec4(args, count, 1);
            if (!color || !value) return true;
            styled->SetStyleColor(*color, *value);
            return true;
        }
        if (BG3LE_IS("Activate")) {
            styled->Activate();
            return true;
        }
        if (BG3LE_IS("Tooltip")) {
            give_handle(out, styled->Tooltip());
            return true;
        }
    }

    // InputText.Text is a getter/setter pair upstream (P_GETTER_SETTER), with
    // no field of its own in the tables; Ext.IMGUI's proxy reads and writes
    // it through these. SetText also makes an active field reload its buffer.
    auto* input = dynamic_cast<InputText*>(object);
    if (input != nullptr) {
        if (BG3LE_IS("GetText")) {
            give_text(out, input->GetText());
            return true;
        }
        if (BG3LE_IS("SetText")) {
            input->SetText(STDString(as_text(args, count, 0)));
            return true;
        }
    }

    auto* tree = dynamic_cast<TreeParent*>(object);
    if (tree != nullptr) {
        if (BG3LE_IS("RemoveChild")) {
            give_bool(out, tree->RemoveChild(as_widget(args, count, 0)));
            return true;
        }
        if (BG3LE_IS("DetachChild")) {
            give_bool(out, tree->DetachChild(as_widget(args, count, 0)));
            return true;
        }
        if (BG3LE_IS("AttachChild")) {
            give_bool(out, tree->AttachChild(as_widget(args, count, 0)));
            return true;
        }
        if (BG3LE_IS("RemoveAllChildren")) {
            tree->RemoveAllChildren();
            return true;
        }
    }

    auto* window = dynamic_cast<WindowBase*>(object);
    if (window != nullptr) {
        if (BG3LE_IS("SetPos")) {
            const auto pos = as_vec2(args, count, 0);
            if (!pos) return true;
            window->SetPos(*pos, as_enum<GuiCond>(args, count, 1, "GuiCond"),
                           as_vec2(args, count, 2));
            return true;
        }
        if (BG3LE_IS("SetSize")) {
            const auto size = as_vec2(args, count, 0);
            if (!size) return true;
            window->SetSize(*size, as_enum<GuiCond>(args, count, 1, "GuiCond"));
            return true;
        }
        if (BG3LE_IS("SetSizeConstraints")) {
            window->SetSizeConstraints(as_vec2(args, count, 0),
                                       as_vec2(args, count, 1));
            return true;
        }
        if (BG3LE_IS("SetContentSize")) {
            window->SetContentSize(as_vec2(args, count, 0));
            return true;
        }
        if (BG3LE_IS("SetCollapsed")) {
            const auto collapsed = as_bool(args, count, 0);
            window->SetCollapsed(collapsed ? *collapsed : false,
                                 as_enum<GuiCond>(args, count, 1, "GuiCond"));
            return true;
        }
        if (BG3LE_IS("SetFocus")) {
            window->SetFocus();
            return true;
        }
        if (BG3LE_IS("SetScroll")) {
            window->SetScroll(as_vec2(args, count, 0));
            return true;
        }
        if (BG3LE_IS("SetBgAlpha")) {
            window->SetBgAlpha(as_float(args, count, 0));
            return true;
        }
    }

    if (auto* w = dynamic_cast<Window*>(object)) {
        if (BG3LE_IS("AddMainMenu")) {
            give_handle(out, w->AddMainMenu());
            return true;
        }
    }

    if (auto* popup = dynamic_cast<Popup*>(object)) {
        if (BG3LE_IS("Open")) {
            popup->Open(as_enum<GuiPopupFlags>(args, count, 0, "GuiPopupFlags"));
            return true;
        }
    }

    if (auto* bar = dynamic_cast<TabBar*>(object)) {
        if (BG3LE_IS("AddTabItem")) {
            give_handle(out, bar->AddTabItem(as_text(args, count, 0)));
            return true;
        }
    }

    if (auto* t = dynamic_cast<Tree*>(object)) {
        if (BG3LE_IS("SetOpen")) {
            const auto open = as_bool(args, count, 0);
            t->SetOpen(open ? *open : false,
                       as_enum<GuiCond>(args, count, 1, "GuiCond"));
            return true;
        }
    }

    if (auto* table = dynamic_cast<Table*>(object)) {
        if (BG3LE_IS("AddRow")) {
            give_handle(out, table->AddRow());
            return true;
        }
        if (BG3LE_IS("AddColumn")) {
            table->AddColumn(as_text(args, count, 0),
                             as_enum<GuiTableColumnFlags>(args, count, 1, "GuiTableColumnFlags"),
                             as_float(args, count, 2));
            return true;
        }
    }

    if (auto* row = dynamic_cast<TableRow*>(object)) {
        if (BG3LE_IS("AddCell")) {
            give_handle(out, row->AddCell());
            return true;
        }
    }

    if (auto* menu = dynamic_cast<Menu*>(object)) {
        if (BG3LE_IS("AddItem")) {
            std::optional<char const*> shortcut;
            if (arg_at(args, count, 1) != nullptr) {
                shortcut = as_text(args, count, 1);
            }
            give_handle(out, menu->AddItem(as_text(args, count, 0), shortcut));
            return true;
        }
    }

    return false;
#undef BG3LE_IS
}

// A container's children, in order. Written into `out` and the count
// returned, or the count alone if `out` is null.
std::size_t imgui_children(Renderable* object, std::uint64_t* out,
                           std::size_t capacity) {
    auto* tree = dynamic_cast<TreeParent*>(object);
    if (tree == nullptr) return 0;

    const std::size_t total = tree->Children.size();
    if (out == nullptr) return total;

    std::size_t written = 0;
    for (auto const& child : tree->Children) {
        if (written >= capacity) break;
        out[written++] = child;
    }
    return total;
}

}  // namespace bg3le
