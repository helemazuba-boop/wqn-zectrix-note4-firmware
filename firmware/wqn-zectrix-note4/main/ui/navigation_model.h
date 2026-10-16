#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "button_event.h"
#include "ui_screen.h"

namespace wqn {

struct NavigationPage {
    UiScreen screen;
    const char* label;
};

inline constexpr std::array<NavigationPage, 7> kNavigationPages = {{
    {UiScreen::kHome, "首页"}, {UiScreen::kTime, "时间"},
    {UiScreen::kWord, "单词"}, {UiScreen::kNote, "笔记"},
    {UiScreen::kTodo, "待办"}, {UiScreen::kAi, "AI"},
    {UiScreen::kSettings, "设置"},
}};

constexpr size_t NavigationPageIndex(UiScreen screen)
{
    for (size_t i = 0; i < kNavigationPages.size(); ++i) {
        if (kNavigationPages[i].screen == screen) return i;
    }
    return kNavigationPages.size();
}

constexpr UiScreen AdjacentNavigationPage(UiScreen screen, int direction)
{
    const size_t current = NavigationPageIndex(screen);
    if (current == kNavigationPages.size()) return UiScreen::kHome;
    return kNavigationPages[(current + kNavigationPages.size() + direction) %
                            kNavigationPages.size()].screen;
}

// Transient UI state; held/blocked masks consume gestures across modal and
// page boundaries. Only visible/selected contribute pixels to the frame.
struct NavigationState {
    bool visible = false;
    uint8_t selected = 0;
    uint8_t held_mask = 0;
    uint8_t blocked_mask = 0;
    bool guard_confirm_double = false;
    int64_t confirmed_at_ms = 0;
};

enum class NavigationAction { kPass, kConsumed, kRepaint, kConfirm };

NavigationAction ReduceNavigationInput(
    NavigationState* state, UiScreen screen, const ButtonEvent& event,
    int64_t event_time_ms, bool can_open);

}  // namespace wqn
