#!/usr/bin/env python3
"""中文：验证真实高峰闸门和池检查。/ Exercise the real peak gate and pool checks."""
from pathlib import Path
import subprocess
import tempfile

from home_cover_cache_host_test import function

ROOT = Path(__file__).resolve().parents[1]
HEADERS = {
    "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG 0x102\n#define ESP_ERR_NO_MEM 0x101\nconst char *esp_err_to_name(esp_err_t);\n",
    "freertos/FreeRTOS.h": "#pragma once\n",
    "freertos/portmacro.h": """#pragma once
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL pthread_mutex_lock
#define portEXIT_CRITICAL pthread_mutex_unlock
#define portENTER_CRITICAL_SAFE pthread_mutex_lock
#define portEXIT_CRITICAL_SAFE pthread_mutex_unlock
""",
    "esp_heap_caps.h": """#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_INTERNAL 1u
#define MALLOC_CAP_SPIRAM 2u
#define MALLOC_CAP_8BIT 4u
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_largest_free_block(uint32_t);
""",
}
SOURCE = r'''
#include "memory_peak.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "freertos/portmacro.h"

static memory_peak_heap_t heap = {30000, 18000, 90000, 20000};
static memory_peak_token_t worker_token;
const char *esp_err_to_name(esp_err_t err) { (void)err; return "error"; }
static int64_t now = 5555;
int64_t esp_timer_get_time(void) { return now; }
size_t heap_caps_get_free_size(uint32_t c) { return c & MALLOC_CAP_INTERNAL ? heap.internal_free : heap.psram_free; }
size_t heap_caps_get_largest_free_block(uint32_t c) { return c & MALLOC_CAP_INTERNAL ? heap.internal_block : heap.psram_block; }
static void *finish_worker(void *ignored) {
    (void)ignored;
    memory_peak_finish(worker_token);
    return NULL;
}
static void wifi_association_test(void);
static void ble_connection_test(void);
int main(void) {
    const memory_peak_heap_t need = {12288, 12288, 16384, 16384};
    assert(memory_peak_check(need) == ESP_OK);
    heap.internal_free = 10000; assert(memory_peak_check(need) == MEMORY_PEAK_CAPACITY);
    heap.internal_free = 30000; heap.psram_free = 10000; assert(memory_peak_check(need) == MEMORY_PEAK_CAPACITY);
    heap.psram_free = 90000; heap.psram_block = 8000; assert(memory_peak_check(need) == MEMORY_PEAK_FRAGMENT);
    heap.psram_block = 20000; heap.internal_block = 8000; assert(memory_peak_check(need) == MEMORY_PEAK_FRAGMENT);
    // 受控压力：交还连续缓存后可完成操作，不只报告拒绝。/ Controlled pressure: cache return makes admission succeed.
    heap.internal_block += 8000; assert(memory_peak_check(need) == ESP_OK);

    memory_peak_token_t old, next;
    assert(memory_peak_try_begin(MEMORY_PEAK_BLE_CONNECT, &old) == ESP_OK);
    const esp_err_t rejected = memory_peak_try_begin(MEMORY_PEAK_CELLULAR, &next);
    assert(memory_peak_is_busy(rejected) && !next);
    // 创建失败或取消后的唯一所有者完成；下一代不接受迟到的完成。/ Failed creation finishes; late completion cannot end the new generation.
    memory_peak_finish(old);
    assert(memory_peak_try_begin(MEMORY_PEAK_CELLULAR, &worker_token) == ESP_OK && old != worker_token);
    assert(!strcmp(memory_peak_hint(rejected), "蓝牙翻页器正在连接，请稍后重试"));
    assert(!memory_peak_is_busy(ESP_ERR_NO_MEM) && !memory_peak_is_busy(MEMORY_PEAK_CONFLICT));
    memory_peak_finish(old);
    assert(memory_peak_is_busy(memory_peak_try_begin(MEMORY_PEAK_WIFI, &next)) && !next);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, finish_worker, NULL));
    assert(!pthread_join(thread, NULL));
    memory_peak_finish(worker_token);
    assert(memory_peak_try_begin(MEMORY_PEAK_READER, &next) == ESP_OK);
    const memory_peak_token_t reader = next;
    const memory_peak_heap_t prior = heap;
    heap.internal_block = 1024;
    assert(memory_peak_check_at(prior, need) == ESP_OK && memory_peak_check(need) == MEMORY_PEAK_FRAGMENT);
    heap = prior;
    assert(memory_peak_is_busy(memory_peak_try_begin(MEMORY_PEAK_WIFI, &next)));
    memory_peak_finish(reader);
    wifi_association_test();
    ble_connection_test();
}
'''
SOURCE += r'''
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_WIFI_NOT_INIT 0x3001
#define ESP_ERR_WIFI_NOT_STARTED 0x3002
#define ESP_LOGW(...) ((void)0)
enum { WIFI_TRANSFER_MODE_STA, WIFI_TRANSFER_UPLOADING, WIFI_TRANSFER_ERROR };
typedef struct {
    bool cancel_pending, online, pending, failed;
    unsigned attempts;
    int64_t deadline_ms, next_ms;
} transfer_connection_t;
static transfer_connection_t s_connection;
static bool s_started = true, s_stopping, s_config_busy, s_connect_calling;
static memory_peak_token_t s_association_peak;
static struct { bool network_ready; char url[64]; int state, mode; esp_err_t last_error; } s_status;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned connect_calls, disconnect_calls, stop_calls, s_cancel_attempts;
static bool s_upload_active, s_time_active;
static esp_err_t connect_result, disconnect_result, stop_result;
static esp_err_t esp_wifi_connect(void) {
    memory_peak_token_t other;
    assert(s_connect_calling && memory_peak_is_busy(memory_peak_try_begin(MEMORY_PEAK_READER, &other)));
    ++connect_calls;
    return connect_result;
}
static esp_err_t esp_wifi_disconnect(void) { assert(s_connect_calling); ++disconnect_calls; return disconnect_result; }
static esp_err_t esp_wifi_stop(void) { assert(s_connect_calling && s_stopping); ++stop_calls; return stop_result; }
'''
policy = ROOT / "components/wifi_transfer/transfer_policy.h"
for name in ("connection_begin", "connection_lost", "connection_poll"):
    SOURCE += function(name, policy) + "\n"
