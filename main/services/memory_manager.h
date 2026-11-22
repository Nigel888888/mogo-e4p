/*
 * 中文：统一内存分配、压力请求与堆快照；固定账本不申请堆，不拥有页面业务资源。
 * English: Unified allocations, pressure requests and heap snapshots; the fixed ledger allocates no heap and owns no page business resources.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MM_POOL_INTERNAL = 0,
    MM_POOL_PSRAM,
    MM_POOL_COUNT,
} mm_pool_t;

typedef enum {
    MM_OWNER_SYSTEM = 0,
    MM_OWNER_READER,
    MM_OWNER_UI_CACHE,
    MM_OWNER_NETWORK,
    MM_OWNER_FILE_JOB,
    MM_OWNER_UNTRACKED,
    MM_OWNER_COUNT,
} mm_owner_t;

typedef enum {
    MM_CLEAN_USER_SHORTCUT = 0,
    MM_CLEAN_MEMORY_PAGE,
    MM_CLEAN_PRESSURE,
} mm_clean_reason_t;

typedef enum {
    MM_CLEAN_IDLE = 0,
    MM_CLEAN_REQUESTED,
    MM_CLEAN_RUNNING,
    MM_CLEAN_DONE,
    MM_CLEAN_PARTIAL,
} mm_clean_state_t;

typedef enum {
    MM_FAIL_NONE = 0,
    MM_FAIL_CAPACITY,
    MM_FAIL_FRAGMENTED,
    MM_FAIL_TRACKING,
    MM_FAIL_ALLOCATOR,
    MM_FAIL_INVALID,
    MM_FAIL_BUSY,
} mm_failure_t;

#define MM_TRACKED_ALLOCS 64

typedef struct {
    size_t total;
    size_t free_bytes;
    size_t largest_free;
    size_t minimum_free;
    size_t used_bytes;
} mm_pool_snapshot_t;

typedef struct {
    size_t internal_bytes;
    size_t psram_bytes;
} mm_owner_snapshot_t;

typedef struct {
    uint32_t generation;
    int64_t updated_ms;
    mm_pool_snapshot_t pool[MM_POOL_COUNT];
    mm_owner_snapshot_t owner[MM_OWNER_COUNT];
    mm_clean_state_t clean_state;
    mm_clean_reason_t clean_reason;
    uint32_t clean_request_id;
    size_t clean_released_bytes;
    uint32_t allocation_failures;
    mm_failure_t last_failure;
    bool pressure;
    bool inventory_partial; ///< 所有权采样有繁忙或并发变化，未覆盖量留在未追踪。/ Busy or changing ownership remains untracked.
} mm_snapshot_t;

typedef struct {
    uint32_t request_id;
    mm_clean_reason_t reason;
    mm_clean_state_t state;
    int64_t completed_ms;
    size_t before_internal_free;
    size_t after_internal_free;
    size_t before_psram_free;
    size_t after_psram_free;
    size_t before_internal_largest;
    size_t after_internal_largest;
    size_t before_psram_largest;
    size_t after_psram_largest;
    size_t released_internal;
    size_t released_psram;
} mm_clean_report_t;

/// 清理回调只在 UI 主循环的安全边界运行；返回 false 表示仍有资源被占用或只完成部分清理。
/// The callback runs at a UI-loop safety boundary; false means resources remain busy or cleanup was partial.
typedef bool (*mm_clean_callback_t)(mm_clean_reason_t reason);

/// 启动时、首个业务分配前初始化一次。/ Initialize once at boot before the first business allocation.
void mm_init(void);
void mm_set_clean_callback(mm_clean_callback_t callback);
esp_err_t mm_request_clean(mm_clean_reason_t reason, uint32_t *request_id);
/// 仅由 UI 主循环在资源借用已交还的安全边界推进。/ Advance only on the UI loop after resource borrows have returned.
void mm_tick(int64_t now_ms);
bool mm_clean_active(void);
void mm_get_snapshot(mm_snapshot_t *out);
void mm_get_clean_report(mm_clean_report_t *out);
const char *mm_owner_name(mm_owner_t owner);

/// 严格使用指定区域；准入按请求大小和 reserve_bytes 预留，不回退到另一块堆。
/// Use the specified pool strictly, budgeting requested bytes plus reserve_bytes without fallback to another heap.
void *mm_alloc(mm_owner_t owner, mm_pool_t pool, size_t bytes, size_t alignment, size_t reserve_bytes);
/// 仅接受账本内的 mm_alloc 指针；调用方仍须保证唯一所有权，释放后不再使用。
/// Accept only ledger-owned mm_alloc pointers; callers must still maintain unique ownership and never use freed pointers.
bool mm_free(void *ptr);
/// 区分总空闲与连续块不足；失败只请求安全边界清理，不在调用任务中释放缓存。
/// Distinguish total and contiguous capacity failures; request safe-boundary cleanup, never evict in the caller.
mm_failure_t mm_check_capacity(mm_pool_t pool, size_t minimum_free, size_t largest_block);

#ifdef __cplusplus
}
#endif
