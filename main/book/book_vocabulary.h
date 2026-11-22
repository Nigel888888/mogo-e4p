/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：阅读查词一键收藏的生词本，JSONL 追加写 TF 卡，便于导出 Anki。
 * English: One-tap vocabulary collector for reader lookups; append-only JSONL on TF card, Anki friendly.
 *
 * 设计约束 / Constraints:
 * - 只依赖 stdio，不接触字体、画面、布局或阅读位置。/ stdio only; no font, frame, layout or position state.
 * - 每条记录一行 JSON，word 为首字段，便于逐行去重与统计。/ One JSON object per line, word first for cheap dedup.
 * - 字符串转义后写盘，不持有查询或章节指针。/ Escape strings while streaming; never retain caller pointers.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOK_VOCABULARY_PATH "/sdcard/vocabulary.jsonl"

/* 显示层字段上限；超过则截断，保证单条结构尺寸固定。
 * Display field caps; longer values are truncated so each item has a fixed size. */
#define BOOK_VOCAB_WORD_MAX       96
#define BOOK_VOCAB_READING_MAX    192
#define BOOK_VOCAB_DEF_MAX       1024
#define BOOK_VOCAB_EXAMPLE_MAX    768
#define BOOK_VOCAB_BOOK_MAX       160
/* 列表常驻 PSRAM：单条约 2.2 KB，256 条约 560 KB。/ The list lives in PSRAM: ~2.2 KB per entry, 256 entries ≈ 560 KB. */
#define BOOK_VOCAB_MAX_ITEMS      256

typedef enum {
    BOOK_VOCABULARY_SAVED = 0,     ///< 新写入 / newly appended
    BOOK_VOCABULARY_DUPLICATE,     ///< 已收藏（未写盘）/ already present, nothing written
    BOOK_VOCABULARY_ERROR,         ///< TF 卡或参数错误 / storage or argument error
} book_vocabulary_status_t;

typedef struct {
    const char *query;      ///< 词头（必填）/ headword, required
    const char *reading;    ///< 读音 / IPA reading (may be empty)
    const char *definition; ///< 释义正文 / definition body
    const char *example;    ///< 例句 / example sentence (may be empty)
    const char *book;       ///< 书名（可空）/ source book title, may be NULL
    uint32_t chapter;       ///< 章节序号 / source chapter index
} book_vocabulary_entry_t;

/// 收藏一条生词；重复词头返回 DUPLICATE 且不触碰文件。/ Append one entry; duplicates return DUPLICATE without touching the file.
book_vocabulary_status_t book_vocabulary_save(const book_vocabulary_entry_t *entry);
/// 只读查询词头是否已收藏；IO 失败按未收藏处理。/ Read-only membership test; IO failure reports false.
bool book_vocabulary_contains(const char *query);
/// 统计已收藏词条数；读取失败返回 0。/ Count saved entries; zero on read failure.
size_t book_vocabulary_count(void);
/// 导出 Anki 友好的 TSV：词头/读音/释义/例句/书名/章节。/ Export an Anki-friendly TSV for desktop and AnkiDroid.
bool book_vocabulary_export_tsv(const char *path);

/* ---- 浏览/复习用：读取、删除 ---- */
/* ---- Browsing/review: load, remove ---- */

typedef struct {
    char word[BOOK_VOCAB_WORD_MAX];
    char reading[BOOK_VOCAB_READING_MAX];
    char definition[BOOK_VOCAB_DEF_MAX];
    char example[BOOK_VOCAB_EXAMPLE_MAX];
    char book[BOOK_VOCAB_BOOK_MAX];
    uint32_t chapter;
    int64_t ts;
} book_vocabulary_item_t;

typedef struct {
    book_vocabulary_item_t *items;  ///< PSRAM 中分配的数组 / array allocated in PSRAM
    size_t count;                    ///< 有效条目数 / number of valid entries
    size_t capacity;                 ///< 数组容量 / allocated capacity
} book_vocabulary_list_t;

/// 读取全部生词到列表（文件顺序）；调用方负责用 list_free 释放。
/// Load all entries into a list (file order); caller must free with list_free.
bool book_vocabulary_load(book_vocabulary_list_t *out);
/// 释放 load 分配的数组。/ Free the array allocated by load.
void book_vocabulary_list_free(book_vocabulary_list_t *list);
/// 按词头删除一条（整行重建文件，保持其余顺序）。/ Delete one entry by headword (rewrites file, preserving order).
bool book_vocabulary_remove(const char *query);
