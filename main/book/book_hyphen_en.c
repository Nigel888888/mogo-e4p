/*
 * 中文：在只读离线表中匹配英文断词规则；不保存单词，也不参与绘制。
 * English: Match English hyphenation in an offline read-only table; owns no text or drawing.
 */
#include "book_hyphen_en.h"
#include <stdbool.h>
#include <string.h>
#include "book_hyphen_en_data.inc"

uint64_t book_hyphen_en_breaks(const char *word, size_t len) {
    if (len < 5 || len > BOOK_HYPHEN_EN_MAX) return 0;
    char padded[BOOK_HYPHEN_EN_MAX + 2] = {'.'};
    bool has_lowercase = false;
    for (size_t i = 0; i < len; ++i) {
        char c = word[i];
        if (c >= 'a' && c <= 'z') has_lowercase = true;
        else if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        else return 0;
        padded[i + 1] = c;
    }
    if (!has_lowercase) return 0;
    padded[len + 1] = '.';
    for (size_t i = 0; i < sizeof(s_exceptions) / sizeof(s_exceptions[0]); ++i) {
        const char *exception = s_exceptions[i].word;
        if (strlen(exception) == len && memcmp(exception, padded + 1, len) == 0)
            return s_exceptions[i].breaks;
    }
    // 各子串命中时逐位取最大权重；奇数允许断词，偶数阻止。
    // Overlay the maximum weight from every matching substring; odd allows a break, even inhibits it.
    uint8_t quality[BOOK_HYPHEN_EN_MAX + 3] = {0};
    for (size_t start = 0; start < len + 2; ++start) {
        uint16_t node = 0;
        for (size_t i = start; i < len + 2; ++i) {
            uint16_t end = s_child[node + 1];
            uint16_t edge = s_child[node];
            while (edge < end && s_label[edge] < padded[i]) ++edge;
            if (edge == end || s_label[edge] != padded[i]) break;
            node = edge;
            const uint8_t *weights = s_weight + s_value[node];
            for (size_t j = 0; j < *weights; ++j) {
                if (weights[j + 1] > quality[start + j]) quality[start + j] = weights[j + 1];
            }
        }
    }
    uint64_t breaks = 0;
    for (size_t i = 2; i + 3 <= len; ++i) {
        if (quality[i + 1] & 1) breaks |= UINT64_C(1) << i;
    }
    return breaks;
}
