/* 中文：为阅读翻页补白筛出远离文字、图标和灰边的纯白像素。
 * English: Select paper whites clear of glyphs, icons and antialiased edges for reader-turn maintenance.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { E0470_WHITE_FIELD_MARGIN = 2 };

typedef struct {
    const uint8_t *back, *front;
    int stride, rows, pair;
    unsigned left, center, right;
} E0470WhiteField;

/// 行邻域裁到屏内。/ Clip the row neighborhood to the panel.
static inline E0470WhiteField e0470_white_field_row(const uint8_t *back, const uint8_t *front,
                                                   int stride, int height, int y) {
    const int margin = E0470_WHITE_FIELD_MARGIN;
    const int first = y >= margin ? y - margin : 0;
    const int end = y + margin < height ? y + margin + 1 : height;
    return (E0470WhiteField){.back = back + first * stride, .front = front + first * stride,
                             .stride = stride, .rows = end - first, .pair = -2};
}

static inline unsigned e0470_white_field_column(const E0470WhiteField *f, int pair) {
    if (pair < 0 || pair >= f->stride) return 0xff;
    unsigned bits = 0xff;
    for (int row = 0; row < f->rows; ++row, pair += f->stride)
        bits &= f->back[pair] & f->front[pair];
    return bits;
}

/// 两帧的 5×5 邻域均为白才允许补白；调用方按行从左到右复用列计算。
/// Allow whitening only when the 5×5 neighborhood is white in both frames; reuse columns on left-to-right scans.
static inline bool e0470_white_field_contains(E0470WhiteField *f, int x) {
    const int pair = x / 2;
    if (pair != f->pair) {
        if (pair == f->pair + 1) {
            f->left = f->center;
            f->center = f->right;
        } else {
            f->left = e0470_white_field_column(f, pair - 1);
            f->center = e0470_white_field_column(f, pair);
        }
        f->right = e0470_white_field_column(f, pair + 1);
        f->pair = pair;
    }
    return f->center == 0xff && ((x & 1) ? ((f->left & 0xf0) == 0xf0 && f->right == 0xff)
                                        : (f->left == 0xff && (f->right & 0x0f) == 0x0f));
}
