#!/bin/sh
# 中文：用真实接收器和搜索实现验证传书存储边界。
# English: Test transfer storage bounds with the real receiver and search implementation.
set -eu
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -Wno-deprecated-declarations -g \
  -fsanitize=address,undefined -Icomponents/book_search/include \
  tools/transfer_host_test.c components/book_search/book_search.c -o "$out/transfer-storage"
"$out/transfer-storage"
