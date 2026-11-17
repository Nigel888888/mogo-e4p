/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：流式转义的 JSONL 生词本，追加写、逐行去重、无整条拼接分配。
 * English: Streaming-escaped JSONL vocabulary; append-only, line dedup, no whole-entry allocation.
 */
#include "book_vocabulary.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"

#define VOCAB_LINE_MAX 1024u
/* 浏览用行缓冲：单行含完整释义，可能远超去重用的 1024，按字段上限取整。
 * Browse line buffer: a full line carries definitions and can exceed the 1024 dedup buffer. */
#define VOCAB_LINE_BUF 16384u
#define BOOK_VOCABULARY_TMP "/sdcard/vocabulary.jsonl.tmp"

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

/* ==================== 浏览/复习：读取与删除 ==================== */

/* 从一行 JSON 里提取字符串字段的值到 out（做反转义），返回是否找到。
 * Extract a JSON string field from one line into out (unescaped); returns found. */
static bool vocab_get_string(const char *line, const char *key, char *out, size_t cap) {
    if (!line || !key || !out || !cap) return false;
    char pat[32];
    int pn = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (pn <= 0 || (size_t)pn >= sizeof(pat)) return false;
    const char *p = strstr(line, pat);
    if (!p) { out[0] = 0; return false; }
    p += pn;
    while (*p == ' ') ++p;
    if (*p != '"') { out[0] = 0; return false; }
    ++p;
    size_t w = 0;
    while (*p && *p != '"' && w + 1 < cap) {
        char c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            switch (c) {
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case 'u': {  /* \u00XX -> 单字节 */
                    unsigned int v = 0;
                    if (p[0] == '0' && p[1] == '0' && p[2] && p[3]) {
                        sscanf(p + 2, "%2x", &v);
                        c = (char)(unsigned char)(v & 0xFF);
                        p += 4;
                    }
                    break;
                }
                default: break;
            }
        }
        out[w++] = c;
    }
    out[w] = 0;
    return true;
}

/* 提取整数字段（chapter / ts）。
 * Extract an integer field (chapter / ts). */
static long long vocab_get_int(const char *line, const char *key) {
    if (!line || !key) return 0;
    char pat[32];
    int pn = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (pn <= 0 || (size_t)pn >= sizeof(pat)) return 0;
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += pn;
    while (*p == ' ') ++p;
    return strtoll(p, NULL, 10);
}

bool book_vocabulary_load(book_vocabulary_list_t *out) {
    if (!out) return false;
    out->items = NULL; out->count = 0; out->capacity = 0;

    FILE *f = fopen(BOOK_VOCABULARY_PATH, "r");
    if (!f) return false;

    /* 列表与行缓冲都放 PSRAM，避免与本就紧张的片内 RAM 争用。
     * Both the list and the line buffer live in PSRAM, sparing tight internal RAM. */
    book_vocabulary_item_t *items = (book_vocabulary_item_t *)heap_caps_malloc(
        sizeof(book_vocabulary_item_t) * BOOK_VOCAB_MAX_ITEMS,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *line = (char *)heap_caps_malloc(VOCAB_LINE_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!items || !line) {
        if (items) heap_caps_free(items);
        if (line) heap_caps_free(line);
        fclose(f);
        return false;
    }

    size_t n = 0;
    while (n < BOOK_VOCAB_MAX_ITEMS && fgets(line, VOCAB_LINE_BUF, f)) {
        if (line[0] != '{') continue;
        book_vocabulary_item_t *it = &items[n];
        memset(it, 0, sizeof(*it));
        if (!vocab_get_string(line, "word", it->word, sizeof(it->word))) continue;
        if (!it->word[0]) continue;
        vocab_get_string(line, "reading", it->reading, sizeof(it->reading));
        vocab_get_string(line, "definition", it->definition, sizeof(it->definition));
        vocab_get_string(line, "example", it->example, sizeof(it->example));
        vocab_get_string(line, "book", it->book, sizeof(it->book));
        it->chapter = (uint32_t)vocab_get_int(line, "chapter");
        it->ts = vocab_get_int(line, "ts");
        ++n;
    }
    fclose(f);
    heap_caps_free(line);

    out->items = items;
    out->count = n;
    out->capacity = BOOK_VOCAB_MAX_ITEMS;
    return true;
}

void book_vocabulary_list_free(book_vocabulary_list_t *list) {
    if (!list) return;
    if (list->items) heap_caps_free(list->items);
    list->items = NULL;
    list->count = list->capacity = 0;
}

bool book_vocabulary_remove(const char *query) {
    char token[128 * 6 + 1];
    if (!escape_token(query, token, sizeof(token))) return false;
    char prefix[24 + sizeof(token)];
    int written = snprintf(prefix, sizeof(prefix), "{\"word\":\"%s\"", token);
    if (written <= 0 || (size_t)written >= sizeof(prefix)) return false;
    size_t plen = (size_t)written;

    FILE *in = fopen(BOOK_VOCABULARY_PATH, "r");
    if (!in) return false;
    FILE *out = fopen(BOOK_VOCABULARY_TMP, "w");
    if (!out) { fclose(in); return false; }

    char *line = (char *)heap_caps_malloc(VOCAB_LINE_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line) { fclose(in); fclose(out); return false; }

    bool removed = false;
    while (fgets(line, VOCAB_LINE_BUF, in)) {
        if (!removed && !strncmp(line, prefix, plen)) { removed = true; continue; }
        fputs(line, out);
    }
    heap_caps_free(line);
    fclose(in);
    int error = ferror(out);
    if (fclose(out) != 0) error = 1;

    if (error || !removed) { remove(BOOK_VOCABULARY_TMP); return false; }
    if (rename(BOOK_VOCABULARY_TMP, BOOK_VOCABULARY_PATH) != 0) {
        remove(BOOK_VOCABULARY_TMP);
        return false;
    }
    return true;
}