for name in ("wifi_transfer_service_poll", "wifi_transfer_recover_stuck_sta"):
    SOURCE += function(name, ROOT / "components/wifi_transfer/wifi_transfer.c") + "\n"
SOURCE += r'''
static void wifi_association_test(void) {
    const int64_t initial = now / 1000;
    connection_begin(&s_connection, initial);
    memory_peak_token_t blocking, other;
    assert(memory_peak_try_begin(MEMORY_PEAK_READER, &blocking) == ESP_OK);
    wifi_transfer_service_poll();
    assert(!s_connection.attempts && connect_calls == 0);
    memory_peak_finish(blocking);
    wifi_transfer_service_poll();
    assert(connect_calls == 0);
    now += 1000000;
    wifi_transfer_service_poll();
    assert(s_association_peak && connect_calls == 1);
    assert(memory_peak_is_busy(memory_peak_try_begin(MEMORY_PEAK_READER, &other)));
    s_connection.online = true;
    wifi_transfer_service_poll();
    assert(!s_association_peak && memory_peak_try_begin(MEMORY_PEAK_READER, &other) == ESP_OK);
    memory_peak_finish(other);
    connection_lost(&s_connection, now / 1000);
    now += 1000000;
    wifi_transfer_service_poll();
    assert(s_association_peak && connect_calls == 2);
    now += 30000000;
    disconnect_result = ESP_FAIL;
    wifi_transfer_service_poll();
    assert(s_association_peak && disconnect_calls == 1 && s_status.last_error == ESP_ERR_TIMEOUT);
    assert(memory_peak_is_busy(memory_peak_try_begin(MEMORY_PEAK_READER, &other)));
    now += 1000000;
    wifi_transfer_service_poll();
    assert(disconnect_calls == 1);
    now += 1000000;
    disconnect_result = ESP_OK;
    wifi_transfer_service_poll();
    assert(s_association_peak && disconnect_calls == 2);
    s_connection.cancel_pending = false;  // 模拟断开事件。/ Simulate the disconnect event.
    wifi_transfer_service_poll();
    assert(!s_association_peak);
    assert(memory_peak_try_begin(MEMORY_PEAK_READER, &other) == ESP_OK);
    memory_peak_finish(other);
    connection_begin(&s_connection, now / 1000);
    connect_result = ESP_ERR_NO_MEM;
    wifi_transfer_service_poll();
    assert(!s_association_peak && s_connection.pending && s_status.last_error == ESP_ERR_NO_MEM);
    connect_result = ESP_OK;
    now += 1000000;
    wifi_transfer_service_poll();
    now += 30000000;
    for (int i = 0; i < 3; ++i) {
        wifi_transfer_service_poll();
        assert(s_association_peak);
        now += 2000000;
    }
    s_upload_active = true;
    wifi_transfer_recover_stuck_sta();
    assert(stop_calls == 0 && s_association_peak);
    s_upload_active = false;
    stop_result = ESP_FAIL;
    wifi_transfer_recover_stuck_sta();
    assert(stop_calls == 1 && s_association_peak && s_status.last_error == ESP_ERR_NO_MEM);
    assert(memory_peak_is_busy(memory_peak_try_begin(MEMORY_PEAK_READER, &other)));
    wifi_transfer_recover_stuck_sta();
    assert(stop_calls == 1);
    now += 2000000;
    stop_result = ESP_OK;
    wifi_transfer_recover_stuck_sta();
    assert(stop_calls == 2 && !s_association_peak && !s_started);
    assert(memory_peak_try_begin(MEMORY_PEAK_READER, &other) == ESP_OK);
    wifi_transfer_recover_stuck_sta();
    memory_peak_token_t unrelated;
    assert(memory_peak_is_busy(memory_peak_try_begin(MEMORY_PEAK_BLE, &unrelated)));
    memory_peak_finish(other);
}
'''
SOURCE += r'''
#define BLE_PT_CONNECT_DEFERRED (-1)
#define BLE_HS_EINVAL 3
#define BLE_PT_HCI_MEM_CAPACITY 0x207
typedef struct { uint8_t val[6], type; } ble_addr_t;
struct ble_gap_event;
static atomic_uint s_connect_peak;
static bool s_connecting, s_rest_known, s_hints_cached, s_has_keyboard_page, s_has_consumer_page;
static unsigned s_held_usage, s_subscribed_count;
static uint8_t s_own_addr_type, s_conn_addr_type;
static int connect_rc;
static void raw_reset(void) {}
static bool hints_load(const char *addr, bool *keyboard, bool *consumer) {
    (void)addr; (void)keyboard; (void)consumer; return false;
}
static int gap_cb(struct ble_gap_event *event, void *arg) { (void)event; (void)arg; return 0; }
static int ble_gap_connect(uint8_t own, const ble_addr_t *peer, int timeout, void *args,
                           int (*cb)(struct ble_gap_event *, void *), void *context) {
    (void)own; (void)args; (void)context;
    assert(peer->val[0] == 6 && timeout == 10000 && cb == gap_cb);
    memory_peak_token_t next;
    assert(atomic_load(&s_connect_peak) && s_connecting);
    assert(memory_peak_is_busy(memory_peak_try_begin(MEMORY_PEAK_READER, &next)));
    return connect_rc;
}
'''
ble = ROOT / "components/ble_page_turner/src/ble_page_turner.c"
SOURCE += function("finish_connect_peak", ble) + "\n" + function("start_connect", ble)
SOURCE += r'''
static void ble_connection_test(void) {
    const char *peer = "01:02:03:04:05:06";
    memory_peak_token_t reader;
    assert(memory_peak_try_begin(MEMORY_PEAK_READER, &reader) == ESP_OK);
    assert(start_connect(peer, 0) == BLE_PT_CONNECT_DEFERRED && !s_connecting);
    memory_peak_finish(reader);
    connect_rc = BLE_PT_HCI_MEM_CAPACITY;
    assert(start_connect(peer, 0) == BLE_PT_HCI_MEM_CAPACITY && !atomic_load(&s_connect_peak) && !s_connecting);
    connect_rc = 0;
    assert(start_connect(peer, 0) == 0 && atomic_load(&s_connect_peak));
    assert(memory_peak_is_busy(memory_peak_try_begin(MEMORY_PEAK_READER, &reader)));
    // 模拟 GAP 回报或 host 退出；不以调用返回作为完成。/ Simulate GAP completion or host exit, not the call returning.
    finish_connect_peak();
    assert(memory_peak_try_begin(MEMORY_PEAK_READER, &reader) == ESP_OK);
    finish_connect_peak();
    memory_peak_finish(reader);
}
'''
with tempfile.TemporaryDirectory(prefix="memory-peak-") as folder:
    work = Path(folder)
    for name, text in HEADERS.items():
        path = work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    (work / "test.c").write_text(SOURCE)
    binary = work / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread", "-g",
                    "-fsanitize=address,undefined", "-Icomponents/memory_peak/include", "-I" + folder,
                    str(work / "test.c"), "components/memory_peak/memory_peak.c", "-o", str(binary)],
                   cwd=ROOT, check=True, timeout=10)
    subprocess.run([str(binary)], check=True, timeout=10)
