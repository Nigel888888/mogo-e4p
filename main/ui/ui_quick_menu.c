/* SPDX-License-Identifier: Apache-2.0
 * 中文：顶部快捷栏，最多两排、每排四个无文字圆钮；项目显隐与顺序由装配层提供。
 * 用户2026-11-16修订：取代按页面能力追加与固定键位，旋转键默认在各页常驻；
 * 不支持旋转的页面显示提示，不改变阅读会话。用户可在设置中选择项目和位置。
 * English: A top quick bar with at most two rows of four unlabeled circles, whose
 * visibility and order come from the layer owner. The user's 2026-11-16 revision
 * replaces page-specific appended keys and fixed positions: rotation is resident
 * by default, unavailable pages show a notice without changing the reader session,
 * and Settings manages the visible items and their positions.
 * 前光开关展开亮度和色温胶囊；熄灯收起。灯灭时停顿后继续下拉，或再次下拉，
 * 可仅在本次开层临时展开；连续快滑不连触发。横屏圆键在左、滑杆在右。
 * The light switch expands brightness/temperature capsules and switching off
 * collapses them. While off, a paused continuation or fresh pull expands them
 * for this opening only; a continuous flick never chains both actions. Landscape
 * puts circles on the left and sliders on the right.
 * WiFi、蓝牙、移动网络、震动和内存长按500ms进入对应设置，抬手不再触发短按。
 * Holding WiFi, Bluetooth, cellular, haptics or memory for 500ms opens its settings
 * without also activating the release tap.
 * 冻结：识别器只产出动作及前光事件，不访问页面、broker、硬件、设置或刷新；
 * 不申请帧缓存。绘图与命中使用同一项目顺序和几何，主循环负责调用绘图。
 * Frozen: the recognizer only emits actions and frontlight events, never accesses
 * pages, broker, hardware, settings or presentation, and allocates no framebuffer.
 * Painting and hits share item order and geometry; the loop owns drawing calls.
 */
#include "ui_quick_menu.h"
#include <stdlib.h>
#include "ui_kit.h"
#include "ui_nav.h"

static int center_x(const ui_quick_menu_t *menu, int button) {
    return ui_quick_menu_button_x(menu, button, epd_rotated_display_width());
}
static EpdRect key_rect(const ui_quick_menu_t *menu, int button) {
    return (EpdRect){center_x(menu,button)-UI_QUICK_RADIUS,ui_quick_menu_button_y(menu,button)-UI_QUICK_RADIUS,
                     2*UI_QUICK_RADIUS,2*UI_QUICK_RADIUS};
}
static const ui_icon_t s_icons[]={UI_ICON_WIFI,UI_ICON_BLUETOOTH,UI_ICON_REFRESH_CW};

static bool layer_pixel(int x, int y, int width, int height) {
    const int r = 56;
    if (y < height - r || (x >= r && x < width - r)) return true;
    int dx = x < r ? x - r : x - (width - r - 1);
    int dy = y - (height - r - 1);
    return dx * dx + dy * dy < r * r;
}

// 顺时针圆箭头围住锁头：开锁表示跟随重力，闭锁表示固定方向。
// A clockwise circular arrow surrounds the lock: open follows gravity, closed holds orientation.
static void rotation_icon(uint8_t *fb, int cx, int cy, bool unlocked) {
    cx -= 2;
    for (int y = -23; y <= 23; ++y) for (int x = -23; x <= 23; ++x) {
        const int radius2 = x * x + y * y;
        if (radius2 >= 19 * 19 && radius2 <= 23 * 23 &&
            !(x > 0 && y >= 0 && y < x * 2))
            epd_draw_pixel(cx + x, cy + y, UI_GRAY_BLACK, fb);
    }
    for (int y = 0; y <= 12; ++y) {
        const int half = (12 - y) * 8 / 12;
        ui_fill_rect_fast((EpdRect){cx + 22 - half, cy - 5 + y, 2 * half + 1, 1}, UI_GRAY_BLACK, fb);
    }
    ui_fill_round_rect(fb, (EpdRect){cx - 8, cy - 1, 17, 14}, 2, UI_GRAY_BLACK);
    for (int y = -7; y <= 0; ++y) for (int x = -7; x <= 7; ++x) {
        const int radius2 = x * x + y * y;
        if (radius2 >= 4 * 4 && radius2 <= 7 * 7)
            epd_draw_pixel(cx + x, cy - 7 + y, UI_GRAY_BLACK, fb);
    }
    ui_fill_rect_fast((EpdRect){cx - 7, cy - 7, 4, 7}, UI_GRAY_BLACK, fb);
    if (!unlocked) ui_fill_rect_fast((EpdRect){cx + 4, cy - 7, 4, 7}, UI_GRAY_BLACK, fb);
}

