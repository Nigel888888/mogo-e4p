/*
 * 中文：无堆分配的扫描偏移与两位驱动行平移。
 * English: Allocation-free scan offsets and packed two-bit drive translation.
 */
#include "epd_output_offset.h"
#include <string.h>

static int s_x, s_y;
void epd_set_output_offset(int x, int y) { s_x = x; s_y = y; }
void epd_get_output_offset(int *x, int *y) { *x = s_x; *y = s_y; }
static uint8_t offset_pixel(const uint8_t *fb, int width, int height, int x, int y) {
    if (x < 0 || x >= width || y < 0 || y >= height) return 15;
    uint8_t packed = fb[(size_t)y * (width / 2) + x / 2];
    return (packed >> ((x & 1) * 4)) & 15;
}
bool epd_output_offset_difference(const uint8_t *front, const uint8_t *back,
    uint8_t *difference, int width, int height, int old_x, int old_y, int new_x, int new_y) {
    bool changed = false;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        uint8_t from = offset_pixel(back, width, height, x - old_x, y - old_y);
        uint8_t to = offset_pixel(front, width, height, x - new_x, y - new_y);
        difference[(size_t)y * width + x] = (to << 4) | from;
        changed |= from != to;
    }
    return changed;
}
void epd_output_shift_line(uint8_t *line, int bytes, int pixels, int msb_first) {
    if (!pixels || bytes <= 0) return;
    if (pixels >= bytes * 4 || pixels <= -bytes * 4) {
        memset(line, 0, (size_t)bytes);
        return;
    }
    int distance = pixels < 0 ? -pixels : pixels;
    int whole = distance / 4, bits = (distance % 4) * 2;
    for (int i = pixels > 0 ? bytes - 1 : 0;
         i >= 0 && i < bytes; i += pixels > 0 ? -1 : 1) {
        int a = pixels > 0 ? i - whole : i + whole;
        int b = pixels > 0 ? a - 1 : a + 1;
        unsigned first = a >= 0 && a < bytes ? line[a] : 0;
        unsigned second = b >= 0 && b < bytes ? line[b] : 0;
        if (!bits) line[i] = (uint8_t)first;
        else if ((pixels > 0) == !!msb_first)
            line[i] = (uint8_t)((first >> bits) | (second << (8 - bits)));
        else line[i] = (uint8_t)((first << bits) | (second >> (8 - bits)));
    }
}
