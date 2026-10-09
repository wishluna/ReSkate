#include "gui_internal.h"

#include "Extension/UI/Overlay/playstation_input.h"

#include <Xinput.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstdlib>

// Controllers: every XInput slot and PlayStation pads over HID, handed to
// ImGui's navigation, and the button prompts along the bottom edge.
namespace dingosdk::launcher_gui::detail {
namespace {

using GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

constexpr std::uint16_t dpad = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
                               XINPUT_GAMEPAD_DPAD_RIGHT;

struct Pad {
    unsigned slots{};             // XInput slots with a pad in them
    bool probe{true};             // ask every slot: at startup, after a device change, back in front
    bool foreground{};
    bool connected{};
    bool active{};                // used since the mouse and keyboard were
    bool woke{};                  // became active this frame
    ImGuiDir stuck{ImGuiDir_None};
    ControllerStyle style{};
    std::uint16_t stick{};        // the D-pad direction the left stick is held in
    std::uint16_t held{};         // XINPUT_GAMEPAD_* bits, the left stick folded into the D-pad
    std::uint16_t pressed{};      // went down this frame
    std::uint16_t swallowed{};    // down when the controller took over; ignored until let go
    ULONGLONG changed{};          // GetTickCount64 when a button last went down or up
    int popups{};                 // popups open before this frame's ImGui::NewFrame
    bool item_active{};           // and whether an item (a text field) was active
    std::vector<PadHint> hints;
};
Pad g_pad;

GetState xinput() {
    // Loaded rather than linked, as ImGui's backend does: a PC without it still gets the launcher.
    static const auto get_state = []() -> GetState {
        for (const auto* name : {L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll"})
            if (const auto module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
                if (const auto entry = GetProcAddress(module, "XInputGetState"))
                    return reinterpret_cast<GetState>(entry);
        return nullptr;
    }();
    return get_state;
}

// Every connected pad at once: a virtual pad (Steam Input, DS4Windows) can take
// any slot, and an idle one may sit ahead of the pad in use. A PlayStation pad
// read over HID only counts when no XInput pad is there, or a remapped pad
// would press each button twice; it still names the buttons.
XINPUT_GAMEPAD read_pads() {
    XINPUT_GAMEPAD merged{};
    bool any = false;
    const auto strength = [](SHORT x, SHORT y) { return std::abs(static_cast<int>(x)) + std::abs(static_cast<int>(y)); };
    if (const auto get_state = xinput()) {
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            const unsigned bit = 1u << slot;
            // An empty slot is slow to ask about, so only when something may have changed.
            if (!g_pad.probe && !(g_pad.slots & bit)) continue;
            XINPUT_STATE state{};
            if (get_state(slot, &state) != ERROR_SUCCESS) {
                g_pad.slots &= ~bit;
                continue;
            }
            g_pad.slots |= bit;
            any = true;
            const auto& pad = state.Gamepad;
            merged.wButtons |= pad.wButtons;
            if (strength(pad.sThumbLX, pad.sThumbLY) > strength(merged.sThumbLX, merged.sThumbLY)) {
                merged.sThumbLX = pad.sThumbLX;
                merged.sThumbLY = pad.sThumbLY;
            }
            if (strength(pad.sThumbRX, pad.sThumbRY) > strength(merged.sThumbRX, merged.sThumbRY)) {
                merged.sThumbRX = pad.sThumbRX;
                merged.sThumbRY = pad.sThumbRY;
            }
        }
    }
    g_pad.probe = false;
    const auto playstation = overlay::detail::read_playstation_pads();
    g_pad.style = playstation.style;
    if (!any && playstation.available) {
        any = true;
        merged.wButtons = playstation.pad.buttons;
        merged.sThumbLX = playstation.pad.left_x;
        merged.sThumbLY = playstation.pad.left_y;
        merged.sThumbRX = playstation.pad.right_x;
        merged.sThumbRY = playstation.pad.right_y;
    }
    g_pad.connected = any;
    return merged;
}

// The left stick as a D-pad. A direction is taken past half way, along the
// axis the stick leans on most, and kept until the stick is nearly back, so a
// stick resting near the line does not flicker between two.
std::uint16_t stick_direction(SHORT x, SHORT y, std::uint16_t held) {
    const float right = static_cast<float>(x) / 32767.0f, up = static_cast<float>(y) / 32767.0f;
    const auto along = [&](std::uint16_t direction) {
        switch (direction) {
        case XINPUT_GAMEPAD_DPAD_LEFT: return -right;
        case XINPUT_GAMEPAD_DPAD_RIGHT: return right;
        case XINPUT_GAMEPAD_DPAD_UP: return up;
        case XINPUT_GAMEPAD_DPAD_DOWN: return -up;
        default: return 0.0f;
        }
    };
    if (held && along(held) > 0.3f) return held;
    if (std::max(std::abs(right), std::abs(up)) < 0.5f) return 0;
    if (std::abs(right) > std::abs(up))
        return static_cast<std::uint16_t>(right < 0 ? XINPUT_GAMEPAD_DPAD_LEFT : XINPUT_GAMEPAD_DPAD_RIGHT);
    return static_cast<std::uint16_t>(up > 0 ? XINPUT_GAMEPAD_DPAD_UP : XINPUT_GAMEPAD_DPAD_DOWN);
}

// How far the right stick scrolls, -1 (up) to 1 (down): nothing near the
// centre, then slow at first so a short push moves a little.
float stick_scroll(SHORT y) {
    const float value = -static_cast<float>(y) / 32767.0f, dead = 0.25f;
    if (std::abs(value) < dead) return 0;
    const float past = std::min((std::abs(value) - dead) / (1 - dead), 1.0f);
    return std::copysign(past * past, value);
}

ImGuiWindow* scrollable_child(ImGuiWindow* window) {
    for (auto* child : window->DC.ChildWindows) {
        if (!child->Active || child->Hidden) continue;
        if (child->ScrollMax.y > 0) return child;
        if (auto* inner = scrollable_child(child)) return inner;
    }
    return nullptr;
}

// What the right stick scrolls: the list the selection is in, or else the first
// list the focused page or panel shows (an overview's text has no items to select).
ImGuiWindow* scroll_target() {
    auto* focused = GImGui->NavWindow;
    for (auto* window = focused; window; window = window->Flags & ImGuiWindowFlags_Popup ? nullptr : window->ParentWindow)
        if (window->ScrollMax.y > 0) return window;
    return focused ? scrollable_child(focused->RootWindow) : nullptr;
}

std::uint16_t button_bit(PadButton button) {
    switch (button) {
    case PadButton::a: return XINPUT_GAMEPAD_A;
    case PadButton::b: return XINPUT_GAMEPAD_B;
    case PadButton::x: return XINPUT_GAMEPAD_X;
    case PadButton::y: return XINPUT_GAMEPAD_Y;
    case PadButton::lb: return XINPUT_GAMEPAD_LEFT_SHOULDER;
    case PadButton::rb: return XINPUT_GAMEPAD_RIGHT_SHOULDER;
    case PadButton::start: return XINPUT_GAMEPAD_START;
    case PadButton::bumpers: break;
    }
    return 0;
}

const char* pill_text(PadButton button, bool playstation) {
    switch (button) {
    case PadButton::lb: return playstation ? "L1" : "LB";
    case PadButton::rb: return playstation ? "R1" : "RB";
    case PadButton::start: return playstation ? "OPTIONS" : "MENU";
    default: return nullptr;
    }
}

// One button as the pad in use shows it: a lettered disc for an Xbox pad, the
// shape for a PlayStation one, a pill for the shoulder and menu buttons. `at`
// is its left edge, vertically centred; a null `draw` only measures it.
float glyph(ImDrawList* draw, const Fonts& fonts, ImVec2 at, PadButton button) {
    const bool playstation = g_pad.style != ControllerStyle::xbox;
    if (button == PadButton::bumpers) {
        const float first = glyph(draw, fonts, at, PadButton::lb) + S(4);
        return first + glyph(draw, fonts, ImVec2(at.x + first, at.y), PadButton::rb);
    }
    const float radius = S(8), size = S(10);
    if (const char* text = pill_text(button, playstation)) {
        const float width = fonts.bold->CalcTextSizeA(size, FLT_MAX, 0, text).x + S(12);
        if (draw) {
            draw->AddRectFilled(ImVec2(at.x, at.y - radius), ImVec2(at.x + width, at.y + radius), color::text, radius);
            draw->AddText(fonts.bold, size, ImVec2(at.x + S(6), at.y - size * 0.5f), color::ink, text);
        }
        return width;
    }
    if (!draw) return radius * 2;
    const ImVec2 centre(at.x + radius, at.y);
    draw->AddCircleFilled(centre, radius, color::text);
    const float mark = S(4), line = S(1.6f);
    if (!playstation) {
        const char letter[2]{"ABXY"[static_cast<int>(button)], 0};
        const auto extent = fonts.bold->CalcTextSizeA(size, FLT_MAX, 0, letter);
        draw->AddText(fonts.bold, size, ImVec2(centre.x - extent.x * 0.5f, centre.y - extent.y * 0.5f), color::ink, letter);
    } else if (button == PadButton::a) {
        draw->AddLine(ImVec2(centre.x - mark, centre.y - mark), ImVec2(centre.x + mark, centre.y + mark), color::ink, line);
        draw->AddLine(ImVec2(centre.x - mark, centre.y + mark), ImVec2(centre.x + mark, centre.y - mark), color::ink, line);
    } else if (button == PadButton::b) {
        draw->AddCircle(centre, mark, color::ink, 0, line);
    } else if (button == PadButton::x) {
        draw->AddRect(ImVec2(centre.x - mark * 0.85f, centre.y - mark * 0.85f),
            ImVec2(centre.x + mark * 0.85f, centre.y + mark * 0.85f), color::ink, 0, 0, line);
    } else {
        draw->AddTriangle(ImVec2(centre.x, centre.y - mark), ImVec2(centre.x + mark, centre.y + mark * 0.75f),
            ImVec2(centre.x - mark, centre.y + mark * 0.75f), color::ink, line);
    }
    return radius * 2;
}

} // namespace

void controller_frame(HWND window) {
    auto& g = *GImGui;
    auto& io = ImGui::GetIO();
    // What B finds before ImGui's navigation acts on it: a B that lets go of a
    // text field or closes a combo must not also leave the page under it.
    g_pad.popups = g.OpenPopupStack.Size;
    g_pad.item_active = g.ActiveId != 0;
    // And a move it found nothing for, before NewFrame lets go of it.
    const bool found = g.NavMoveResultLocal.ID || g.NavMoveResultOther.ID;
    g_pad.stuck = g.NavMoveSubmitted && !found && !(g.NavMoveFlags & ImGuiNavMoveFlags_IsTabbing) ? g.NavMoveDir
                                                                                                    : ImGuiDir_None;

    // Nothing is read for a window in the background, so a pad used in the game
    // or elsewhere never presses anything here.
    const bool foreground = GetForegroundWindow() == window;
    if (foreground && !g_pad.foreground) g_pad.probe = true;   // pads may have come and gone meanwhile
    g_pad.foreground = foreground;
    XINPUT_GAMEPAD pad{};
    if (foreground) pad = read_pads();
    else g_pad.connected = false;

    g_pad.stick = stick_direction(pad.sThumbLX, pad.sThumbLY, g_pad.stick);
    const auto held = static_cast<std::uint16_t>(pad.wButtons | g_pad.stick);
    const float scroll = stick_scroll(pad.sThumbRY);
    g_pad.woke = false;
    if (!g_pad.connected) g_pad.active = false;
    else if (!g_pad.active && ((held & ~g_pad.held) || scroll != 0)) {
        // The controller takes over. Its first press only shows where the
        // selection is; with nothing selected yet, a direction picks the first item.
        g_pad.active = g_pad.woke = true;
        g_pad.swallowed = g.NavId ? held : static_cast<std::uint16_t>(held & ~dpad);
        ImGui::SetNavCursorVisibleAfterMove();
    }
    g_pad.swallowed &= held;
    const auto usable = static_cast<std::uint16_t>(held & ~g_pad.swallowed);
    g_pad.pressed = static_cast<std::uint16_t>(usable & ~g_pad.held);
    if (held != g_pad.held) g_pad.changed = GetTickCount64();
    g_pad.held = held;

    // ImGui gets A, B and the D-pad for its navigation. X, Y, the shoulders and
    // START are the screens' own (and ImGui would open its window switcher on X).
    if (g_pad.connected) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    const auto feed = [&](ImGuiKey key, int bit) { io.AddKeyEvent(key, (usable & bit) != 0); };
    feed(ImGuiKey_GamepadFaceDown, XINPUT_GAMEPAD_A);
    feed(ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B);
    feed(ImGuiKey_GamepadDpadUp, XINPUT_GAMEPAD_DPAD_UP);
    feed(ImGuiKey_GamepadDpadDown, XINPUT_GAMEPAD_DPAD_DOWN);
    feed(ImGuiKey_GamepadDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
    feed(ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);

    if (scroll != 0)
        if (auto* target = scroll_target())
            ImGui::SetScrollY(target, target->Scroll.y + scroll * S(1600) * io.DeltaTime);

    // The selection outline only while the controller is in use: the mouse and
    // keyboard look as they always have.
    g.Style.Colors[ImGuiCol_NavCursor] = g_pad.active ? ImGui::ColorConvertU32ToFloat4(color::text) : ImVec4(0, 0, 0, 0);
}

void controller_other_input() { g_pad.active = false; }

void controller_devices_changed() { g_pad.probe = true; }

bool pad_active() { return g_pad.active; }

bool pad_echo() { return GetTickCount64() - g_pad.changed < 250; }

bool pad_woke() { return g_pad.woke; }

ImGuiDir pad_stuck() { return g_pad.stuck; }

bool pad_pressed(PadButton button) { return (g_pad.pressed & button_bit(button)) != 0; }

bool pad_back(int popups) {
    return pad_pressed(PadButton::b) && !g_pad.item_active && g_pad.popups <= popups;
}

void pad_focus_here(bool now) {
    ImGui::SetItemDefaultFocus();
    if (!now) return;
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisibleAfterMove();
}

void pad_enter_window() {
    auto* window = ImGui::GetCurrentWindow();
    ImGui::FocusWindow(window);
    ImGui::NavInitWindow(window, true);
    ImGui::SetNavCursorVisibleAfterMove();
}

void pad_hints(std::vector<PadHint> hints) { g_pad.hints = std::move(hints); }

void pad_end_frame(const Fonts& fonts, ImVec2 size) {
    auto hints = std::move(g_pad.hints);
    g_pad.hints.clear();
    if (!g_pad.active) return;
    // The pointer would only sit over something the selection is not on.
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    // A combo or menu has the input, whichever screen opened it.
    if (const auto* window = GImGui->NavWindow;
        window && (window->Flags & ImGuiWindowFlags_Popup) && !(window->Flags & ImGuiWindowFlags_Modal))
        hints = {{PadButton::a, "SELECT"}, {PadButton::b, "CLOSE"}};
    if (hints.empty()) return;

    const float label = S(11), gap = S(6), spacing = S(16);
    const auto text_width = [&](const std::string& text) {
        return fonts.bold->CalcTextSizeA(label, FLT_MAX, 0, text.c_str()).x;
    };
    float width = spacing * static_cast<float>(hints.size() - 1);
    for (const auto& hint : hints) width += glyph(nullptr, fonts, ImVec2(), hint.button) + gap + text_width(hint.label);
    // In the margin under every page and below PLAY, right-aligned, clear of
    // the window's rounded bottom corner.
    const float y = size.y - S(16);
    float x = size.x - S(28) - width;
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(x - S(9), y - S(10)), ImVec2(x + width + S(9), y + S(10)), rgba(0, 0, 0, 0.55f), S(10));
    for (const auto& hint : hints) {
        x += glyph(draw, fonts, ImVec2(x, y), hint.button) + gap;
        draw->AddText(fonts.bold, label, ImVec2(x, y - label * 0.5f), color::text, hint.label.c_str());
        x += text_width(hint.label) + spacing;
    }
}

} // namespace dingosdk::launcher_gui::detail
