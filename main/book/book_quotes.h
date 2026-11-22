/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：好句集——阅读时收录整句，追加写 TF 卡，可导出 Kindle Clippings。
 * English: Quote notebook: append-only sentences from the reader, exportable as Kindle Clippings.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOK_QUOTES_PATH "/sdcard/quotes.tsv"
#define BOOK_QUOTE_TEXT_MAX 512
#define BOOK_QUOTE_BOOK_MAX 160
#define BOOK_QUOTE_MAX_ITEMS 256

typedef struct {
    char text[BOOK_QUOTE_TEXT_MAX];
    char book[BOOK_QUOTE_BOOK_MAX];
    uint32_t chapter;
    int64_t ts;
} book_quote_item_t;

typedef struct {
    book_quote_item_t *items;
    size_t count;
    size_t capacity;
} book_quotes_list_t;

/// 收录整句；同句去重，返回是否新写入。/ Append one sentence; duplicates are ignored.
bool book_quotes_save(const char *text, const char *book, uint32_t chapter);
/// 读取全部句子到列表，调用方负责 list_free。/ Load all quotes into a caller-owned list.
bool book_quotes_load(book_quotes_list_t *out);
void book_quotes_list_free(book_quotes_list_t *list);
/// 按原句删除。/ Remove one quote by exact text.
bool book_quotes_remove(const char *text);
/// 导出 Kindle My Clippings.txt 格式。/ Export in Kindle My Clippings.txt format.
bool book_quotes_export_clippings(const char *path);
