#!/usr/bin/env python3
"""验证 UHCI RX 池、回退及缓存交接。/ Verify UHCI RX pools, fallback and cache handoff."""
from pathlib import Path
import subprocess
import sys
import tempfile

from home_cover_cache_host_test import function
from run_reader_baseline import toolchain

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "components/78__uart-uhci/src/uart_uhci.cc"

STUBS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#define UHCI_RX_PSRAM_SUPPORTED 1
#define ALIGN_UP(n, a) (((n) + (a) - 1) & ~((a) - 1))
#define MAX_OF(a, b) ((a) > (b) ? (a) : (b))
#define ESP_RETURN_ON_FALSE(condition, e, ...) do { if (!(condition)) return e; } while (0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { const auto e = (expr); if (e != ESP_OK) return e; } while (0)
#define ESP_DRAM_LOGI(...) ((void)0)
#define ESP_DRAM_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_NO_MEM = 1, ESP_ERR_INVALID_STATE = 2;
constexpr uint32_t MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_SPIRAM = 2, MALLOC_CAP_DMA = 4;
constexpr int ESP_CACHE_MSYNC_FLAG_DIR_M2C = 8;
constexpr size_t DMA_DESCRIPTOR_BUFFER_MAX_SIZE = 4095;
struct Alloc { size_t capacity; uint32_t caps; };
static std::map<void*, Alloc> live;
static int psram_allow = -1, dram_allow = -1;
static unsigned invalidations, mounts, barriers, pm_count;
static bool fail_sync, fail_mount;
static int next_buf;
static size_t sync_size;
static void* heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    assert(caps == MALLOC_CAP_INTERNAL);
    void* p = calloc(n, size);
    if (p) live[p] = {n * size, caps};
    return p;
}
static void* heap_caps_aligned_alloc(size_t align, size_t size, uint32_t caps) {
    const bool external = caps == (MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    assert(external || caps == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    assert(align == (external ? 64u : 32u) && size == ALIGN_UP(1601u, align));
    int& remaining = external ? psram_allow : dram_allow;
    if (!remaining) return nullptr;
    if (remaining > 0) --remaining;
    void* p = nullptr;
    if (posix_memalign(&p, align, size) != 0) return nullptr;
    live[p] = {size, caps};
    return p;
}
static void heap_caps_free(void* p) { assert(live.erase(p) == 1); free(p); }
struct uart_dev_t {};
#define UART_LL_GET_HW(...) nullptr
static void uart_ll_rxfifo_rst(uart_dev_t*) {}
static esp_err_t esp_cache_msync(void* p, size_t len, int flags) {
    const auto a = live.at(p);
    size_t cache_line = a.caps & MALLOC_CAP_SPIRAM ? 64u : 32u;
    assert((uintptr_t)p % cache_line == 0 && len % cache_line == 0 && len <= a.capacity);
    assert(flags == ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (fail_sync) return ESP_ERR_INVALID_STATE;
    ++invalidations;
    sync_size = len;
    return ESP_OK;
}
static void esp_psram_mspi_mb() { ++barriers; }
struct gdma_buffer_mount_config_t { void* buffer; size_t length, buffer_alignment; struct { bool mark_eof; } flags; };
static esp_err_t gdma_reset(int) { return ESP_OK; }
static esp_err_t gdma_start(int, uintptr_t) { assert(mounts == 4); return ESP_OK; }
static esp_err_t gdma_stop(int) { return ESP_OK; }
static esp_err_t gdma_link_mount_buffers(int, int index, const gdma_buffer_mount_config_t* m, size_t n, int*) {
    assert(n == 1 && index == (int)mounts && invalidations == mounts + 1 && m->flags.mark_eof);
    assert(m->length == live.at(m->buffer).capacity);
    assert(m->buffer_alignment == ((live.at(m->buffer).caps & MALLOC_CAP_SPIRAM) ? 64u : 32u));
    if (fail_mount) return ESP_ERR_INVALID_STATE;
    ++mounts;
    return ESP_OK;
}
enum gdma_lli_owner_t { GDMA_LLI_OWNER_CPU, GDMA_LLI_OWNER_DMA };
static esp_err_t gdma_link_get_owner(int, int i, gdma_lli_owner_t* owner) {
    *owner = i == next_buf ? GDMA_LLI_OWNER_CPU : GDMA_LLI_OWNER_DMA;
    return ESP_OK;
}
static void gdma_link_set_owner(int, int i, gdma_lli_owner_t owner) {
    assert(i == next_buf && owner == GDMA_LLI_OWNER_DMA);
    next_buf = -1;
}
static size_t gdma_link_get_length(int, int) { return 65; }
static void gdma_append(int) {}
static void esp_pm_lock_acquire(int) { ++pm_count; }
static void esp_pm_lock_release(int) { assert(pm_count); --pm_count; }
class UartUhci {
public:
    struct RxBuffer { uint8_t* data; size_t capacity, size; uint32_t index; };
    struct RxEventData { RxBuffer* buffer; size_t recv_size; };
    struct BufferPoolConfig { size_t buffer_count, buffer_size; bool prefer_psram; };
    RxBuffer* rx_buffer_pool_ = nullptr;
    int rx_dma_chan_ = 1, rx_dma_link_ = 1, pm_lock_ = 1;
    uintptr_t rx_dma_link_head_addr_ = 0;
    size_t rx_int_mem_align_ = 16, rx_ext_mem_align_ = 32, ext_mem_cache_line_ = 64, int_mem_cache_line_ = 32;
    size_t rx_pool_size_ = 0, rx_buffer_size_ = 0, rx_payload_align_ = 0, rx_cache_line_ = 0;
    bool rx_payload_psram_ = false;
    std::atomic<bool> rx_running_{false}, buffer_overflow_{false};
    int last_rx_buf_idx_ = -1;
    bool (*rx_callback_)(const RxEventData&, void*) = nullptr;
    void* rx_callback_user_data_ = nullptr;
    esp_err_t InitRxBufferPool(const BufferPoolConfig&);
    void DeinitRxBufferPool();
    esp_err_t RemountAndRestartDma(bool flush = false);
    esp_err_t StartReceive();
    esp_err_t StopReceive();
    bool HandleGdmaRxDone(uintptr_t, bool);
    void ReturnBufferFromISR(RxBuffer*);
};
'''

TESTS = r'''
static bool got_payload(const UartUhci::RxEventData& result, void*) {
    assert(result.buffer->size == 65 && result.recv_size == 65 && result.buffer->index == 0);
    assert(invalidations == 5 && barriers == 1 && sync_size == 128);
    return true;
}
int main() {
    UartUhci u;
    assert(u.InitRxBufferPool({4, 1601, true}) == ESP_OK && u.rx_payload_psram_ && live.size() == 5);
    assert(u.StartReceive() == ESP_OK && mounts == 4 && invalidations == 4 && pm_count == 1);
    u.rx_callback_ = got_payload;
    next_buf = 0;
    assert(u.HandleGdmaRxDone(0, true) && u.last_rx_buf_idx_ == 0);
    next_buf = 1;
    fail_sync = true;
    assert(!u.HandleGdmaRxDone(0, true) && next_buf == -1 && barriers == 2);
    fail_sync = false;
    u.StopReceive();
    assert(!u.rx_running_ && !pm_count);
    u.DeinitRxBufferPool();
    assert(live.empty() && !u.rx_payload_psram_ && !u.rx_cache_line_);

    psram_allow = 2;
    assert(u.InitRxBufferPool({4, 1601, true}) == ESP_OK && !u.rx_payload_psram_ && live.size() == 5);
    for (const auto& a : live) assert(a.second.caps & MALLOC_CAP_INTERNAL);
    mounts = invalidations = 0;
    fail_mount = true;
    assert(u.StartReceive() == ESP_ERR_INVALID_STATE && !pm_count && !u.rx_running_);
    u.DeinitRxBufferPool();
    assert(live.empty());

    psram_allow = -1;
    u.rx_ext_mem_align_ = size_t{1} << 31;
    assert(u.InitRxBufferPool({4, 1601, true}) == ESP_OK && !u.rx_payload_psram_ && live.size() == 5);
    u.DeinitRxBufferPool();
    assert(live.empty());
    u.rx_ext_mem_align_ = 32;
    psram_allow = 2;
    dram_allow = 3;
    assert(u.InitRxBufferPool({4, 1601, true}) == ESP_ERR_NO_MEM);
    assert(live.size() == 1);
    u.DeinitRxBufferPool();
    assert(live.empty());
    puts("PASS: PSRAM RX alignment, cache barrier, dropped stale data, failure cleanup and DRAM fallback");
}
'''

with tempfile.TemporaryDirectory(prefix="cellular-uhci-buffers-") as folder:
    work = Path(folder)
    env, compiler = toolchain(work)
    unit = work / "test.cc"
    functions = ("InitRxBufferPool", "DeinitRxBufferPool", "RemountAndRestartDma", "StartReceive", "StopReceive",
                 "HandleGdmaRxDone", "ReturnBufferFromISR")
    unit.write_text(STUBS + "\n".join(function(f"UartUhci::{name}", SOURCE) for name in functions) + TESTS)
    binary = work / "test"
    subprocess.run(compiler + ["-x", "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                               "-fsanitize=address,undefined", str(unit), "-o", str(binary)] +
                   (["-lc++"] if sys.platform == "darwin" else ["-lstdc++"]), cwd=ROOT, env=env, check=True)
    subprocess.run([str(binary)], cwd=ROOT, env=env, check=True)