/* ---- 圆键 / Circle keys ---- */

static void draw_badge(uint8_t *fb,const ui_quick_menu_t *menu,int button,bool active) {
    const app_quick_item_t item = ui_quick_menu_item(menu, button);
    const bool status_key = item == APP_QUICK_WIFI || item == APP_QUICK_BLUETOOTH || button == ui_quick_menu_cellular_button(menu) ||
        button == ui_quick_menu_memory_button(menu);
    if(!status_key || !active)return;
    int cx=center_x(menu,button);
    const int cy = ui_quick_menu_button_y(menu, button);
    epd_fill_circle(cx+30,cy-30,5,UI_GRAY_WHITE,fb);
    epd_draw_circle(cx+30,cy-30,5,UI_GRAY_BLACK,fb);
}

// 圆键只用固定灰活动态和白色普通态，按压增加黑色内圈。
// Keys use fixed gray when active and white otherwise; pressing adds a black inner ring.
static void draw_key(uint8_t *fb,int button,const ui_quick_menu_t *menu,bool active,bool pressed) {
    int cx=center_x(menu,button);
    const int cy=ui_quick_menu_button_y(menu,button);
    const app_quick_item_t item = ui_quick_menu_item(menu, button);
    const bool frontlight_key=item==APP_QUICK_FRONTLIGHT;
    epd_fill_circle(cx,cy,UI_QUICK_RADIUS,active?UI_GRAY_LIGHT:UI_GRAY_WHITE,fb);
    epd_draw_circle(cx,cy,UI_QUICK_RADIUS,UI_GRAY_BLACK,fb);
    epd_draw_circle(cx,cy,UI_QUICK_RADIUS-1,UI_GRAY_BLACK,fb);
    if(item<=APP_QUICK_FULL) ui_draw_icon(fb,cx,cy,44,s_icons[item],UI_GRAY_BLACK);
    else if (button == ui_quick_menu_cellular_button(menu)) ui_draw_icon(fb,cx,cy,44,UI_ICON_MOGO_CELLULAR_SIGNAL,UI_GRAY_BLACK);
    else if(frontlight_key) ui_draw_icon(fb,cx,cy,44,UI_ICON_SUN,UI_GRAY_BLACK);
    else if (button == ui_quick_menu_memory_button(menu)) ui_draw_icon(fb,cx,cy,44,UI_ICON_CPU,UI_GRAY_BLACK);
    else if (button == ui_quick_menu_lock_button(menu)) ui_draw_icon(fb,cx,cy,44,UI_ICON_LOCK,UI_GRAY_BLACK);
    else if (button == ui_quick_menu_haptics_button(menu)) ui_draw_icon(fb,cx,cy,44,UI_ICON_VIBRATE,UI_GRAY_BLACK);
    else if (button == ui_quick_menu_rotation_button(menu)) rotation_icon(fb,cx,cy,menu->rotation_enabled);
    else ui_draw_icon(fb,cx,cy,44,UI_ICON_LOCK,UI_GRAY_BLACK);
    if(pressed){
        epd_draw_circle(cx,cy,UI_QUICK_RADIUS-7,UI_GRAY_BLACK,fb);
        epd_draw_circle(cx,cy,UI_QUICK_RADIUS-8,UI_GRAY_BLACK,fb);
    }
}

