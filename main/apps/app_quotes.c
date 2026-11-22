/*
 * 中文：好句集——浏览、删除并导出阅读时收录的整句。
 * English: Quote notebook: browse, delete and export sentences collected while reading.
 */
#include "app.h"
#include "book_quotes.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_nav.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>

extern const app_desc_t app_quotes;

#define Q_ROWS 5u
#define Q_ROW_H 160
#define Q_TOP 230
#define Q_BTN_EXPORT {500, 168, 164, 52}
#define Q_BTN_DEL {36, 1000, 200, 72}
#define Q_CLIPPINGS "/sdcard/My Clippings.txt"

typedef enum { Q_LIST, Q_DETAIL } q_view_t;
static book_quotes_list_t s_list;
static bool s_loaded;
static q_view_t s_view;
static unsigned s_page, s_detail;
static char s_notice[80];

static EpdRect row_rect(unsigned i) { return (EpdRect){36, Q_TOP + (int)i * Q_ROW_H, 612, Q_ROW_H - 8}; }
static EpdRect page_btn(bool next) { return (EpdRect){next ? 576 : 36, 1024, 72, 72}; }
static unsigned page_count(void) {
    return !s_loaded || !s_list.count ? 0 : (unsigned)((s_list.count + Q_ROWS - 1) / Q_ROWS);
}
static void notice(const char *t) { snprintf(s_notice, sizeof(s_notice), "%s", t); }

static void load_list(void) {
    if (s_loaded) return;
    s_loaded = true;
    if (!book_quotes_load(&s_list)) { s_list.items = NULL; s_list.count = 0; }
    s_page = 0; s_view = Q_LIST; s_notice[0] = 0;
}
static void unload_list(void) {
    if (s_loaded) book_quotes_list_free(&s_list);
    s_loaded = false; s_list.items = NULL; s_list.count = 0;
}
static void enter(app_ctx_t *ctx) { (void)ctx; load_list(); }
static void leave(app_ctx_t *ctx) { (void)ctx; }

static void fit_text(uint8_t *fb, int x, int y, int px, const char *v, int width) {
    char shown[BOOK_QUOTE_TEXT_MAX + 4];
    snprintf(shown, sizeof(shown), "%s", v ? v : "");
    while (*shown && ui_text_fixed_width_px(px, shown) > width) {
        size_t n = strlen(shown);
        if (!n) break;
        --n; while (n && ((unsigned char)shown[n] & 0xC0) == 0x80) --n;
        shown[n] = 0;
    }
    ui_text_fixed(fb, x, y, px, shown, EPD_DRAW_ALIGN_LEFT, false);
}

