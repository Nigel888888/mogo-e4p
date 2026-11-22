/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "epdiy.h"
// 准备阶段先读TF成品缓存，缺失或失效才解码；绘制不读文件、不分配。
// Preparation loads prepared SD art before decoding misses; painting never reads files or allocates.
// 原尺寸4位背景单份最多415872B，正文不重缩放。
// Native-resolution 4-bit wallpaper (<=415872B); no second resident cache or per-page image rescale.
// 正文固定屏幕坐标，纸面白点归一后与预览共用同一份灰度。
// Blended reading backgrounds use fixed screen coordinates; paper is white-point clamped and preview shares the cached tones.
bool ui_reader_background_prepare(const char *path);
void ui_reader_background_release(void);
/// UI 安全边界访问唯一背景缓存；回调不得修改、分配或阻塞。
/// Visit the sole background cache at a UI safety boundary; the callback must not mutate, allocate or block.
void ui_reader_background_visit_allocations(void (*visit)(void *context, const void *allocation), void *context);
void ui_reader_background_paint(uint8_t *fb, EpdRect area, bool blend);