/* ---- 前光胶囊 / Frontlight capsules ---- */

// 胶囊在开灯或本次开层下拉临时展开时存在：层高随之伸缩，熄灯点按即收起。
// The capsules exist while the light is on or the layer was pulled open this
// session: the layer height follows, and the light-off tap collapses them.
static bool fl_rows_visible(const ui_quick_menu_t *menu) {
    return menu != NULL && menu->frontlight && (menu->fl_on || menu->fl_expanded);
}
static EpdRect fl_row_rect(const ui_quick_menu_t *menu, ui_quick_control_t control) {
    // 行矩形只作为局刷范围：胶囊整片自绘，行内胶囊以外的像素保持遮罩不动。
    // The row rectangle is only the local refresh scope: the capsule paints its
    // whole body, so pixels outside it in the row keep the shade untouched.
    const EpdRect track = ui_quick_menu_frontlight_track(menu, control);
    if (track.width <= 0) return (EpdRect){0};
    return (EpdRect){track.x - 8, track.y - 8, track.width + 16, UI_QUICK_FL_ROW_H};
}
static int fl_thumb_x(EpdRect track, uint8_t value) {
    return track.x + (track.width * value) / 100;
}

// 左端半圆帽在当前行的内缩量；整数解且偏保守，填充不出轨道的胶囊端。
// Per-row inset of the left semicircular cap; integer and conservative, so the fill never leaves the capsule end.
static int cap_inset(int offset) {
    const int r = UI_QUICK_FL_TRACK_H / 2;
    int chord = 0;
    while ((chord + 1) * (chord + 1) + offset * offset <= r * r) ++chord;
    return r - chord;
}

// 胶囊使用黑底白色填充与固定白底图标，移动时只有黑白像素改变。
// Capsules use black tracks, white fills and fixed white icon disks; motion changes black/white pixels only.
static void draw_capsule(uint8_t *fb, EpdRect track, ui_icon_t icon, uint8_t value) {
    const int r = UI_QUICK_FL_TRACK_H / 2;
    const int cy = track.y + r, icon_cx = track.x + UI_QUICK_FL_ICON_CX - UI_QUICK_FL_TRACK_X;
    ui_fill_round_rect_raw(fb, track, r, UI_GRAY_BLACK);
    const int width = fl_thumb_x(track, value) - track.x;
    if (width >= UI_QUICK_FL_TRACK_H) {
        ui_fill_round_rect_raw(fb, (EpdRect){track.x, cy - r, width, UI_QUICK_FL_TRACK_H}, r, UI_GRAY_WHITE);
    } else if (width > 0) {
        // 不足一个帽宽的填充按行收进左端半圆，与轨道的圆端保持同形。
        // A fill narrower than one cap tucks into the left cap row by row, matching the track's rounded end.
        for (int dy = 0; dy < UI_QUICK_FL_TRACK_H; ++dy) {
            const int inset = cap_inset(dy < r ? r - dy : dy - r);
            if (inset >= width) continue;
            ui_fill_rect_fast((EpdRect){track.x + inset, cy - r + dy, width - inset, 1},
                             UI_GRAY_WHITE, fb);
        }
    }
    epd_fill_circle(icon_cx, cy, UI_QUICK_FL_ICON_SIZE / 2 + 8, UI_GRAY_WHITE, fb);
    ui_draw_icon(fb, icon_cx, cy, UI_QUICK_FL_ICON_SIZE, icon, UI_GRAY_BLACK);
    ui_draw_round_rect_raw(fb, track, r, UI_GRAY_BLACK);
    ui_draw_round_rect_raw(fb, ui_inset_rect(track, 1), r - 1, UI_GRAY_BLACK);
}

