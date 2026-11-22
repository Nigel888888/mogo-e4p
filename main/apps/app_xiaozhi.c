/*
 * 中文：小智 AI 入口——显示 6 位设备码与绑定说明，保存小智 me / DeepSeek / 自定义 API 参数。
 * English: XiaoZhi AI entry: device code, binding hint and saved API configuration.
 */
#include "app.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_keyboard.h"
#include "ui_nav.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

extern const app_desc_t app_xiaozhi;

#define XZ_CFG_PATH "/sdcard/xiaozhi.cfg"
#define XZ_FIELDS 4
#define XZ_FIELD_CAP 320
#define XZ_ROW_Y 320
#define XZ_ROW_H 68
#define XZ_BTN_ME {36, 950, 200, 72}
#define XZ_BTN_DS {252, 950, 200, 72}
#define XZ_BTN_SAVE {468, 950, 180, 72}

typedef enum { XZ_MAIN, XZ_EDIT } xz_page_t;
static xz_page_t s_page;
static char s_cfg[XZ_FIELDS][XZ_FIELD_CAP];
static char s_edit_buf[XZ_FIELD_CAP];
static ui_text_edit_t s_edit;
static int s_edit_field;
static char s_notice[96];
static char s_code[8];

static const char *const kLabels[XZ_FIELDS] = {"平台", "接口地址", "API Key", "模型"};

static void make_code(void) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) memset(mac, 0x5a, sizeof(mac));
    unsigned long v = ((unsigned long)mac[3] << 16) | ((unsigned long)mac[4] << 8) | mac[5];
    snprintf(s_code, sizeof(s_code), "%06lu", v % 1000000ul);
}
static void load_cfg(void) {
    const char *def[XZ_FIELDS] = {"小智me", "https://api.xiaozhi.me", "", ""};
    for (int i = 0; i < XZ_FIELDS; ++i) snprintf(s_cfg[i], XZ_FIELD_CAP, "%s", def[i]);
    FILE *f = fopen(XZ_CFG_PATH, "r");
    if (!f) return;
    for (int i = 0; i < XZ_FIELDS; ++i) {
        char line[XZ_FIELD_CAP];
        line[0] = 0;
        if (fgets(line, sizeof(line), f)) {
            size_t n = strlen(line);
            while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
            if (n) snprintf(s_cfg[i], XZ_FIELD_CAP, "%s", line);
        }
    }
    fclose(f);
}
static bool save_cfg(void) {
    FILE *f = fopen(XZ_CFG_PATH, "w");
    if (!f) return false;
    for (int i = 0; i < XZ_FIELDS; ++i) fprintf(f, "%s\n", s_cfg[i]);
    int error = ferror(f);
    if (fclose(f) != 0) error = 1;
    return !error;
}
static void enter(app_ctx_t *ctx) { (void)ctx; make_code(); load_cfg(); s_page = XZ_MAIN; s_notice[0] = 0; }
static void leave(app_ctx_t *ctx) { (void)ctx; ui_keyboard_end(); }

static void masked(char *out, size_t cap, const char *v) {
    size_t n = strlen(v);
    if (n > 6) snprintf(out, cap, "%.4s****%s", v, v + n - 4);
    else snprintf(out, cap, "%s", v);
}

