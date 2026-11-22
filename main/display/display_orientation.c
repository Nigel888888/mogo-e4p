/*
 * 中文：在显示边界封装 EPD 旋转编号与输入坐标，不访问页面或传感器。
 * English: Wrap EPD rotation numbering and input coordinates at the display boundary, without pages or sensors.
 */
#include "display_orientation.h"
#include "epdiy.h"
#include "epd_output_offset.h"

display_orientation_t display_orientation_get(void) {
    return (display_orientation_t)((epd_get_rotation() + 1) & 3);
}
void display_orientation_set(display_orientation_t orientation) {
    epd_set_rotation((enum EpdRotation)((orientation + 3) & 3));
}
bool display_orientation_touch(uint16_t *x, uint16_t *y) {
    const int width = epd_height(), height = epd_width();
    int px = *x, py = *y;
    if (px >= width || py >= height) return true;
    int native_x, native_y;
    epd_get_output_offset(&native_x, &native_y);
    px += native_y;
    py -= native_x;
    if (px < 0 || py < 0 || px >= width || py >= height) return false;
    *x = (uint16_t)px; *y = (uint16_t)py;
    switch (display_orientation_get()) {
        case DISPLAY_ORIENTATION_RIGHT: *x = py; *y = width - 1 - px; break;
        case DISPLAY_ORIENTATION_INVERTED: *x = width - 1 - px; *y = height - 1 - py; break;
        case DISPLAY_ORIENTATION_LEFT: *x = height - 1 - py; *y = px; break;
        default: break;
    }
    return true;
}
