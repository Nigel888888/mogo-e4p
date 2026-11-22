/*
 * 中文：固定分配账本、区域准入、限频压力清理与实时堆快照。
 * English: A fixed allocation ledger, pool admission, throttled pressure cleanup and live heap snapshots.
 */
#include "memory_manager.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static mm_clean_callback_t s_callback;
static mm_clean_state_t s_state = MM_CLEAN_IDLE;
static mm_clean_reason_t s_reason = MM_CLEAN_USER_SHORTCUT;
static uint32_t s_request_id;
static uint32_t s_generation;
static mm_clean_report_t s_report;
typedef struct {
    void *ptr;
    size_t bytes;
    mm_pool_t pool;
    mm_owner_t owner;
} allocation_t;
static allocation_t s_allocations[MM_TRACKED_ALLOCS];
static size_t s_pending[MM_POOL_COUNT];
static uint32_t s_allocation_epoch;
static uint32_t s_failures;
static mm_failure_t s_last_failure;
static bool s_pressure_pending;
static bool s_pressure;
static int64_t s_next_sample_ms;
static int64_t s_next_pressure_ms;

static uint32_t pool_caps(mm_pool_t pool) {
    return (pool == MM_POOL_INTERNAL ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT;
}

static void failure(mm_failure_t reason) {
    portENTER_CRITICAL(&s_lock);
    ++s_failures;
    s_last_failure = reason;
    if (reason != MM_FAIL_INVALID && reason != MM_FAIL_BUSY) s_pressure_pending = true;
    portEXIT_CRITICAL(&s_lock);
}

mm_failure_t mm_check_capacity(mm_pool_t pool, size_t minimum_free, size_t largest_block) {
    if (pool < 0 || pool >= MM_POOL_COUNT) {
        failure(MM_FAIL_INVALID);
        return MM_FAIL_INVALID;
    }
    const uint32_t caps = pool_caps(pool);
    const size_t available = heap_caps_get_free_size(caps);
    const size_t largest = heap_caps_get_largest_free_block(caps);
    portENTER_CRITICAL(&s_lock);
    const size_t pending = s_pending[pool];
    portEXIT_CRITICAL(&s_lock);
    mm_failure_t result = MM_FAIL_NONE;
    if (available < minimum_free || available - minimum_free < pending) result = MM_FAIL_CAPACITY;
    else if (largest < largest_block) result = MM_FAIL_FRAGMENTED;
    if (result != MM_FAIL_NONE) failure(result);
    return result;
}

void *mm_alloc(mm_owner_t owner, mm_pool_t pool, size_t bytes, size_t alignment, size_t reserve_bytes) {
    if (owner < 0 || owner >= MM_OWNER_UNTRACKED || pool < 0 || pool >= MM_POOL_COUNT ||
        !bytes || (alignment && (alignment < sizeof(void *) || (alignment & (alignment - 1)))) ||
        bytes > SIZE_MAX - reserve_bytes || bytes > SIZE_MAX - alignment) {
        failure(MM_FAIL_INVALID); return NULL;
    }
    int slot = -1;
    // 分配器在临界区外运行；用代次确认采样后没有别的 broker 分配提交，避免旧 free 覆盖新预留。
    // Run the allocator outside the critical section; validate the epoch so stale free samples cannot override new reservations.
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        portENTER_CRITICAL(&s_lock);
        const uint32_t epoch = s_allocation_epoch;
        portEXIT_CRITICAL(&s_lock);
        const size_t available = heap_caps_get_free_size(pool_caps(pool));
        const size_t largest = heap_caps_get_largest_free_block(pool_caps(pool));
        portENTER_CRITICAL(&s_lock);
        if (epoch != s_allocation_epoch) { portEXIT_CRITICAL(&s_lock); continue; }
        if (available < bytes + reserve_bytes || available - bytes - reserve_bytes < s_pending[pool]) {
            portEXIT_CRITICAL(&s_lock); failure(MM_FAIL_CAPACITY); return NULL;
        }
        if (largest < bytes + alignment) {
            portEXIT_CRITICAL(&s_lock); failure(MM_FAIL_FRAGMENTED); return NULL;
        }
        for (int i = 0; i < MM_TRACKED_ALLOCS; ++i) {
            if (!s_allocations[i].bytes) { slot = i; break; }
        }
        if (slot >= 0) {
            s_allocations[slot] = (allocation_t){.bytes = bytes, .pool = pool, .owner = owner};
            s_pending[pool] += bytes;
            ++s_allocation_epoch;
        }
        portEXIT_CRITICAL(&s_lock);
        if (slot < 0) { failure(MM_FAIL_TRACKING); return NULL; }
        break;
    }
    if (slot < 0) { failure(MM_FAIL_BUSY); return NULL; }
    void *ptr = alignment ? heap_caps_aligned_alloc(alignment, bytes, pool_caps(pool))
                          : heap_caps_malloc(bytes, pool_caps(pool));
    portENTER_CRITICAL(&s_lock);
    s_pending[pool] -= bytes;
    if (ptr) s_allocations[slot].ptr = ptr;
    else memset(&s_allocations[slot], 0, sizeof(s_allocations[slot]));
    ++s_allocation_epoch;
    portEXIT_CRITICAL(&s_lock);
    if (!ptr) failure(MM_FAIL_ALLOCATOR);
    return ptr;
}