static void draw_fl_row_at(uint8_t *fb, const ui_quick_menu_t *menu, ui_quick_control_t control, uint8_t value) {
    // 亮度用半填充圆图标，与前光开关的太阳明显不同；色温用温度计。
    // Brightness uses the half-filled circle, clearly unlike the sun switch; colour temperature uses the thermometer.
    if (control == UI_QUICK_CTL_BRIGHTNESS)
        draw_capsule(fb, ui_quick_menu_frontlight_track(menu, control), UI_ICON_CONTRAST, value);
    else if (control == UI_QUICK_CTL_CCT)
        draw_capsule(fb, ui_quick_menu_frontlight_track(menu, control), UI_ICON_THERMOMETER, value);
}

static void draw_fl_row(uint8_t *fb, const ui_quick_menu_t *menu, ui_quick_control_t control) {
    draw_fl_row_at(fb, menu, control,
                   control == UI_QUICK_CTL_BRIGHTNESS ? menu->fl_bri : menu->fl_cct);
}

void ui_quick_menu_end(void) {}

bool ui_quick_menu_draw_button(uint8_t *fb,const ui_quick_menu_t *menu,int button,bool active,bool pressed,EpdRect *dirty) {
    if(!fb||!dirty||button<0||button>=ui_quick_menu_button_count(menu)||epd_rotated_display_width()<UI_LOCK_WIDTH||epd_rotated_display_height()<ui_quick_menu_height(menu))return false;
    if(menu!=NULL&&ui_quick_menu_item(menu,button)==APP_QUICK_FRONTLIGHT)active=menu->fl_on;
    if(menu!=NULL&&button==ui_quick_menu_rotation_button(menu))active=menu->rotation_enabled;
    if(menu!=NULL&&button==ui_quick_menu_memory_button(menu))active=menu->memory_busy;
    draw_key(fb,button,menu,active,pressed);
    draw_badge(fb,menu,button,active);
    *dirty=key_rect(menu,button);
    return true;
}

bool ui_quick_menu_draw_control(uint8_t *fb, const ui_quick_menu_t *menu,
                                ui_quick_control_t control, EpdRect *dirty) {
    if (!fb || !fl_rows_visible(menu) || !dirty) return false;
    const EpdRect row = fl_row_rect(menu, control);
    if (row.width <= 0) return false;
    draw_fl_row(fb, menu, control);
    *dirty = row;
    return true;
}

bool ui_quick_menu_draw_control_white(uint8_t *fb, const ui_quick_menu_t *menu,
                                      ui_quick_control_t control, EpdRect *dirty) {
    if (!fb || !fl_rows_visible(menu) || !dirty) return false;
    const EpdRect row = fl_row_rect(menu, control);
    if (row.width <= 0 || control < UI_QUICK_CTL_BRIGHTNESS) return false;
    // 定稿第一段：白填充长到本次弄脏的上沿，底色区域先归一到已知白。
    // Settle stage one: grow the white fill to the dirty extent so the body
    // region is normalized to known white first.
    draw_fl_row_at(fb, menu, control,
                   control == UI_QUICK_CTL_CCT ? menu->fl_cct_dirty : menu->fl_bri_dirty);
    *dirty = row;
    return true;
}

void ui_quick_menu_draw_status(uint8_t *fb) {
    ui_fill_rect_fast((EpdRect){0,0,epd_rotated_display_width(),76},UI_GRAY_WHITE,fb);
    ui_nav_status(fb);
}

bool ui_quick_menu_draw_notice(uint8_t *fb, const ui_quick_menu_t *menu, EpdRect *dirty) {
    if (!fb || !menu || !dirty) return false;
    *dirty = (EpdRect){40, ui_quick_menu_height(menu) - 50, epd_rotated_display_width() - 80, 32};
    ui_fill_rect_fast(*dirty, UI_GRAY_WHITE, fb);
    if (menu->network_notice || menu->rotation_notice)
        ui_text_fixed_vc(fb, epd_rotated_display_width() / 2, dirty->y + 16, 20,
            menu->network_notice ? menu->network_notice : "仅阅读正文支持自动旋转", EPD_DRAW_ALIGN_CENTER, false);
    return true;
}

