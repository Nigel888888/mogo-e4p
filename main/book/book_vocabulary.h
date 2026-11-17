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
