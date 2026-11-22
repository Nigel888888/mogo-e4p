#pragma once

#include <stddef.h>
#include <stdint.h>

#define BOOK_HYPHEN_EN_MAX 48u

/// 美式英文断词位图；位 n 表示在第 n 个 ASCII 字母后可断开。不接受缩写式全大写、数字和非 ASCII。
/// US English break mask; bit n permits a break after n ASCII letters. Reject all-caps, digits and non-ASCII.
uint64_t book_hyphen_en_breaks(const char *word, size_t len);
