#pragma once
#include "../../components/memory_peak/include/memory_peak.h"

// 非调度专项默认允许执行，专项使用真实调度器或显式注入冲突。
// Unrelated suites permit admission; dedicated suites use the real gate or inject conflicts.
static inline esp_err_t memory_peak_test_begin(memory_peak_kind_t kind, memory_peak_token_t *token) {
    (void)kind; *token = 1; return ESP_OK;
}
static inline memory_peak_heap_t memory_peak_test_snapshot(void) {
    const memory_peak_heap_t heap = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
    return heap;
}
static inline esp_err_t memory_peak_test_check_at(memory_peak_heap_t now, memory_peak_heap_t need) {
    if (now.internal_free < need.internal_free || now.psram_free < need.psram_free)
        return MEMORY_PEAK_CAPACITY;
    if (now.internal_block < need.internal_block || now.psram_block < need.psram_block)
        return MEMORY_PEAK_FRAGMENT;
    return ESP_OK;
}
static inline const char *memory_peak_test_hint(esp_err_t error) {
    return memory_peak_is_busy(error) || error == MEMORY_PEAK_CONFLICT ? "busy" :
        error == MEMORY_PEAK_CAPACITY || error == MEMORY_PEAK_FRAGMENT ? "memory" : NULL;
}
#define memory_peak_try_begin(kind, token) memory_peak_test_begin((kind), (token))
#define memory_peak_check(...) ((void)(__VA_ARGS__), ESP_OK)
#define memory_peak_snapshot() memory_peak_test_snapshot()
#define memory_peak_finish(token) ((void)(token))
#define memory_peak_is_memory_error(error) ((error) == 0x101 || (error) == MEMORY_PEAK_CAPACITY || (error) == MEMORY_PEAK_FRAGMENT)
#define memory_peak_hint(error) memory_peak_test_hint(error)
#define memory_peak_check_at(now, need) memory_peak_test_check_at((now), (need))
