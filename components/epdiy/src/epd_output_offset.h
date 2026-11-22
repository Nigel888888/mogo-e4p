/*
 * 中文：扫描出口平移；保留逻辑帧缓冲与波形，空出的扫描位置不驱动。
 * English: Translate scan output, preserving logical buffers and waveforms; exposed scan positions hold.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// 只在扫描停止时设置原生面板坐标偏移；调用方须清屏或按物理差分迁移真实基准。
/// Set native-panel offsets only between scans; callers must clear or migrate the actual baseline with physical differences.
void epd_set_output_offset(int x, int y);
/// 返回原生面板坐标偏移。/ Return native-panel offsets.
void epd_get_output_offset(int *x, int *y);
/// 原地平移一行两位驱动值；msb_first 对应 S31 的像素位序。
/// Shift packed two-bit drives in place; msb_first selects S31 pixel ordering.
void epd_output_shift_line(uint8_t *line, int bytes, int pixels, int msb_first);
/// 按新旧原生偏移生成物理差分，屏外取白；复用调用方的 1ppB 差分缓冲，返回是否有变化。
/// Build physical differences at old/new native offsets, taking outside pixels as white; reuse the caller's 1ppB buffer and report changes.
bool epd_output_offset_difference(const uint8_t *front, const uint8_t *back,
    uint8_t *difference, int width, int height, int old_x, int old_y, int new_x, int new_y);
