/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：流式转义的 JSONL 生词本，追加写、逐行去重、无整条拼接分配。
 * English: Streaming-escaped JSONL vocabulary; append-only, line dedup, no whole-entry allocation.
 */
#include "book_vocabulary.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define VOCAB_LINE_MAX 1024u

/* 写出 JSON 字符串：逐字符流式，不构造临时缓存。
 * Write a JSON string byte by byte; no temporary buffer for the escaped value. */
static void json_string(FILE *f, const char *text) {
    static const char hex[] = "0123456789abcdef";
    fputc('"', f);
    if (text) for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        unsigned char c = *p;
        switch (c) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\b': fputs("\\b", f); break;
            case '\f': fputs("\\f", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default:
                if (c < 0x20) {
                    fputs("\\u00", f); fputc(hex[c >> 4], f); fputc(hex[c & 15], f);
                } else fputc(c, f);
        }
    }
    fputc('"', f);
}

/* 词头转义必须与 json_string 逐字节一致，才能命中整行前缀。
 * Escape a headword exactly as json_string does, so the line prefix matches byte for byte. */
static size_t escape_token(const char *query, char *out, size_t cap) {
    static const char hex[] = "0123456789abcdef";
    if (!query || !*query || cap < 2) return 0;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)query; *p && n + 7 < cap; ++p) {
        unsigned char c = *p;
        switch (c) {
            case '"':  out[n++] = '\\'; out[n++] = '"'; break;
            case '\\': out[n++] = '\\'; out[n++] = '\\'; break;
            case '\b': out[n++] = '\\'; out[n++] = 'b'; break;
            case '\f': out[n++] = '\\'; out[n++] = 'f'; break;
            case '\n': out[n++] = '\\'; out[n++] = 'n'; break;
            case '\r': out[n++] = '\\'; out[n++] = 'r'; break;
            case '\t': out[n++] = '\\'; out[n++] = 't'; break;
            default:
                if (c < 0x20) {
                    out[n++] = '\\'; out[n++] = 'u'; out[n++] = '0'; out[n++] = '0';
                    out[n++] = hex[c >> 4]; out[n++] = hex[c & 15];
                } else out[n++] = (char)c;
        }
    }
    out[n] = 0;
    return n;
}

bool book_vocabulary_contains(const char *query) {
    char token[128 * 6 + 1];
    if (!escape_token(query, token, sizeof(token))) return false;
    char prefix[24 + sizeof(token)];
    int written = snprintf(prefix, sizeof(prefix), "{\"word\":\"%s\"", token);
    if (written <= 0 || (size_t)written >= sizeof(prefix)) return false;
    size_t plen = (size_t)written;

    FILE *f = fopen(BOOK_VOCABULARY_PATH, "r");
    if (!f) return false;
    char line[VOCAB_LINE_MAX];
    bool found = false;
    /* 只比对每行开头，避免正文里出现相同字串造成误判。/ Compare only the line prefix to avoid body false positives. */
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, prefix, plen)) { found = true; break; }
    }
    fclose(f);
    return found;
}

size_t book_vocabulary_count(void) {
    FILE *f = fopen(BOOK_VOCABULARY_PATH, "r");
    if (!f) return 0;
    char line[VOCAB_LINE_MAX];
    size_t count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "{\"word\":", 8)) ++count;
    }
    fclose(f);
    return count;
}

book_vocabulary_status_t book_vocabulary_save(const book_vocabulary_entry_t *entry) {
    if (!entry || !entry->query || !*entry->query) return BOOK_VOCABULARY_ERROR;
    if (book_vocabulary_contains(entry->query)) return BOOK_VOCABULARY_DUPLICATE;

    FILE *f = fopen(BOOK_VOCABULARY_PATH, "a");
    if (!f) return BOOK_VOCABULARY_ERROR;

    fputs("{\"word\":", f);          json_string(f, entry->query);
    fputs(",\"reading\":", f);       json_string(f, entry->reading);
    fputs(",\"definition\":", f);    json_string(f, entry->definition);
    fputs(",\"example\":", f);       json_string(f, entry->example);
    fputs(",\"book\":", f);          json_string(f, entry->book);
    fprintf(f, ",\"chapter\":%lu", (unsigned long)entry->chapter);
    fprintf(f, ",\"ts\":%ld}\n", (long)time(NULL));

    int error = ferror(f);
    if (fclose(f) != 0) error = 1;
    return error ? BOOK_VOCABULARY_ERROR : BOOK_VOCABULARY_SAVED;
}