bool mm_free(void *ptr) {
    if (!ptr) return true;
    int slot = -1;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < MM_TRACKED_ALLOCS; ++i) {
        if (s_allocations[i].ptr == ptr) { slot = i; s_allocations[i].ptr = NULL; ++s_allocation_epoch; break; }
    }
    portEXIT_CRITICAL(&s_lock);
    if (slot < 0) { failure(MM_FAIL_INVALID); return false; }
    heap_caps_free(ptr);
    portENTER_CRITICAL(&s_lock);
    memset(&s_allocations[slot], 0, sizeof(s_allocations[slot]));
    ++s_allocation_epoch;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

static mm_pool_snapshot_t sample_pool(mm_pool_t pool) {
    const uint32_t caps = pool_caps(pool);
    mm_pool_snapshot_t result = {
        .total = heap_caps_get_total_size(caps),
        .free_bytes = heap_caps_get_free_size(caps),
        .largest_free = heap_caps_get_largest_free_block(caps),
        .minimum_free = heap_caps_get_minimum_free_size(caps),
    };
    result.used_bytes = result.total > result.free_bytes ? result.total - result.free_bytes : 0;
    return result;
}

static void sample_pools(mm_pool_snapshot_t pools[MM_POOL_COUNT]) {
    pools[MM_POOL_INTERNAL] = sample_pool(MM_POOL_INTERNAL);
    pools[MM_POOL_PSRAM] = sample_pool(MM_POOL_PSRAM);
}

void mm_init(void) {
    portENTER_CRITICAL(&s_lock);
    s_callback = NULL;
    s_state = MM_CLEAN_IDLE;
    s_reason = MM_CLEAN_USER_SHORTCUT;
    s_request_id = 0;
    s_generation = 1;
    memset(&s_report, 0, sizeof(s_report));
    // 仅在启动且还没有业务分配时初始化。/ Initialize only at boot before business allocations exist.
    memset(s_allocations, 0, sizeof(s_allocations));
    memset(s_pending, 0, sizeof(s_pending));
    s_allocation_epoch = 0;
    s_failures = 0;
    s_last_failure = MM_FAIL_NONE;
    s_pressure_pending = s_pressure = false;
    s_next_sample_ms = s_next_pressure_ms = 0;
    portEXIT_CRITICAL(&s_lock);
}

