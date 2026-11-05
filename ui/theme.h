// One place for every colour and measurement the panel draws with.
#pragma once

#include <d2d1.h>

namespace theme {

constexpr D2D1_COLOR_F rgb(unsigned int hex, float alpha = 1.0f) {
    return D2D1_COLOR_F{
        ((hex >> 16) & 0xFF) / 255.0f,
        ((hex >> 8) & 0xFF) / 255.0f,
        (hex & 0xFF) / 255.0f,
        alpha};
}

constexpr D2D1_COLOR_F bg = rgb(0x141216);
constexpr D2D1_COLOR_F panel = rgb(0x1C1A20);
constexpr D2D1_COLOR_F bubble = rgb(0x26232B);
constexpr D2D1_COLOR_F bubble_user = rgb(0x2E2A38);
constexpr D2D1_COLOR_F text = rgb(0xECEAEF);
constexpr D2D1_COLOR_F dim = rgb(0x8A8593);
constexpr D2D1_COLOR_F accent = rgb(0x2DD4BF);
constexpr D2D1_COLOR_F accent_ink = rgb(0x0B2B28);
constexpr D2D1_COLOR_F chip_fill = rgb(0xFFFFFF, 0.05f);
constexpr D2D1_COLOR_F scrim = rgb(0x000000, 0.55f);

constexpr float gutter = 12.0f;
constexpr float settings_h = 56.0f;
constexpr float composer_h = 56.0f;
constexpr float chat_gap = 8.0f;
constexpr float bubble_pad_x = 14.0f;
constexpr float bubble_pad_y = 10.0f;
constexpr float avatar_band_ratio = 0.37f;
constexpr float bubble_max_ratio = 0.78f;
constexpr float trace_min_width = 248.0f;
constexpr float chip_h = 18.0f;
constexpr float tool_row_h = 15.0f;
constexpr float hit_row_h = 34.0f;

}  // namespace theme