static void render_main(uint8_t *fb) {
    ui_clear_page(fb); ui_nav_status(fb);
    ui_text_fixed_vc(fb, 342, 116, 46, "小智 AI", EPD_DRAW_ALIGN_CENTER, false);
    char code[64]; snprintf(code, sizeof(code), "设备码  %s", s_code);
    ui_text_fixed_vc(fb, 342, 186, 42, code, EPD_DRAW_ALIGN_CENTER, false);
    ui_text_fixed_vc(fb, 342, 240, 24, "绑定：访问 xiaozhi.me → 控制台 → 添加设备", EPD_DRAW_ALIGN_CENTER, false);
    for (int i = 0; i < XZ_FIELDS; ++i) {
        EpdRect r = (EpdRect){36, XZ_ROW_Y + i * XZ_ROW_H, 612, XZ_ROW_H - 8};
        ui_draw_choice_round_rect(fb, r, 10, false);
        char value[XZ_FIELD_CAP];
        if (i == 2) masked(value, sizeof(value), s_cfg[i]); else snprintf(value, sizeof(value), "%s", s_cfg[i]);
        char line[2 * XZ_FIELD_CAP];
        snprintf(line, sizeof(line), "%s：%s", kLabels[i], value[0] ? value : "未设置");
        char shown[2 * XZ_FIELD_CAP];
        snprintf(shown, sizeof(shown), "%s", line);
        while (ui_text_fixed_width_px(26, shown) > 580 && strlen(shown) > 4) {
            size_t n = strlen(shown) - 1; while (n && ((unsigned char)shown[n] & 0xC0) == 0x80) --n; shown[n] = 0;
        }
        ui_text_fixed(fb, r.x + 16, r.y + 18, 26, shown, EPD_DRAW_ALIGN_LEFT, false);
    }
    ui_draw_button(fb, (EpdRect)XZ_BTN_ME, "小智me", false);
    ui_draw_button(fb, (EpdRect)XZ_BTN_DS, "DeepSeek", false);
    ui_draw_button(fb, (EpdRect)XZ_BTN_SAVE, "保存配置", false);
    if (s_notice[0]) ui_text_fixed_vc(fb, 342, 286, 24, s_notice, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_separator(fb, UI_NAV_TOP, 0, 684, 0x90);
    ui_draw_icon(fb, 342, 1164, 44, UI_ICON_HOUSE, 0);
}

static app_redraw_t edit_paint(app_ctx_t *ctx, bool field) {
    ui_keyboard_update_t u = ui_keyboard_update(ctx->fb, 560, (EpdRect){36, 243, 612, 82}, 28, s_edit_field == 2, NULL, field);
    return u.area.width ? APP_REDRAW_AREA : APP_REDRAW_NONE;
}
static void open_editor(int field) {
    s_edit_field = field;
    snprintf(s_edit_buf, sizeof(s_edit_buf), "%s", s_cfg[field]);
    ui_text_edit_init(&s_edit, s_edit_buf, sizeof(s_edit_buf));
    s_notice[0] = 0;
    ui_keyboard_begin(&s_edit, true);
    s_page = XZ_EDIT;
}
static void render_edit(uint8_t *fb) {
    ui_clear_page(fb); ui_nav_status(fb);
    char title[64]; snprintf(title, sizeof(title), "编辑 · %s", kLabels[s_edit_field]);
    ui_text_fixed_vc(fb, 342, 116, 42, title, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_button(fb, (EpdRect){36, 1000, 200, 72}, "返回", false);
    ui_draw_button(fb, (EpdRect){448, 1000, 200, 72}, "确定", false);
    ui_text_input_draw(fb, &s_edit, (EpdRect){36, 243, 612, 82}, 28, s_edit_field == 2, NULL);
    ui_keyboard_draw(fb, 560);
}
static void render(app_ctx_t *ctx, uint8_t *fb) { if (s_page == XZ_EDIT) render_edit(fb); else render_main(fb); }

static app_redraw_t edit_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_PRESS)
        return ui_keyboard_press(ev->x0, ev->y0, 560, ctx->now_ms) ? edit_paint(ctx, false) : APP_REDRAW_NONE;
    bool feedback = ev->type != UI_GESTURE_LONG_PRESS && ui_keyboard_release();
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R) {
        bool changed = ui_keyboard_page(ev->type == UI_GESTURE_SWIPE_L ? 1 : -1);
        return (changed || feedback) ? edit_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_TAP) return feedback ? edit_paint(ctx, false) : APP_REDRAW_NONE;
    int x = ev->x0, y = ev->y0;
    if (ui_rect_hit((EpdRect){36, 1000, 200, 72}, x, y)) { ui_keyboard_end(); s_page = XZ_MAIN; return APP_REDRAW_PAGE; }
    if (ui_rect_hit((EpdRect){448, 1000, 200, 72}, x, y)) {
        if (ui_keyboard_pending()) { snprintf(s_notice, sizeof(s_notice), "请先确认候选字"); return APP_REDRAW_PAGE; }
        snprintf(s_cfg[s_edit_field], XZ_FIELD_CAP, "%s", s_edit_buf);
        ui_keyboard_end(); s_page = XZ_MAIN;
        snprintf(s_notice, sizeof(s_notice), "字段已更新，记得保存配置"); return APP_REDRAW_PAGE;
    }
    if (ui_text_input_tap(&s_edit, (EpdRect){36, 243, 612, 82}, 28, s_edit_field == 2, NULL, x, y))
        return edit_paint(ctx, true);
    ui_keyboard_result_t r = ui_keyboard_tap(x, y, 560, esp_timer_get_time() / 1000);
    return r == UI_KEYBOARD_CHANGED ? edit_paint(ctx, false) : APP_REDRAW_NONE;
}

static app_redraw_t gesture_cb(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (s_page == XZ_EDIT) return edit_gesture(ctx, ev);
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    int x = ev->x0, y = ev->y0;
    if (y >= UI_NAV_TOP && x >= 228 && x < 456) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
    if (ui_rect_hit((EpdRect)XZ_BTN_ME, x, y)) {
        snprintf(s_cfg[0], XZ_FIELD_CAP, "%s", "小智me");
        snprintf(s_cfg[1], XZ_FIELD_CAP, "%s", "https://api.xiaozhi.me");
        s_cfg[3][0] = 0; snprintf(s_notice, sizeof(s_notice), "已选小智 me，请保存配置"); return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit((EpdRect)XZ_BTN_DS, x, y)) {
        snprintf(s_cfg[0], XZ_FIELD_CAP, "%s", "DeepSeek");
        snprintf(s_cfg[1], XZ_FIELD_CAP, "%s", "https://api.deepseek.com/v1");
        snprintf(s_cfg[3], XZ_FIELD_CAP, "%s", "deepseek-chat");
        snprintf(s_notice, sizeof(s_notice), "已选 DeepSeek，请填 Key 并保存"); return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit((EpdRect)XZ_BTN_SAVE, x, y)) {
        snprintf(s_notice, sizeof(s_notice), save_cfg() ? "配置已保存到 xiaozhi.cfg" : "保存失败，请检查 TF 卡");
        return APP_REDRAW_PAGE;
    }
    for (int i = 0; i < XZ_FIELDS; ++i)
        if (ui_rect_hit((EpdRect){36, XZ_ROW_Y + i * XZ_ROW_H, 612, XZ_ROW_H - 8}, x, y)) {
            open_editor(i); return APP_REDRAW_PAGE;
        }
    return APP_REDRAW_NONE;
}
static app_redraw_t key_cb(app_ctx_t *ctx, int value) {
    (void)value;
    if (s_page == XZ_EDIT) { ui_keyboard_end(); s_page = XZ_MAIN; return APP_REDRAW_PAGE; }
    ctx->request_return = true; return APP_REDRAW_NONE;
}

const app_desc_t app_xiaozhi = {
    .title = "小智 AI", .detail = "设备码与 API 配置", .enter_full = false, .owns_keys = false,
    .on_enter = enter, .on_exit = leave,
    .render = render, .on_gesture = gesture_cb, .on_key = key_cb,
};