void mm_set_clean_callback(mm_clean_callback_t callback) {
    portENTER_CRITICAL(&s_lock);
    s_callback = callback;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t mm_request_clean(mm_clean_reason_t reason, uint32_t *request_id) {
    if (reason < MM_CLEAN_USER_SHORTCUT || reason > MM_CLEAN_PRESSURE) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (s_state == MM_CLEAN_REQUESTED || s_state == MM_CLEAN_RUNNING) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_request_id++;
    if (s_request_id == 0) s_request_id = 1;
    s_reason = reason;
    s_state = MM_CLEAN_REQUESTED;
    s_report = (mm_clean_report_t){
        .request_id = s_request_id,
        .reason = reason,
        .state = MM_CLEAN_REQUESTED,
    };
    if (request_id) *request_id = s_request_id;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

bool mm_clean_active(void) {
    portENTER_CRITICAL(&s_lock);
    const bool active = s_state == MM_CLEAN_REQUESTED || s_state == MM_CLEAN_RUNNING;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

void mm_tick(int64_t now_ms) {
    mm_clean_callback_t callback;
    mm_clean_reason_t reason;
    uint32_t request_id;
    if (now_ms >= s_next_sample_ms) {
        mm_pool_snapshot_t pools[MM_POOL_COUNT];
        sample_pools(pools);
        const bool low = pools[MM_POOL_INTERNAL].free_bytes < 64u * 1024u ||
            pools[MM_POOL_INTERNAL].largest_free < 16u * 1024u ||
            (pools[MM_POOL_PSRAM].total && (pools[MM_POOL_PSRAM].free_bytes < 768u * 1024u ||
             pools[MM_POOL_PSRAM].largest_free < 256u * 1024u));
        portENTER_CRITICAL(&s_lock);
        s_pressure = low;
        const bool request = (low || s_pressure_pending) && now_ms >= s_next_pressure_ms;
        portEXIT_CRITICAL(&s_lock);
        if (request && mm_request_clean(MM_CLEAN_PRESSURE, NULL) == ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_pressure_pending = false;
            portEXIT_CRITICAL(&s_lock);
        }
        s_next_sample_ms = now_ms + 1000;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_state != MM_CLEAN_REQUESTED) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    s_state = MM_CLEAN_RUNNING;
    s_report.state = MM_CLEAN_RUNNING;
    callback = s_callback;
    reason = s_reason;
    request_id = s_request_id;
    portEXIT_CRITICAL(&s_lock);

    mm_pool_snapshot_t before[MM_POOL_COUNT];
    mm_pool_snapshot_t after[MM_POOL_COUNT];
    sample_pools(before);
    const bool completed = callback != NULL && callback(reason);
    sample_pools(after);
    const size_t released_internal = after[MM_POOL_INTERNAL].free_bytes > before[MM_POOL_INTERNAL].free_bytes
        ? after[MM_POOL_INTERNAL].free_bytes - before[MM_POOL_INTERNAL].free_bytes : 0;
    const size_t released_psram = after[MM_POOL_PSRAM].free_bytes > before[MM_POOL_PSRAM].free_bytes
        ? after[MM_POOL_PSRAM].free_bytes - before[MM_POOL_PSRAM].free_bytes : 0;
    const mm_clean_state_t final_state = completed ? MM_CLEAN_DONE : MM_CLEAN_PARTIAL;

    portENTER_CRITICAL(&s_lock);
    s_state = final_state;
    s_generation++;
    s_next_pressure_ms = now_ms + 30000;
    s_report = (mm_clean_report_t){
        .request_id = request_id,
        .reason = reason,
        .state = final_state,
        .completed_ms = now_ms,
        .before_internal_free = before[MM_POOL_INTERNAL].free_bytes,
        .after_internal_free = after[MM_POOL_INTERNAL].free_bytes,
        .before_psram_free = before[MM_POOL_PSRAM].free_bytes,
        .after_psram_free = after[MM_POOL_PSRAM].free_bytes,
        .before_internal_largest = before[MM_POOL_INTERNAL].largest_free,
        .after_internal_largest = after[MM_POOL_INTERNAL].largest_free,
        .before_psram_largest = before[MM_POOL_PSRAM].largest_free,
        .after_psram_largest = after[MM_POOL_PSRAM].largest_free,
        .released_internal = released_internal,
        .released_psram = released_psram,
    };
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI("memory", "MEMORY_CLEAN v=1 id=%u reason=%u complete=%u "
             "internal_before=%u internal_after=%u internal_largest_before=%u internal_largest_after=%u internal_min=%u "
             "psram_before=%u psram_after=%u psram_largest_before=%u psram_largest_after=%u psram_min=%u",
             (unsigned)request_id, (unsigned)reason, (unsigned)completed,
             (unsigned)before[MM_POOL_INTERNAL].free_bytes, (unsigned)after[MM_POOL_INTERNAL].free_bytes,
             (unsigned)before[MM_POOL_INTERNAL].largest_free, (unsigned)after[MM_POOL_INTERNAL].largest_free,
             (unsigned)after[MM_POOL_INTERNAL].minimum_free,
             (unsigned)before[MM_POOL_PSRAM].free_bytes, (unsigned)after[MM_POOL_PSRAM].free_bytes,
             (unsigned)before[MM_POOL_PSRAM].largest_free, (unsigned)after[MM_POOL_PSRAM].largest_free,
             (unsigned)after[MM_POOL_PSRAM].minimum_free);
}

void mm_get_snapshot(mm_snapshot_t *out) {
    if (!out) return;
    mm_pool_snapshot_t pools[MM_POOL_COUNT];
    sample_pools(pools);
    memset(out, 0, sizeof(*out));
    out->updated_ms = esp_timer_get_time() / 1000;
    out->pool[MM_POOL_INTERNAL] = pools[MM_POOL_INTERNAL];
    out->pool[MM_POOL_PSRAM] = pools[MM_POOL_PSRAM];
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < MM_TRACKED_ALLOCS; ++i) {
        const allocation_t *a = &s_allocations[i];
        if (!a->ptr) continue;
        if (a->pool == MM_POOL_INTERNAL) out->owner[a->owner].internal_bytes += a->bytes;
        else out->owner[a->owner].psram_bytes += a->bytes;
    }
    out->generation = s_generation;
    out->clean_state = s_state;
    out->clean_reason = s_reason;
    out->clean_request_id = s_request_id;
    out->clean_released_bytes = s_report.released_internal + s_report.released_psram;
    out->allocation_failures = s_failures;
    out->last_failure = s_last_failure;
    out->pressure = s_pressure;
    portEXIT_CRITICAL(&s_lock);
    size_t internal = 0, psram = 0;
    for (int i = 0; i < MM_OWNER_UNTRACKED; ++i) {
        internal += out->owner[i].internal_bytes;
        psram += out->owner[i].psram_bytes;
    }
    // 未迁移的系统和第三方分配保留为未知，不把它们伪装成零占用。
    // Keep unmigrated system/third-party allocations unknown instead of pretending they use zero bytes.
    out->owner[MM_OWNER_UNTRACKED].internal_bytes = pools[MM_POOL_INTERNAL].used_bytes > internal
        ? pools[MM_POOL_INTERNAL].used_bytes - internal : 0;
    out->owner[MM_OWNER_UNTRACKED].psram_bytes = pools[MM_POOL_PSRAM].used_bytes > psram
        ? pools[MM_POOL_PSRAM].used_bytes - psram : 0;
}

void mm_get_clean_report(mm_clean_report_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_report;
    portEXIT_CRITICAL(&s_lock);
}

const char *mm_owner_name(mm_owner_t owner) {
    static const char *const names[MM_OWNER_COUNT] = {
        "系统/显示", "阅读/字体", "UI 缓存", "网络传输", "文件任务", "未追踪/外部",
    };
    return owner >= 0 && owner < MM_OWNER_COUNT ? names[owner] : "未知";
}
