/*
 * SPDX-FileCopyrightText: 2026 chopper1026
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 *
 * 中文：生词本——列表浏览、详情复习、逐词删除。数据来自阅读查词「保存」生成的 JSONL。
 * English: Vocabulary notebook: list, review details, delete. Reads the JSONL written by reader lookup "save".
 */
#include "app.h"
#include "book_vocabulary.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>

extern const app_desc_t app_vocabulary;

#define V_ROWS 5u
#define V_ROW_H 160
#define V_LIST_TOP 230
#define V_DETAIL_LINE_H 60
#define V_DETAIL_TOP 330
#define V_DETAIL_PER_PAGE 8u
#define V_BTN_DEL  {36, 1000, 200, 72}
#define V_BTN_PREV {256, 1000, 172, 72}
#define V_BTN_NEXT {448, 1000, 200, 72}

typedef enum { V_VIEW_LIST = 0, V_VIEW_DETAIL } vocab_view_t;

static book_vocabulary_list_t s_list;
static bool s_loaded;
static vocab_view_t s_view;
static unsigned s_page;      /* 列表页号 */
static unsigned s_selected;  /* 详情显示的分页（释义按屏翻） */
static unsigned s_detail;    /* 当前详情条目索引 */
static char s_notice[80];

/* 每行矩形：列表条目命中与绘制共用。/ Row rect shared by drawing and hit-test. */
static EpdRect row_rect(unsigned i) {
    return (EpdRect){ 36, V_LIST_TOP + (int)i * V_ROW_H, 612, V_ROW_H - 8 };
}
static EpdRect page_btn(bool next) {
    return (EpdRect){ next ? 576 : 36, 1024, 72, 72 };
}
static unsigned page_count(void) {
    if (!s_loaded || !s_list.count) return 0;
    return (unsigned)((s_list.count + V_ROWS - 1) / V_ROWS);
}
static void notice(const char *text) { snprintf(s_notice, sizeof(s_notice), "%s", text); }

static void load_list(void) {
    if (s_loaded) return;
    s_loaded = true;
    if (!book_vocabulary_load(&s_list)) { s_list.items = NULL; s_list.count = 0; }
    s_page = 0; s_view = V_VIEW_LIST; s_notice[0] = 0;
}

static void unload_list(void) {
    if (s_loaded) book_vocabulary_list_free(&s_list);
    s_loaded = false;
    s_list.items = NULL; s_list.count = 0;
}

static void enter(app_ctx_t *ctx) {
    (void)ctx;
    load_list();
}
static void leave(app_ctx_t *ctx) { (void)ctx; /* 保留列表缓存，返回时无需重读 TF */ }

/* 单行文本按像素宽截断，按 UTF-8 边界收缩。/ Truncate one line to a pixel width on UTF-8 boundaries. */
static void fit_text(uint8_t *fb, int x, int y, int px, const char *value, int width) {
    char shown[BOOK_VOCAB_DEF_MAX + 4];
    snprintf(shown, sizeof(shown), "%s", value ? value : "");
    while (*shown && ui_text_fixed_width_px(px, shown) > width) {
        size_t n = strlen(shown);
        if (!n) break;
        --n;
        while (n && ((unsigned char)shown[n] & 0xC0) == 0x80) --n;
        shown[n] = 0;
    }
    ui_text_fixed(fb, x, y, px, shown, EPD_DRAW_ALIGN_LEFT, false);
}

