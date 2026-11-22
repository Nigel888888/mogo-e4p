#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 业务错误不覆盖 ESP-IDF 的驱动错误。/ Business errors do not overwrite ESP-IDF driver errors.
#define MEMORY_PEAK_BUSY       ((esp_err_t)0x7a01)
#define MEMORY_PEAK_CAPACITY   ((esp_err_t)0x7a02)
#define MEMORY_PEAK_FRAGMENT  ((esp_err_t)0x7a03)
#define MEMORY_PEAK_CONFLICT  ((esp_err_t)0x7a04)

typedef enum {
    MEMORY_PEAK_READER,
    MEMORY_PEAK_WIFI,
    MEMORY_PEAK_WIFI_SCAN,
    MEMORY_PEAK_WIFI_TIME,
    MEMORY_PEAK_WIFI_CONNECT,
    MEMORY_PEAK_CELLULAR,
    MEMORY_PEAK_BLE,
    MEMORY_PEAK_BLE_CONNECT,
    MEMORY_PEAK_WEATHER,
    MEMORY_PEAK_OTA_CHECK,
    MEMORY_PEAK_CLOUDZAO,
} memory_peak_kind_t;

// 忙错误携带拒绝时的所有者，勿在 UI 渲染时查询当前阶段来推断。
// A busy error captures the owner at rejection; do not infer it from the current phase in the UI.
#define MEMORY_PEAK_BUSY_OWNER_BASE ((esp_err_t)0x7a10)
static inline bool memory_peak_is_busy(esp_err_t error) {
    return error == MEMORY_PEAK_BUSY || (error >= MEMORY_PEAK_BUSY_OWNER_BASE &&
        error <= MEMORY_PEAK_BUSY_OWNER_BASE + MEMORY_PEAK_CLOUDZAO);
}

typedef uint32_t memory_peak_token_t;
typedef struct {
    size_t internal_free, internal_block;
    size_t psram_free, psram_block;
} memory_peak_heap_t;

/// 只保护高峰，不预分配或长期持有堆。/ Protect the peak without allocating or retaining heap.
esp_err_t memory_peak_try_begin(memory_peak_kind_t kind, memory_peak_token_t *token);
/// 只检查已知需求，调用方可先安全回收，再复测一次。/ Check known needs; caller may safely evict and recheck once.
esp_err_t memory_peak_check(memory_peak_heap_t need);
/// 复用已有快照判定总量与连续块，不再次扫描堆。/ Classify capacity and contiguity using an existing snapshot.
esp_err_t memory_peak_check_at(memory_peak_heap_t now, memory_peak_heap_t need);
memory_peak_heap_t memory_peak_snapshot(void);
/// 调用方必须交还一次；代次不符的旧交还无效。/ Finish once; stale generations cannot release a newer operation.
void memory_peak_finish(memory_peak_token_t token);
bool memory_peak_is_memory_error(esp_err_t error);
const char *memory_peak_hint(esp_err_t error);

#ifdef __cplusplus
}
#endif
