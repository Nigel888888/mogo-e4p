/* SPDX-License-Identifier: Apache-2.0
 * 显示硬件主机替身。/ Display hardware host shim.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { int x, y, width, height; } EpdRect;
#include "../../components/epdiy/include/epd_waveform.h"
enum EpdDrawMode { MODE_DU = 1, MODE_GC16 = 2, MODE_GL16 = 5,
                  MODE_PACKING_1PPB_DIFFERENCE = 0x100 };
enum EpdRotation { EPD_ROT_LANDSCAPE, EPD_ROT_PORTRAIT,
                   EPD_ROT_INVERTED_LANDSCAPE, EPD_ROT_INVERTED_PORTRAIT };
enum EpdRotation epd_get_rotation(void);
void epd_set_rotation(enum EpdRotation rotation);
enum EpdDrawError { EPD_DRAW_SUCCESS = 0, EPD_DRAW_EMPTY_LINE_QUEUE = 1,
                    EPD_DRAW_OTHER_ERROR = 2, EPD_DRAW_NO_PHASES_AVAILABLE = 4,
                    EPD_DRAW_INVALID_CROP = 0x40 };
void epd_poweron(void);
void epd_poweroff(void);
void epd_clear(void);
void epd_clear_area_cycles(EpdRect area, int cycles, int cycle_time);
EpdRect epd_full_screen(void);
void epd_push_pixels(EpdRect area, short time, int color);
void epd_lcd_set_prefill_lines(int lines);
int epd_rotated_display_width(void);
int epd_rotated_display_height(void);
int epd_width(void);
int epd_height(void);

void epd_fill_rect(EpdRect area, uint8_t color, uint8_t *fb);
void epd_clear_phase_luts(void);
void epd_leading_skip_discard(void);
void epd_leading_skip_forget_waveform(void);
enum EpdDrawError epd_draw_base(EpdRect area, const uint8_t *data, EpdRect crop,
    enum EpdDrawMode mode, int temperature, const bool *lines,
    const uint8_t *columns, const EpdWaveform *waveform);

bool epd_power_scan_allowed(void);
void epd_power_session_begin(void);
void epd_power_session_end(void);
bool epd_flash_begin(void);
void epd_flash_end(void);
