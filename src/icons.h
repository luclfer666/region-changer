#pragma once

#include "imgui.h"

namespace icons
{
    enum class tab : ImWchar
    {
        aimbot   = 0xE04E,
        visuals  = 0xE9CC,
        heroes   = 0xEB0F,
        exploits = 0xF21B,
        config   = 0xEA67,
        settings = 0xE0B7,
    };

    const char* to_utf8(tab icon);

    inline constexpr const char* aimbot   = "\xEE\x81\x8E";
    inline constexpr const char* visuals  = "\xEE\xA7\x8C";
    inline constexpr const char* heroes   = "\xEE\xAC\x8F";
    inline constexpr const char* exploits = "\xEF\x88\x9B";
    inline constexpr const char* config   = "\xEE\xA9\xA7";
    inline constexpr const char* settings = "\xEE\x82\xB7";

    inline constexpr const char* check    = "\xEE\x82\x9C";
    inline constexpr const char* keyboard = "\xEF\x84\x9C";
    inline constexpr const char* exit     = "\xEE\xAE\x92";
    inline constexpr const char* search   = "\xEE\x82\xB2";
    inline constexpr const char* folder   = "\xEE\xA9\xA7";
}
