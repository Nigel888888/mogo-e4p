#!/usr/bin/env python3
"""验证内建与自选字体在各分配点失败后重绘恢复。/ Test glyph retries after allocation failures in both font engines."""
import platform
import subprocess
import tempfile
from pathlib import Path

from reader_title_style_preview import ROOT, FONT, write_shims


TEST = r'''
#include "ttf_font.h"
#include "ui_font.h"
#include "ui_hanzi.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union { max_align_t align; size_t size; } allocation_t;
static size_t live, fail_at, calls;
void *glyph_test_malloc(size_t n) {
    if (++calls == fail_at) return NULL;
    allocation_t *p = malloc(sizeof(*p) + n);
    assert(p);
    p->size = n;
    live += n;
    return p + 1;
}
void glyph_test_free(void *ptr) {
    if (!ptr) return;
    allocation_t *p = (allocation_t *)ptr - 1;
    live -= p->size;
    free(p);
}
void *heap_caps_malloc(size_t n, int caps) { (void)caps; return glyph_test_malloc(n); }
void *heap_caps_calloc(size_t n, size_t s, int caps) {
    void *p = heap_caps_malloc(n * s, caps);
    return p ? memset(p, 0, n * s) : NULL;
}
void heap_caps_free(void *p) { glyph_test_free(p); }
size_t heap_caps_get_allocated_size(void *p) { return p ? ((allocation_t *)p - 1)->size : 0; }
size_t heap_caps_get_free_size(int c) { (void)c; return 8 * 1024 * 1024 - live; }
size_t heap_caps_get_largest_free_block(int c) { return heap_caps_get_free_size(c); }
void *heap_caps_realloc(void *ptr, size_t n, int caps) {
    void *p = heap_caps_malloc(n, caps);
    if (!p) return NULL;
    if (ptr) {
        size_t old = heap_caps_get_allocated_size(ptr);
        memcpy(p, ptr, n < old ? n : old);
        heap_caps_free(ptr);
    }
    return p;
}

// 补充字形块由 ui_hanzi_host_test.py 单独覆盖。/ Real supplement decoding is covered by ui_hanzi_host_test.py.
bool ui_hanzi_has(uint32_t cp) { (void)cp; return false; }
bool ui_hanzi_get(uint32_t cp, uint8_t record[UI_HANZI_RECORD]) { (void)cp; (void)record; return false; }

enum { W = 384, H = 192 };
static uint8_t fb[W * H], empty[W * H], warm[W * H], complete[W * H];
void epd_draw_pixel(int x, int y, uint8_t color, uint8_t *pixels) {
    assert(x >= 0 && x < W && y >= 0 && y < H);
    pixels[y * W + x] = color;
}

static void paint(bool builtin, int px, const char *text) {
    memset(fb, 255, sizeof(fb));
    if (builtin) ui_font_draw_text_px(fb, 16, 152, px, text, EPD_DRAW_ALIGN_LEFT, 0, 15, false);
    else ttf_draw_text_px(fb, 16, 152, px, text, EPD_DRAW_ALIGN_LEFT, 0, 15);
}

static void clear(bool builtin) {
    if (builtin) (void)ui_font_cache_clear();
    else ttf_font_cache_clear();
}

static void recover_each_allocation(bool builtin, int px) {
    clear(builtin);
    paint(builtin, px, "日");
    assert(memcmp(fb, empty, sizeof(fb)) != 0);
    memcpy(warm, fb, sizeof(warm));
    calls = 0;
    paint(builtin, px, "日蓝");
    const size_t allocation_count = calls;
    assert(allocation_count >= (builtin ? 6u : 8u));
    assert(memcmp(fb, warm, sizeof(fb)) != 0);
    memcpy(complete, fb, sizeof(complete));

    for (size_t i = 1; i <= allocation_count; ++i) {
        clear(builtin);
        paint(builtin, px, "日");
        const size_t before = live;
        calls = 0;
        fail_at = i;
        paint(builtin, px, "日蓝");
        assert(calls >= i);
        assert(live == before);
        assert(memcmp(fb, warm, sizeof(fb)) == 0);
        fail_at = 0;
        paint(builtin, px, "日蓝");
        assert(memcmp(fb, complete, sizeof(fb)) == 0);
        calls = 0;
        paint(builtin, px, "日蓝");
        assert(calls == 0 && memcmp(fb, complete, sizeof(fb)) == 0);
    }
    paint(builtin, px, " ");
    assert(memcmp(fb, empty, sizeof(fb)) == 0);
    calls = 0;
    paint(builtin, px, " ");
    assert(calls == 0 && memcmp(fb, empty, sizeof(fb)) == 0);
    printf("PASS: %s %dpx, all %zu cold-glyph allocation points retry with exact pixels and no retained scratch\n",
           builtin ? "builtin UI" : "file-backed TTF", px, allocation_count);
    clear(builtin);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    memset(empty, 255, sizeof(empty));
    assert(ui_font_has_text("日蓝 "));
    assert(ui_font_text_width_px(32, "日蓝 ") > 0);
    assert(live == 0 && calls == 0 && ui_font_cache_clear() == 0);
    fail_at = 1;
    paint(true, 32, "日");
    assert(calls == 1 && live == 0 && memcmp(fb, empty, sizeof(fb)) == 0);
    fail_at = 0;
    recover_each_allocation(true, 32);
    assert(live == 0);
    assert(ttf_font_open(argv[1]) == ESP_OK && !ttf_font_is_builtin());
    assert(ttf_font_has_text("日蓝 "));
    recover_each_allocation(false, 160);
    ttf_font_unload();
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="ui-glyph-retry-") as path:
        work = Path(path)
        write_shims(work)
        with (work / "esp_heap_caps.h").open("a") as f:
            f.write("void *heap_caps_realloc(void *,size_t,int);\n"
                    "size_t heap_caps_get_allocated_size(void *);\n")
        (work / "test.c").write_text(TEST)
        main_dir = ROOT / "main"
        flags = ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                 "-Wno-unused-variable", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-ffunction-sections", "-fdata-sections"]
        flags += [f"-I{p}" for p in (work, main_dir, *(main_dir / d for d in
                                                      ("font", "boot", "diagnostics", "display", "settings")))]
        ui = work / "ui_font.o"
        subprocess.run(flags + ["-Dmalloc=glyph_test_malloc", "-Dfree=glyph_test_free", "-c",
                                str(main_dir / "font/ui_font.c"), "-o", str(ui)], check=True)
        real = [main_dir / "font" / f"{name}.c" for name in
                ("ttf_font", "ttf_font_source", "ttf_glyph_cache", "ttf_block_cache")]
        gc = "-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections"
        exe = work / "test"
        subprocess.run(flags + list(map(str, (work / "test.c", work / "assets.c", ui, *real))) +
                       [gc, "-lm", "-o", str(exe)], check=True)
        subprocess.run([str(exe), str(FONT)], check=True)


if __name__ == "__main__":
    main()
