/* 中文：同一轮 GL16 内合并请求区域差分和静态补偿，阅读翻页额外补白。
 * English: Merge requested differences and static reinforcement into one GL16 scan, also maintaining whites on reader turns.
 */
#include "display_static_black.h"
#include "e0470_epaper_waveform.h"
#include "e0470_white_field.h"
#include "esp_heap_caps.h"

#include <string.h>

enum { STATIC_WHITE_CODE = 0xdd, STATIC_WHITE_SLOT = 13 };

static unsigned pixel(const uint8_t *row, int x) {
    return (row[x / 2] >> (4 * (x & 1))) & 15;
}

bool display_static_black_native(EpdRect a, EpdRect *native) {
    int w = epd_width(), h = epd_height();
    if (!native || w <= 0 || h <= 0 || (w & 7) || a.x < 0 || a.y < 0 || a.width <= 0 || a.height <= 0 ||
        a.width > epd_rotated_display_width() - a.x || a.height > epd_rotated_display_height() - a.y)
        return false;
    switch (epd_get_rotation()) {
        case EPD_ROT_PORTRAIT: a = (EpdRect){w - a.y - a.height, a.x, a.height, a.width}; break;
        case EPD_ROT_INVERTED_PORTRAIT: a = (EpdRect){a.y, h - a.x - a.width, a.height, a.width}; break;
        case EPD_ROT_INVERTED_LANDSCAPE: a.x = w - a.x - a.width; a.y = h - a.y - a.height; break;
        case EPD_ROT_LANDSCAPE: break;
        default: return false;
    }
    if (a.x < 0 || a.y < 0 || a.width > w - a.x || a.height > h - a.y) return false;
    *native = a;
    return true;
}

bool display_static_black_changed(const EpdiyHighlevelState *hl, EpdRect a, EpdRect *native) {
    if (!hl || !hl->front_fb || !hl->back_fb || !hl->difference_fb || !display_static_black_native(a, native))
        return false;
    a = *native;
    const int w = epd_width();
    const int x1 = a.x + a.width, first = (a.x + 1) / 2, end = x1 / 2;
    for (int y = a.y; y < a.y + a.height; ++y) {
        size_t offset = (size_t)y * (w / 2);
        const uint8_t *front = hl->front_fb + offset, *back = hl->back_fb + offset;
        if (((a.x & 1) && pixel(front, a.x) != pixel(back, a.x)) ||
            ((x1 & 1) && pixel(front, x1 - 1) != pixel(back, x1 - 1)) ||
            (end > first && memcmp(front + first, back + first, (size_t)(end - first)))) return true;
    }
    return false;
}

static bool pack_frame(EpdiyHighlevelState *hl, EpdRect a, int w, int h,
                       bool stable_only, bool full_body, uint8_t hold, const EpdRect *chrome,
                       bool reinforce_black, bool reinforce_white, bool *changed) {
    bool outside_drive = false;
    const int x1 = a.x + a.width, y1 = a.y + a.height, stride = w / 2;
    for (int y = 0; y < h; ++y) {
        size_t offset = (size_t)y * w;
        const uint8_t *front = hl->front_fb + offset / 2, *back = hl->back_fb + offset / 2;
        uint8_t *diff = hl->difference_fb + offset;
        const bool inside_row = y >= a.y && y < y1;
        E0470WhiteField paper = e0470_white_field_row(hl->back_fb, hl->front_fb, stride, h, y);
        int start0 = 0, end0 = 0, start1 = 0, end1 = 0;
        if (chrome) {
            if (y >= chrome[0].y && y < chrome[0].y + chrome[0].height) {
                start0 = chrome[0].x; end0 = start0 + chrome[0].width;
            }
            if (y >= chrome[1].y && y < chrome[1].y + chrome[1].height) {
                start1 = chrome[1].x; end1 = start1 + chrome[1].width;
            }
        }
        for (int x = 0; x < w; ++x) {
            const unsigned old = pixel(back, x);
            const bool body = inside_row && x >= a.x && x < x1;
            const bool bar = (x >= start0 && x < end0) || (x >= start1 && x < end1);
            const unsigned target = body || bar || stable_only ? pixel(front, x) : old;
            const bool body_equal = body && full_body && target != 0 &&
                !(reinforce_white && target == STATIC_WHITE_SLOT);
            if (((body || bar) && target != old) || body_equal) {
                diff[x] = (uint8_t)((target << 4) | old);
                *changed |= target != old;
            } else {
                const bool black = !body && reinforce_black && old == 0 && (!stable_only || target == 0);
                const bool white = !body && reinforce_white && (old & target) == 15 &&
                    e0470_white_field_contains(&paper, x);
                diff[x] = black ? 0 : white ? STATIC_WHITE_CODE : hold;
                outside_drive |= black || white;
            }
        }
    }
    return outside_drive;
}

static void commit_area(EpdiyHighlevelState *hl, EpdRect a, int w) {
    const int x1 = a.x + a.width, first = (a.x + 1) / 2, end = x1 / 2;
    for (int y = a.y; y < a.y + a.height; ++y) {
        size_t offset = (size_t)y * (w / 2);
        const uint8_t *front = hl->front_fb + offset;
        uint8_t *back = hl->back_fb + offset;
        if (a.x & 1) back[a.x / 2] = (back[a.x / 2] & 0x0f) | (front[a.x / 2] & 0xf0);
        if (end > first) memcpy(back + first, front + first, (size_t)(end - first));
        if (x1 & 1) back[end] = (back[end] & 0xf0) | (front[end] & 0x0f);
    }
}

