/* 中文：两个字体引擎共用的失败可恢复光栅化。仅在各自的实现文件中包含。
 * English: Failure-safe rasterization for both font engines. Include only from their implementations.
 */
#pragma once

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef STB_GLYPH_ALLOC
#define STB_GLYPH_ALLOC malloc
#define STB_GLYPH_FREE free
#endif

typedef struct stb_glyph_block {
    struct stb_glyph_block *next, **prev_link;
} stb_glyph_block_t;

typedef struct {
    jmp_buf on_oom;
    stb_glyph_block_t *head;
} stb_glyph_scope_t;

static void *stb_glyph_malloc(size_t size, void *userdata) {
    if (!userdata) return STB_GLYPH_ALLOC(size);
    stb_glyph_scope_t *scope = userdata;
    stb_glyph_block_t *block = size > SIZE_MAX - sizeof(*block) ? NULL :
        STB_GLYPH_ALLOC(sizeof(*block) + size);
    // stb 部分分配点不判空，不能把 NULL 交回去；仅退出当前同步光栅化，统一释放其临时分配。
    // Some stb allocation sites cannot handle NULL; exit only this synchronous raster call and release its scratch.
    if (!block) longjmp(scope->on_oom, 1);
    block->next = scope->head;
    block->prev_link = &scope->head;
    if (scope->head) scope->head->prev_link = &block->next;
    scope->head = block;
    return block + 1;
}

static void stb_glyph_free(void *ptr, void *userdata) {
    if (!ptr) return;
    if (!userdata) {
        STB_GLYPH_FREE(ptr);
        return;
    }
    stb_glyph_block_t *block = (stb_glyph_block_t *)ptr - 1;
    *block->prev_link = block->next;
    if (block->next) block->next->prev_link = block->prev_link;
    STB_GLYPH_FREE(block);
}

#define STBTT_malloc(n, u) stb_glyph_malloc((n), (u))
#define STBTT_free(p, u) stb_glyph_free((p), (u))
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

// 追踪头只要求指针对齐，符合 PSRAM heap 提供的对齐。
// The tracking prefix only requires pointer alignment, as provided by the PSRAM heap.
_Static_assert(_Alignof(stb_glyph_block_t) >= _Alignof(stbtt__active_edge) &&
               _Alignof(stb_glyph_block_t) >= _Alignof(stbtt_vertex), "stb glyph scratch alignment");

typedef enum {
    STB_GLYPH_OK, STB_GLYPH_NO_MEMORY, STB_GLYPH_NO_OUTLINE,
} stb_glyph_result_t;

static stb_glyph_result_t stb_glyph_render(stb_glyph_scope_t *scope, const stbtt_fontinfo *font,
                                          int gid, float scale, int x0, int y0,
                                          int w, int h, uint8_t *pixels) {
    if (setjmp(scope->on_oom)) return STB_GLYPH_NO_MEMORY;
    stbtt_fontinfo raster_font = *font;
    raster_font.userdata = scope;
    stbtt_vertex *shape = NULL;
    int count = stbtt_GetGlyphShape(&raster_font, gid, &shape);
    if (count > 0) {
        // 与 stbtt_MakeGlyphBitmap 相同的采样参数；额外区分无轮廓和成功。
        // Match stbtt_MakeGlyphBitmap sampling while distinguishing missing outlines from success.
        stbtt__bitmap out = {.w = w, .h = h, .stride = w, .pixels = pixels};
        stbtt_Rasterize(&out, 0.35f, shape, count, scale, scale, 0.f, 0.f, x0, y0, 1, scope);
    }
    stbtt_FreeShape(&raster_font, shape);
    return count > 0 ? STB_GLYPH_OK : STB_GLYPH_NO_OUTLINE;
}

static stb_glyph_result_t stb_glyph_make_bitmap(const stbtt_fontinfo *font, int gid, float scale,
                                               int x0, int y0, int w, int h, uint8_t *pixels) {
    // 把分配链放在 setjmp 的调用方，跳回时它仍然有效。
    // Keep the allocation list in the caller of setjmp so it remains valid after the jump.
    stb_glyph_scope_t scope;
    scope.head = NULL;
    stb_glyph_result_t result = stb_glyph_render(&scope, font, gid, scale, x0, y0, w, h, pixels);
    while (scope.head) stb_glyph_free(scope.head + 1, &scope);
    return result;
}
