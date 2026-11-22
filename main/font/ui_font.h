/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：独立的内建无衬线界面字形，不随阅读正文字体切换。
 * English: Separate built-in sans UI glyphs, unaffected by reading-font changes.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "epdiy.h"

bool ui_font_has_text(const char *text);
/// UI 主任务在没有字形借用时清空可重建位图，保留内置字体信息。
/// The UI task may clear rebuildable bitmaps when no glyph is borrowed, retaining built-in font metadata.
size_t ui_font_cache_clear(void);
/// UI 任务盘点仍持有的字形分配；不初始化或生成字形，回调不得修改或分配。
/// Visit live glyph allocations on the UI task without initializing/rasterizing; callbacks must not mutate or allocate.
void ui_font_visit_allocations(void (*visit)(void *context, const void *allocation), void *context);
int ui_font_ascender_px(int px);
/// 测宽不生成字形位图或创建字形缓存。/ Measure advances without rasterizing or creating the glyph cache.
int ui_font_text_width_px(int px, const char *text);
/// 标题里是否有字只存在于补充位图：有则整行走补充基准字号，没有则按轮廓字号生成。
/// Whether any title glyph lives only in the bitmap supplement: such a line takes the supplement's
/// base size, every other line rasterizes at the target size.
bool ui_font_title_uses_supplement(const char *text);
/// 含补充位图的标题整行使用原生24px；轮廓字形直接按目标字号生成。
/// Titles containing bitmap supplements use native 24px throughout; outlines rasterize at the target size.
int ui_font_title_px(int preferred_px, const char *text);
/// 标题统一使用黑色笔画；补充位图不插值，轮廓按相同覆盖率门限绘制。
/// Titles use solid black strokes, unscaled supplements and matching outline coverage thresholds.
void ui_font_draw_title_px(uint8_t *fb, int x, int baseline, int px,
                          const char *text, enum EpdFontFlags align);
/// 以实际字形测量基线上下范围，不分配位图。/ Measure actual glyph extents above/below baseline without bitmap allocation.
void ui_font_measure_line_px(int px, const char *text, int *above, int *below);
void ui_font_draw_text_px(uint8_t *fb, int x, int baseline, int px,
                          const char *text, enum EpdFontFlags align,
                          uint8_t fg, uint8_t bg, bool black_white);