EpdRect ui_quick_menu_rect(const ui_quick_menu_t *menu) {
    return (EpdRect){0, 0, epd_rotated_display_width(), ui_quick_menu_height(menu)};
}

void ui_quick_menu_draw(uint8_t *fb,const ui_quick_menu_t *menu,bool wifi,bool bluetooth) {
    if(!fb)return;
    const int width = epd_rotated_display_width();
    const int height = epd_rotated_display_height();
    const int layer_height = ui_quick_menu_height(menu);
    // 不透明白底不混合底页，避免叠层产生额外灰阶。
    // Opaque white never blends the underlay into additional gray levels.
    for (int y = 0; y < layer_height && y < height; ++y) for (int x = 0; x < width; ++x)
        if (layer_pixel(x, y, width, layer_height)) epd_draw_pixel(x, y, UI_GRAY_WHITE, fb);
    // 状态栏留清晰白底，沿用系统时间/签名/电池；四图标与底栏共用Lucide细线44px。
    // Keep the system time/signature/battery legible on white; share navigation's 44px Lucide line icons.
    ui_quick_menu_draw_status(fb);
    for (int i = 0; i < ui_quick_menu_button_count(menu); ++i) {
        const app_quick_item_t item = ui_quick_menu_item(menu, i);
        bool active = (item == APP_QUICK_WIFI && wifi) || (item == APP_QUICK_BLUETOOTH && bluetooth) ||
            (i == ui_quick_menu_cellular_button(menu) && menu->cellular_enabled) ||
            (item == APP_QUICK_FRONTLIGHT && menu->fl_on) ||
            (i == ui_quick_menu_rotation_button(menu) && menu->rotation_enabled) ||
            (i == ui_quick_menu_haptics_button(menu) && menu->haptics_enabled) ||
            (i == ui_quick_menu_memory_button(menu) && menu->memory_busy);
        draw_key(fb, i, menu, active, false);
        draw_badge(fb, menu, i, active);
    }
    // 胶囊滑杆只在开灯或本次开层下拉展开时出现，直接浮在遮罩上；熄灯点按或
    // 没有前光的板只保留圆键区；阅读时额外留出第二排。
    // The capsule sliders appear while the light is on or the layer was pulled
    // open this session, floating on the shade; the light-off tap and boards
    // without a frontlight keep only the circles, including the second reader row.
    if (fl_rows_visible(menu)) {
        draw_fl_row(fb, menu, UI_QUICK_CTL_BRIGHTNESS);
        draw_fl_row(fb, menu, UI_QUICK_CTL_CCT);
    }
    if (menu->rotation_notice || menu->network_notice) { EpdRect dirty; (void)ui_quick_menu_draw_notice(fb, menu, &dirty); }
    ui_fill_round_rect(fb, (EpdRect){width / 2 - 26, layer_height - 24, 52, 6}, 3, UI_GRAY_BLACK);
    // 只画遮罩内的双像素轮廓，顶部直角、底部圆角，不越过下层边界。
    // Draw a two-pixel outline within the mask: square top, rounded bottom, no underlay leakage.
    for (int y = 0; y < layer_height && y < height; ++y) for (int x = 0; x < width; ++x) {
        if (layer_pixel(x, y, width, layer_height) && (x < 2 || x >= width - 2 || y < 2 ||
            y >= layer_height - 2 || !layer_pixel(x - 2, y, width, layer_height) ||
            !layer_pixel(x + 2, y, width, layer_height) || !layer_pixel(x, y + 2, width, layer_height)))
            epd_draw_pixel(x, y, UI_GRAY_BLACK, fb);
    }
}
