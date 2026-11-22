/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 图片专用的稳定 16 级有序抖动；文字和界面线条不要经过这里。
 * Stable 16-level ordered dither for images only; UI text and rules bypass it.
 */
#pragma once

#include <stdint.h>

/// 把 8 位灰度映射到相邻的两个 4 位物理灰阶，坐标决定稳定的 4x4 Bayer 相位。
/// Map 8-bit gray to adjacent physical 4-bit levels with a stable 4x4 Bayer phase.
uint8_t ui_image_dither_gray(uint8_t gray, int x, int y);
/// 主页面黑白图片点阵；不会对阅读插图启用。/ Black/white image dots for main screens; not enabled for reader illustrations.
uint8_t ui_image_dither_bw(uint8_t gray, int x, int y);
/// 封面采样后压深中间调，保留黑白端点；最终灰阶或黑白量化由调用方完成。
/// Deepen cover midtones after sampling without shifting black or white endpoints; callers quantize to gray or BW.
static inline uint8_t ui_image_cover_tone(uint8_t gray, uint8_t contrast) {
    int value = 128 + ((int)gray - 128) * contrast / 100;
    if (value <= 0) return 0;
    if (value >= 255) return 255;
    return (uint8_t)(value - (5 * value * (255 - value) + 2048) / 4096);
}
/// 初版壁纸色调，供旧关机成品只补偿新增的浅灰调整。
/// First wallpaper curve, used to apply only the added highlight correction to older shutdown art.
static inline uint8_t ui_image_wallpaper_base_tone(uint8_t gray) {
    uint32_t g = gray;
    return (uint8_t)(g - (g * g * (255u - g) + 49152u) / 98304u);
}
/// 只对偏亮灰阶增加压深，向纯白平滑收回。/ Deepen lighter grays only, easing back to unchanged white.
static inline uint8_t ui_image_wallpaper_highlight_delta(uint8_t gray) {
    if (gray <= 160u) return 0;
    uint32_t hi = gray - 160u;
    return (uint8_t)((hi * hi * (255u - gray) + 8192u) / 16384u);
}
/// 在16级量化前拉开浅灰层次，纯黑与纯白不变。
/// Separate near-white wallpaper tones before 16-level quantization, keeping black and white fixed.
static inline uint8_t ui_image_wallpaper_tone(uint8_t gray) {
    return ui_image_wallpaper_base_tone(gray) - ui_image_wallpaper_highlight_delta(gray);
}
/// 亚克力用一像素规则棋盘格点阵，固定坐标且保持黑白端点，无额外内存。/ One-pixel regular checkerboard dots for acrylic, coordinate-stable and endpoint-preserving, with no extra memory.
uint8_t ui_image_dither_acrylic_bw(uint8_t gray, int x, int y);