static bool draw(EpdiyHighlevelState *hl, const EpdWaveform *waveform,
                               EpdRect a, bool stable_only, bool full_body,
                               unsigned black_pushes, const EpdRect *chrome, enum EpdDrawError *result) {
    if ((!black_pushes && !chrome) || !waveform || waveform->num_temp_ranges != 1 || !waveform->temp_intervals) return false;
    const EpdWaveformPhases *base = e0470_waveform_phases(waveform, MODE_GL16);
    if (!base || !base->luts || base->phases <= 0 || base->phases > 64) return false;
    // 借用最后一个黑转白相补静态白；背景正文的 FF 仍走原表。
    // Reuse the last black-to-white push for static white; preserve the body's original FF action over backgrounds.
    int white_phase = -1;
    if (stable_only && black_pushes) {
        for (int f = base->phases - 1; f >= 0; --f) {
            if (e0470_phase_action(base, f, 15, 0) == 2) {
                white_phase = f;
                break;
            }
        }
    }
    const uint8_t hold = full_body ? 0xee : 0xff;
    if (full_body) {
        for (int f = 0; f < base->phases; ++f)
            if (e0470_phase_action(base, f, 0, 0) || e0470_phase_action(base, f, 14, 14) ||
                (white_phase >= 0 && e0470_phase_action(base, f, STATIC_WHITE_SLOT, STATIC_WHITE_SLOT))) return false;
    }
    const int w = epd_width(), h = epd_height();
    if (a.width == w && a.height == h) return false;
    size_t bytes = (size_t)base->phases * 64;
    uint8_t (*lut)[16][4] = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lut) return false;
    memcpy(lut, base->luts, bytes);
    unsigned pushes = 0;
    // 00 和 DD 分别代表区外静态黑和静态白；有背景时，仅在原 DD 保持时借用该编码。
    // Reserve 00 and DD for static outside black and white; in the background table DD must originally hold.
    for (int f = base->phases - 1; f >= 0; --f) {
        if (!full_body) lut[f][15][3] &= 0xfc;
        if (white_phase >= 0) {
            uint8_t *slot = &lut[f][STATIC_WHITE_SLOT][STATIC_WHITE_SLOT / 4];
            const unsigned shift = 6 - 2 * (STATIC_WHITE_SLOT % 4);
            *slot = (*slot & ~(3u << shift)) | (f == white_phase ? 2u << shift : 0);
        }
        lut[f][0][0] &= 0x3f;
        if ((lut[f][0][3] & 3) == 1 && pushes < black_pushes) {
            lut[f][0][0] |= 0x40;
            ++pushes;
        }
    }
    bool changed = false;
    if (pushes != black_pushes) {
        heap_caps_free(lut);
        return false;
    }
    const bool outside_drive = pack_frame(hl, a, w, h, stable_only, full_body, hold, chrome,
                                          pushes > 0, white_phase >= 0, &changed);
    if ((!chrome && !outside_drive) || (chrome && !changed)) {
        heap_caps_free(lut);
        if (chrome) *result = EPD_DRAW_SUCCESS;
        return chrome != NULL;
    }
    const EpdWaveformPhases phases = {base->phases, (const uint8_t *)lut, base->phase_times};
    const EpdWaveformPhases *ranges[] = {&phases};
    const EpdWaveformMode mode = {MODE_GL16, 1, ranges};
    const EpdWaveformMode *modes[] = {&mode};
    const EpdWaveform scan = {.num_modes=1, .num_temp_ranges=1, .mode_data=modes,
                              .temp_intervals=waveform->temp_intervals};
    const EpdRect full = epd_full_screen();
    epd_leading_skip_discard();
    *result = epd_draw_base(full, hl->difference_fb, full, MODE_PACKING_1PPB_DIFFERENCE | MODE_GL16,
                            25, NULL, NULL, &scan);
    epd_leading_skip_forget_waveform();
    heap_caps_free(lut);
    if (*result == EPD_DRAW_SUCCESS) {
        commit_area(hl, a, w);
        if (chrome) for (int i = 0; i < 2; ++i)
            if (chrome[i].width > 0 && chrome[i].height > 0) commit_area(hl, chrome[i], w);
    }
    return true;
}

bool display_static_black_draw(EpdiyHighlevelState *hl, const EpdWaveform *waveform,
                               EpdRect a, bool stable_only, bool full_body,
                               unsigned black_pushes, enum EpdDrawError *result) {
    return draw(hl, waveform, a, stable_only, full_body, black_pushes, NULL, result);
}

bool display_static_black_join(EpdiyHighlevelState *hl, const EpdWaveform *waveform, EpdRect body,
                               const EpdRect chrome[2], bool full_body, unsigned black_pushes,
                               enum EpdDrawError *result) {
    return draw(hl, waveform, body, true, full_body, black_pushes, chrome, result);
}
