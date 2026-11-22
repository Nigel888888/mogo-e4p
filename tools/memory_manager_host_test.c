/* 中文：用独立的堆模型验证管理器；模型计数与 broker 账本分别维护。
 * English: Validate the manager with an independent heap model whose counts are separate from the broker ledger.
 */
#include "memory_manager.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t admission_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t admission_changed = PTHREAD_COND_INITIALIZER;
static _Thread_local int admission_role;
static bool slow_sampled, fast_done;
static void *race_results[2];
static struct { void *ptr; size_t bytes; int pool; } allocations[256];
static size_t totals[] = {1024u * 1024u, 8u * 1024u * 1024u};
static size_t used[2], extra[2], largest[2];
static unsigned attempts, callbacks;
static bool fail_allocator, partial;
static void *cache;
static int pool_for(uint32_t caps) {
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) || caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return (caps & MALLOC_CAP_SPIRAM) ? 1 : 0;
}
size_t heap_caps_get_total_size(uint32_t caps) { return totals[pool_for(caps)]; }
size_t heap_caps_get_free_size(uint32_t caps) {
    const int pool = pool_for(caps);
    pthread_mutex_lock(&heap_lock);
    size_t result = totals[pool] - used[pool] - extra[pool];
    pthread_mutex_unlock(&heap_lock);
    if (admission_role) {
        // 两个任务读取同一余量，较慢任务等到另一分配提交后再返回旧样本。
        // Both tasks sample the same free space; delay one until the other allocation commits.
        const int role = admission_role;
        admission_role = 0;
        pthread_mutex_lock(&admission_lock);
        if (role == 1) {
            while (!slow_sampled) pthread_cond_wait(&admission_changed, &admission_lock);
        } else {
            slow_sampled = true;
            pthread_cond_broadcast(&admission_changed);
            while (!fast_done) pthread_cond_wait(&admission_changed, &admission_lock);
        }
        pthread_mutex_unlock(&admission_lock);
    }
    return result;
}
size_t heap_caps_get_largest_free_block(uint32_t caps) {
    const int pool = pool_for(caps);
    return largest[pool] ? largest[pool] : heap_caps_get_free_size(caps);
}
size_t heap_caps_get_minimum_free_size(uint32_t caps) { return heap_caps_get_free_size(caps) / 2; }
int64_t esp_timer_get_time(void) { return 123456000; }
void *heap_caps_aligned_alloc(size_t alignment, size_t bytes, uint32_t caps) {
    const int pool = pool_for(caps);
    pthread_mutex_lock(&heap_lock);
    ++attempts;
    if (fail_allocator) { pthread_mutex_unlock(&heap_lock); return NULL; }
    assert(bytes <= totals[pool] - used[pool] - extra[pool]);
    void *ptr = NULL;
    assert(!posix_memalign(&ptr, alignment, bytes) && ptr);
    unsigned slot = 0;
    while (slot < 256 && allocations[slot].ptr) ++slot;
    assert(slot < 256);
    allocations[slot].ptr = ptr; allocations[slot].bytes = bytes; allocations[slot].pool = pool;
    used[pool] += bytes;
    pthread_mutex_unlock(&heap_lock);
    return ptr;
}
void *heap_caps_malloc(size_t bytes, uint32_t caps) { return heap_caps_aligned_alloc(16, bytes, caps); }
void heap_caps_free(void *ptr) {
    pthread_mutex_lock(&heap_lock);
    unsigned slot = 0;
    while (slot < 256 && allocations[slot].ptr != ptr) ++slot;
    assert(slot < 256);
    used[allocations[slot].pool] -= allocations[slot].bytes;
    allocations[slot].ptr = NULL;
    free(ptr);
    pthread_mutex_unlock(&heap_lock);
}
static bool clean(mm_clean_reason_t reason) {
    assert(reason >= MM_CLEAN_USER_SHORTCUT && reason <= MM_CLEAN_PRESSURE);
    ++callbacks;
    mm_snapshot_t snapshot; mm_get_snapshot(&snapshot);
    mm_clean_report_t report; mm_get_clean_report(&report);
    assert(mm_clean_active() && snapshot.clean_state == MM_CLEAN_RUNNING && report.state == MM_CLEAN_RUNNING);
    uint32_t id = 99;
    assert(mm_request_clean(reason, &id) == ESP_ERR_INVALID_STATE && id == 99);
    if (cache) { assert(mm_free(cache)); cache = NULL; }
    return !partial;
}
static void *allocation_thread(void *arg) {
    const mm_pool_t pool = (mm_pool_t)(uintptr_t)arg;
    for (unsigned i = 0; i < 400; ++i) {
        void *ptr = mm_alloc(MM_OWNER_READER, pool, 128 + i % 16, 16, 1024);
        assert(ptr && !((uintptr_t)ptr & 15));
        mm_snapshot_t snapshot; mm_get_snapshot(&snapshot);
        assert(mm_free(ptr));
    }
    return NULL;
}
static void *stale_sample_thread(void *arg) {
    const int role = (int)(uintptr_t)arg;
    admission_role = role;
    race_results[role - 1] = mm_alloc(MM_OWNER_READER, MM_POOL_PSRAM, 4096, 0, 32768);
    if (role == 1) {
        pthread_mutex_lock(&admission_lock);
        fast_done = true;
        pthread_cond_broadcast(&admission_changed);
        pthread_mutex_unlock(&admission_lock);
    }
    return NULL;
}
int main(void) {
    mm_snapshot_t snapshot; mm_clean_report_t report; uint32_t id;
    mm_init(); mm_set_clean_callback(clean);
    cache = mm_alloc(MM_OWNER_READER, MM_POOL_PSRAM, 4096, 64, 1024u * 1024u);
    assert(cache && !((uintptr_t)cache & 63));
    extra[0] = 8192; extra[1] = 16384;
    mm_get_snapshot(&snapshot);
    assert(snapshot.owner[MM_OWNER_READER].psram_bytes == 4096);
    assert(snapshot.owner[MM_OWNER_UNTRACKED].internal_bytes == extra[0]);
    assert(snapshot.owner[MM_OWNER_UNTRACKED].psram_bytes == extra[1]);
    assert(snapshot.pool[MM_POOL_PSRAM].used_bytes == 4096 + extra[1]);
    assert(mm_request_clean(MM_CLEAN_USER_SHORTCUT, &id) == ESP_OK && id == 1 && mm_clean_active());
    assert(mm_request_clean(MM_CLEAN_MEMORY_PAGE, NULL) == ESP_ERR_INVALID_STATE);
    mm_tick(0); mm_get_clean_report(&report);
    assert(callbacks == 1 && !cache && report.state == MM_CLEAN_DONE && report.released_psram == 4096);
    assert(report.before_psram_largest + 4096 == report.after_psram_largest && !mm_clean_active());
    mm_tick(1000); assert(callbacks == 1);
    partial = true;
    assert(mm_request_clean(MM_CLEAN_MEMORY_PAGE, &id) == ESP_OK && id == 2);
    mm_tick(2000); mm_get_clean_report(&report);
    assert(callbacks == 2 && report.state == MM_CLEAN_PARTIAL && !report.released_internal && !report.released_psram);
    assert(mm_request_clean((mm_clean_reason_t)99, NULL) == ESP_ERR_INVALID_ARG);
    partial = false; extra[0] = extra[1] = 0;
    assert(mm_check_capacity(MM_POOL_INTERNAL, totals[0], totals[0]) == MM_FAIL_NONE);
    assert(mm_check_capacity(MM_POOL_INTERNAL, totals[0] + 1, 1) == MM_FAIL_CAPACITY);
    largest[0] = 1024;
    assert(mm_check_capacity(MM_POOL_INTERNAL, 4096, 1025) == MM_FAIL_FRAGMENTED);
    largest[0] = 0;
    unsigned before = attempts;
    assert(!mm_alloc(MM_OWNER_READER, MM_POOL_PSRAM, SIZE_MAX, 16, 1));
    assert(!mm_alloc(MM_OWNER_READER, MM_POOL_PSRAM, 10, 3, 0));
    assert(!mm_alloc(MM_OWNER_READER, MM_POOL_INTERNAL, totals[0], 0, 1));
    largest[1] = 1024;
    assert(!mm_alloc(MM_OWNER_READER, MM_POOL_PSRAM, 2048, 16, 0));
    mm_get_snapshot(&snapshot); assert(snapshot.last_failure == MM_FAIL_FRAGMENTED && attempts == before);
    largest[1] = 0; fail_allocator = true;
    assert(!mm_alloc(MM_OWNER_READER, MM_POOL_PSRAM, 2048, 0, 0));
    mm_get_snapshot(&snapshot); assert(snapshot.last_failure == MM_FAIL_ALLOCATOR);
    fail_allocator = false;
    int foreign;
    assert(!mm_free(&foreign));
    void *slots[MM_TRACKED_ALLOCS];
    for (unsigned i = 0; i < MM_TRACKED_ALLOCS; ++i) { slots[i] = mm_alloc(MM_OWNER_READER, MM_POOL_PSRAM, 32, 0, 0); assert(slots[i]); }
    before = attempts;
    assert(!mm_alloc(MM_OWNER_READER, MM_POOL_PSRAM, 32, 0, 0) && attempts == before);
    for (unsigned i = 0; i < MM_TRACKED_ALLOCS; ++i) assert(mm_free(slots[i]));
    assert(!mm_free(slots[0]) && mm_free(NULL));
    assert(!used[0] && !used[1]);
    mm_init(); mm_set_clean_callback(clean); callbacks = 0;
    extra[0] = totals[0] - 32000;
    mm_tick(0); assert(callbacks == 1);
    mm_tick(1000); mm_tick(29999); assert(callbacks == 1);
    mm_tick(31000); assert(callbacks == 2);
    extra[0] = 0;
    mm_init(); callbacks = 0;
    assert(mm_request_clean(MM_CLEAN_MEMORY_PAGE, NULL) == ESP_OK);
    mm_tick(0); mm_get_clean_report(&report); assert(report.state == MM_CLEAN_PARTIAL && !callbacks);
    mm_init();
    pthread_t threads[4];
    extra[1] = totals[1] - 32768 - 4096;
    for (unsigned i = 0; i < 2; ++i)
        assert(!pthread_create(&threads[i], NULL, stale_sample_thread, (void *)(uintptr_t)(i + 1)));
    for (unsigned i = 0; i < 2; ++i) assert(!pthread_join(threads[i], NULL));
    assert(race_results[0] && !race_results[1] && used[1] == 4096);
    assert(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == 32768);
    assert(mm_free(race_results[0]));
    extra[1] = 0;
    mm_init();
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, allocation_thread, (void *)(uintptr_t)(i % 2)));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    mm_get_snapshot(&snapshot);
    assert(!used[0] && !used[1] && !snapshot.allocation_failures);
    puts("PASS: strict pools, alignment, reserve/fragmentation/allocator failures, bounded ledger, owner totals, duplicate/foreign frees, reentrant cleanup rejection, partial results, automatic cooldown, stale concurrent admission and concurrent allocations");
    return 0;
}