static void render_list(uint8_t *fb) {
    ui_clear_page(fb); ui_nav_status(fb);
    char head[64]; snprintf(head, sizeof(head), "好句集 · 共 %u 句", (unsigned)s_list.count);
    ui_text_fixed_vc(fb, 342, 116, 46, "好句集", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_fixed(fb, 36, 176, 26, head, EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_button(fb, (EpdRect)Q_BTN_EXPORT, "导出 Clippings", false);
    if (!s_list.count) {
        ui_text_fixed_vc(fb, 342, 560, 34, "还没有好句", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_fixed_vc(fb, 342, 610, 26, "长按压词句，点「存句」收录", EPD_DRAW_ALIGN_CENTER, false);
    } else {
        unsigned pages = page_count(), first = s_page * Q_ROWS;
        for (unsigned i = 0; i < Q_ROWS; ++i) {
            unsigned idx = first + i; if (idx >= s_list.count) break;
            EpdRect r = row_rect(i);
            ui_draw_choice_round_rect(fb, r, 12, false);
            const book_quote_item_t *q = &s_list.items[idx];
            fit_text(fb, r.x + 18, r.y + 12, 30, q->text, 570);
            char src[BOOK_QUOTE_BOOK_MAX + 32];
            snprintf(src, sizeof(src), "%s · 第%lu章", q->book[0] ? q->book : "未知书籍", (unsigned long)q->chapter);
            fit_text(fb, r.x + 18, r.y + 112, 24, src, 570);
        }
        char pager[32]; snprintf(pager, sizeof(pager), "%u / %u", s_page + 1, pages);
        ui_text_fixed_vc(fb, 342, 1060, 28, pager, EPD_DRAW_ALIGN_CENTER, false);
        if (s_page) ui_draw_icon(fb, page_btn(false).x + 36, 1060, 40, UI_ICON_CHEVRON_LEFT, 0);
        if (s_page + 1 < pages) ui_draw_icon(fb, page_btn(true).x + 36, 1060, 40, UI_ICON_CHEVRON_RIGHT, 0);
    }
    if (s_notice[0]) ui_text_fixed_vc(fb, 342, 226, 26, s_notice, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_separator(fb, UI_NAV_TOP, 0, 684, 0x90);
    ui_draw_icon(fb, 342, 1164, 44, UI_ICON_HOUSE, 0);
}

static EXT_RAM_BSS_ATTR char s_lines[24][192];
static unsigned s_line_count;
static void render_detail(uint8_t *fb) {
    ui_clear_page(fb); ui_nav_status(fb);
    if (s_detail >= s_list.count) { s_view = Q_LIST; render_list(fb); return; }
    const book_quote_item_t *q = &s_list.items[s_detail];
    char src[BOOK_QUOTE_BOOK_MAX + 40];
    snprintf(src, sizeof(src), "%s · 第%lu章", q->book[0] ? q->book : "未知书籍", (unsigned long)q->chapter);
    fit_text(fb, 36, 180, 26, src, 600);
    s_line_count = 0;
    const char *p = q->text;
    while (*p && s_line_count < 24) {
        const char *end = p; int cols = 0;
        while (*end && cols < 38) { unsigned char c = (unsigned char)*end; if ((c & 0xC0) != 0x80) ++cols; ++end; }
        size_t n = (size_t)(end - p); if (n >= sizeof(s_lines[0])) n = sizeof(s_lines[0]) - 1;
        memcpy(s_lines[s_line_count], p, n); s_lines[s_line_count][n] = 0; ++s_line_count; p = end;
    }
    for (unsigned i = 0; i < s_line_count; ++i) ui_text_fixed(fb, 40, 240 + (int)i * 38, 30, s_lines[i], EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_button(fb, (EpdRect)Q_BTN_DEL, "删除", false);
    if (s_notice[0]) ui_text_fixed_vc(fb, 342, 150, 26, s_notice, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_separator(fb, UI_NAV_TOP, 0, 684, 0x90);
    ui_draw_icon(fb, 342, 1164, 44, UI_ICON_HOUSE, 0);
}

static void render(app_ctx_t *ctx, uint8_t *fb) { (void)ctx; if (s_view == Q_DETAIL) render_detail(fb); else render_list(fb); }
static bool nav_hit(int x, int y) { return y >= UI_NAV_TOP && x >= 228 && x < 456; }

static void do_export(void) {
    notice(book_quotes_export_clippings(Q_CLIPPINGS) ? "已导出 My Clippings.txt" : "导出失败，请检查 TF 卡");
}
static void do_delete(void) {
    if (s_detail >= s_list.count) return;
    if (book_quotes_remove(s_list.items[s_detail].text)) {
        unload_list(); load_list(); s_view = Q_LIST; notice("已删除");
    } else notice("删除失败");
}
static app_redraw_t gesture_list(int x, int y) {
    if (ui_rect_hit((EpdRect)Q_BTN_EXPORT, x, y)) { do_export(); return APP_REDRAW_PAGE; }
    unsigned pages = page_count();
    if (ui_rect_hit(page_btn(false), x, y) && s_page) { --s_page; return APP_REDRAW_PAGE; }
    if (ui_rect_hit(page_btn(true), x, y) && s_page + 1 < pages) { ++s_page; return APP_REDRAW_PAGE; }
    for (unsigned i = 0; i < Q_ROWS; ++i) {
        unsigned idx = s_page * Q_ROWS + i; if (idx >= s_list.count) break;
        if (ui_rect_hit(row_rect(i), x, y)) { s_detail = idx; s_view = Q_DETAIL; return APP_REDRAW_PAGE; }
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t gesture_cb(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    int x = ev->x0, y = ev->y0;
    if (nav_hit(x, y)) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
    if (s_view == Q_DETAIL) {
        if (ui_rect_hit((EpdRect)Q_BTN_DEL, x, y)) { do_delete(); return APP_REDRAW_PAGE; }
        s_view = Q_LIST; return APP_REDRAW_PAGE;
    }
    return gesture_list(x, y);
}
static app_redraw_t key_cb(app_ctx_t *ctx, int value) {
    (void)value;
    if (s_view == Q_DETAIL) { s_view = Q_LIST; return APP_REDRAW_PAGE; }
    ctx->request_return = true; return APP_REDRAW_NONE;
}

const app_desc_t app_quotes = {
    .title = "好句集", .detail = "收藏的句子与出处", .enter_full = false, .owns_keys = false,
    .on_enter = enter, .on_exit = leave,
    .render = render, .on_gesture = gesture_cb, .on_key = key_cb,
};