static void render_list(uint8_t *fb) {
    ui_clear_page(fb);
    ui_nav_status(fb);
    char head[64];
    snprintf(head, sizeof(head), "生词本 · 共 %u 词", (unsigned)s_list.count);
    ui_text_fixed_vc(fb, 342, 116, 46, "生词本", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_fixed(fb, 36, 176, 26, head, EPD_DRAW_ALIGN_LEFT, false);

    if (!s_list.count) {
        ui_text_fixed_vc(fb, 342, 560, 34, "还没有生词", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_fixed_vc(fb, 342, 610, 26, "阅读时长按查词，点右上角保存", EPD_DRAW_ALIGN_CENTER, false);
    } else {
        unsigned pages = page_count();
        unsigned first = s_page * V_ROWS;
        for (unsigned i = 0; i < V_ROWS; ++i) {
            unsigned idx = first + i;
            if (idx >= s_list.count) break;
            EpdRect r = row_rect(i);
            ui_draw_choice_round_rect(fb, r, 12, false);
            const book_vocabulary_item_t *it = &s_list.items[idx];
            fit_text(fb, r.x + 20, r.y + 12, 40, it->word, 460);
            if (it->reading[0]) fit_text(fb, r.x + 20, r.y + 62, 28, it->reading, 560);
            fit_text(fb, r.x + 20, r.y + 100, 26, it->definition, 560);
        }
        char pager[48];
        snprintf(pager, sizeof(pager), "%u / %u", s_page + 1, pages);
        ui_text_fixed_vc(fb, 342, 1060, 28, pager, EPD_DRAW_ALIGN_CENTER, false);
        if (s_page) ui_draw_icon(fb, page_btn(false).x + 36, 1060, 40, UI_ICON_CHEVRON_LEFT, 0);
        if (s_page + 1 < pages) ui_draw_icon(fb, page_btn(true).x + 36, 1060, 40, UI_ICON_CHEVRON_RIGHT, 0);
    }
    if (s_notice[0]) ui_text_fixed_vc(fb, 342, 226, 26, s_notice, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_separator(fb, UI_NAV_TOP, 0, 684, 0x90);
    ui_draw_icon(fb, 342, 1164, 44, UI_ICON_HOUSE, 0);
}

/* 详情把释义+例句拼成一个数组，按屏分页。/ Detail flattens definition+example into paged lines. */
#define V_DETAIL_MAX_LINES 48
#define V_DETAIL_LINE_BUF 128  /* 42 汉字 ≈ 126 字节 UTF-8 */
/* 6 KB 静态数组放片外 RAM：片内 RAM 只剩几 KB，放这里会挤爆链接期的 iram0 预算。
 * 6 KB of static data goes to external RAM; internal RAM has no room left in the iram0 budget. */
static EXT_RAM_BSS_ATTR char s_lines[V_DETAIL_MAX_LINES][V_DETAIL_LINE_BUF];
static unsigned s_line_count;

static void build_detail(const book_vocabulary_item_t *it) {
    s_line_count = 0;
    const char *labels[] = { "释义", "例句", "来自", NULL };
    const char *vals[] = { it->definition, it->example, it->book, NULL };
    for (int f = 0; f < 3; ++f) {
        if (!vals[f] || !vals[f][0]) continue;
        if (s_line_count < V_DETAIL_MAX_LINES)
            snprintf(s_lines[s_line_count++], V_DETAIL_LINE_BUF, "【%s】", labels[f]);
        /* 按列宽粗分句，按 UTF-8 边界前进；每行约 42 个汉字宽。
         * Coarse wrap by column width with UTF-8-safe stepping. */
        const char *p = vals[f];
        while (*p && s_line_count < V_DETAIL_MAX_LINES) {
            const char *end = p;
            int cols = 0;
            while (*end && cols < 42) {
                unsigned char c = (unsigned char)*end;
                if ((c & 0xC0) != 0x80) ++cols;
                ++end;
            }
            size_t len = (size_t)(end - p);
            if (len >= V_DETAIL_LINE_BUF) len = V_DETAIL_LINE_BUF - 1;
            memcpy(s_lines[s_line_count++], p, len);
            s_lines[s_line_count - 1][len] = 0;
            p = end;
        }
    }
}

typedef struct { unsigned start, end; } line_span_t;
static line_span_t detail_span(void) {
    line_span_t sp;
    sp.start = s_selected * V_DETAIL_PER_PAGE;
    sp.end = sp.start + V_DETAIL_PER_PAGE;
    if (sp.end > s_line_count) sp.end = s_line_count;
    return sp;
}

static void render_detail(uint8_t *fb) {
    ui_clear_page(fb);
    ui_nav_status(fb);
    if (s_detail >= s_list.count) { s_view = V_VIEW_LIST; render_list(fb); return; }
    const book_vocabulary_item_t *it = &s_list.items[s_detail];
    fit_text(fb, 36, 196, 54, it->word, 560);
    if (it->reading[0]) fit_text(fb, 36, 268, 30, it->reading, 600);

    line_span_t sp = detail_span();
    int y = V_DETAIL_TOP;
    for (unsigned i = sp.start; i < sp.end; ++i) {
        fit_text(fb, 40, y, 30, s_lines[i], 600);
        y += V_DETAIL_LINE_H;
    }
    unsigned detail_pages = (s_line_count + V_DETAIL_PER_PAGE - 1) / V_DETAIL_PER_PAGE;
    char meta[96];
    /* S31 上 uint32_t 是 long unsigned int，%u 会触发 -Werror=format；统一转成 unsigned long。
     * On S31 uint32_t is long unsigned int, so %u trips -Werror=format; cast to unsigned long. */
    snprintf(meta, sizeof(meta), "第%lu章 · 第%u/%u页", (unsigned long)it->chapter, s_selected + 1,
             detail_pages ? detail_pages : 1);
    ui_text_fixed(fb, 40, 900, 26, meta, EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_button(fb, (EpdRect)V_BTN_DEL, "删除", false);
    ui_draw_button(fb, (EpdRect)V_BTN_PREV, "上一页", s_selected > 0);
    ui_draw_button(fb, (EpdRect)V_BTN_NEXT, "下一页", sp.end < s_line_count);

    if (s_notice[0]) ui_text_fixed_vc(fb, 342, 150, 26, s_notice, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_separator(fb, UI_NAV_TOP, 0, 684, 0x90);
    ui_draw_icon(fb, 342, 1164, 44, UI_ICON_HOUSE, 0);
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    if (s_view == V_VIEW_DETAIL) render_detail(fb);
    else render_list(fb);
}

static bool nav_hit(int x, int y) { return y >= UI_NAV_TOP && x >= 228 && x < 456; }

static void do_delete(void) {
    if (s_detail >= s_list.count) return;
    const char *word = s_list.items[s_detail].word;
    if (book_vocabulary_remove(word)) {
        /* 重新读取，保证索引一致。/ Reload so indices stay consistent. */
        unload_list();
        load_list();
        s_view = V_VIEW_LIST;
        notice("已删除");
    } else {
        notice("删除失败");
    }
}

static app_redraw_t gesture_list(int x, int y) {
    unsigned pages = page_count();
    if (ui_rect_hit(page_btn(false), x, y) && s_page) { --s_page; return APP_REDRAW_PAGE; }
    if (ui_rect_hit(page_btn(true), x, y) && s_page + 1 < pages) { ++s_page; return APP_REDRAW_PAGE; }
    for (unsigned i = 0; i < V_ROWS; ++i) {
        unsigned idx = s_page * V_ROWS + i;
        if (idx >= s_list.count) break;
        if (ui_rect_hit(row_rect(i), x, y)) {
            s_detail = idx; s_selected = 0;
            build_detail(&s_list.items[s_detail]);
            s_view = V_VIEW_DETAIL;
            return APP_REDRAW_PAGE;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t gesture_detail(int x, int y) {
    if (ui_rect_hit((EpdRect)V_BTN_DEL, x, y)) { do_delete(); return APP_REDRAW_PAGE; }
    line_span_t sp = detail_span();
    if (ui_rect_hit((EpdRect)V_BTN_PREV, x, y) && s_selected) {
        --s_selected; return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit((EpdRect)V_BTN_NEXT, x, y) && sp.end < s_line_count) {
        ++s_selected; return APP_REDRAW_PAGE;
    }
    s_view = V_VIEW_LIST; /* 点空白处返回列表 */
    return APP_REDRAW_PAGE;
}

static app_redraw_t gesture_cb(app_ctx_t *ctx, const ui_gesture_event_t *event) {
    if (s_view != V_VIEW_LIST &&
        (event->type == UI_GESTURE_SWIPE_L || event->type == UI_GESTURE_SWIPE_R))
        return APP_REDRAW_NONE;

    /* 列表左右滑动翻页。/ Swipe to change list page. */
    if (s_view == V_VIEW_LIST &&
        (event->type == UI_GESTURE_SWIPE_L || event->type == UI_GESTURE_SWIPE_R)) {
        unsigned pages = page_count();
        if (event->type == UI_GESTURE_SWIPE_L && s_page + 1 < pages) { ++s_page; return APP_REDRAW_PAGE; }
        if (event->type == UI_GESTURE_SWIPE_R && s_page) { --s_page; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    int x = event->x0, y = event->y0;
    if (nav_hit(x, y)) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
    if (s_view == V_VIEW_DETAIL) return gesture_detail(x, y);
    return gesture_list(x, y);
}

static app_redraw_t key_cb(app_ctx_t *ctx, int value) {
    (void)value;
    /* 非 owns_keys 页面只在菜单关闭时收到 KEY_1：详情退回列表，列表按返回。
     * Non-owns_keys pages get KEY_1 only; KEY_2/KEY_3 stay with the loop (refresh/menu). */
    if (s_view == V_VIEW_DETAIL) { s_view = V_VIEW_LIST; return APP_REDRAW_PAGE; }
    ctx->request_return = true;
    return APP_REDRAW_NONE;
}

const app_desc_t app_vocabulary = {
    .title = "生词本", .detail = "收藏的单词与释义", .enter_full = false, .owns_keys = false,
    .on_enter = enter, .on_exit = leave,
    .render = render, .on_gesture = gesture_cb, .on_key = key_cb,
};
