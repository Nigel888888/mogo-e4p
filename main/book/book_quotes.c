/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#include "book_quotes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"

#define QUOTE_LINE_BUF 1024u

/* 字段内制表符与换行折成空格，保证一行一条。/ Flatten tabs/newlines so each record stays on one line. */
static void flat(char *out, size_t cap, const char *text) {
    size_t w = 0;
    if (!text) { out[0] = 0; return; }
    for (const unsigned char *p = (const unsigned char *)text; *p && w + 1 < cap; ++p) {
        unsigned char c = *p;
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        out[w++] = (char)c;
    }
    out[w] = 0;
}

static bool line_has_text(const char *line, const char *text) {
    const char *tab = strchr(line, '\t');
    if (!tab) return false;
    size_t n = (size_t)(tab - line);
    return strlen(text) == n && !strncmp(line, text, n);
}

bool book_quotes_save(const char *text, const char *book, uint32_t chapter) {
    if (!text || !*text) return false;
    char a[BOOK_QUOTE_TEXT_MAX], b[BOOK_QUOTE_BOOK_MAX];
    flat(a, sizeof(a), text); flat(b, sizeof(b), book);
    if (!a[0]) return false;
    FILE *f = fopen(BOOK_QUOTES_PATH, "r");
    if (f) {
        char line[QUOTE_LINE_BUF];
        while (fgets(line, sizeof(line), f)) if (line_has_text(line, a)) { fclose(f); return false; }
        fclose(f);
    }
    f = fopen(BOOK_QUOTES_PATH, "a");
    if (!f) return false;
    fprintf(f, "%s\t%s\t%lu\t%ld\n", a, b, (unsigned long)chapter, (long)time(NULL));
    int error = ferror(f);
    if (fclose(f) != 0) error = 1;
    return !error;
}

bool book_quotes_load(book_quotes_list_t *out) {
    if (!out) return false;
    out->items = NULL; out->count = 0; out->capacity = 0;
    FILE *f = fopen(BOOK_QUOTES_PATH, "r");
    if (!f) return false;
    book_quote_item_t *items = (book_quote_item_t *)heap_caps_malloc(
        sizeof(book_quote_item_t) * BOOK_QUOTE_MAX_ITEMS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *line = (char *)heap_caps_malloc(QUOTE_LINE_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!items || !line) {
        if (items) heap_caps_free(items);
        if (line) heap_caps_free(line);
        fclose(f); return false;
    }
    size_t n = 0;
    while (fgets(line, QUOTE_LINE_BUF, f) && n < BOOK_QUOTE_MAX_ITEMS) {
        char *f1 = strchr(line, '\t'); if (!f1) continue;
        char *f2 = strchr(f1 + 1, '\t'); if (!f2) continue;
        char *f3 = strchr(f2 + 1, '\t'); if (!f3) continue;
        *f1 = *f2 = *f3 = 0;
        snprintf(items[n].text, sizeof(items[n].text), "%s", line);
        snprintf(items[n].book, sizeof(items[n].book), "%s", f1 + 1);
        items[n].chapter = (uint32_t)strtoul(f2 + 1, NULL, 10);
        items[n].ts = (int64_t)strtoll(f3 + 1, NULL, 10);
        ++n;
    }
    fclose(f); heap_caps_free(line);
    out->items = items; out->count = n; out->capacity = BOOK_QUOTE_MAX_ITEMS;
    return true;
}

void book_quotes_list_free(book_quotes_list_t *list) {
    if (!list || !list->items) return;
    heap_caps_free(list->items);
    list->items = NULL; list->count = 0; list->capacity = 0;
}

bool book_quotes_remove(const char *text) {
    if (!text || !*text) return false;
    book_quotes_list_t list;
    if (!book_quotes_load(&list)) return false;
    bool kept = false;
    for (size_t i = 0; i < list.count; ++i) if (!strcmp(list.items[i].text, text)) { kept = true; break; }
    if (!kept) { book_quotes_list_free(&list); return false; }
    FILE *f = fopen(BOOK_QUOTES_PATH, "w");
    if (!f) { book_quotes_list_free(&list); return false; }
    for (size_t i = 0; i < list.count; ++i) {
        if (!strcmp(list.items[i].text, text)) continue;
        fprintf(f, "%s\t%s\t%lu\t%ld\n", list.items[i].text, list.items[i].book,
                (unsigned long)list.items[i].chapter, (long)list.items[i].ts);
    }
    int error = ferror(f);
    if (fclose(f) != 0) error = 1;
    book_quotes_list_free(&list);
    return !error;
}

bool book_quotes_export_clippings(const char *path) {
    if (!path || !*path) return false;
    book_quotes_list_t list;
    if (!book_quotes_load(&list)) return false;
    FILE *f = fopen(path, "w");
    if (!f) { book_quotes_list_free(&list); return false; }
    for (size_t i = 0; i < list.count; ++i) {
        const book_quote_item_t *q = &list.items[i];
        fprintf(f, "%s\n", q->book[0] ? q->book : "未知书籍");
        fprintf(f, "- 第 %lu 章 | 位置 %ld\n", (unsigned long)q->chapter, (long)q->ts);
        fprintf(f, "%s\n==========\n", q->text);
    }
    int error = ferror(f);
    if (fclose(f) != 0) error = 1;
    book_quotes_list_free(&list);
    return !error;
}
