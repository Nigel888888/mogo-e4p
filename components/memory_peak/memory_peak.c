/*
 * 中文：固定容量的业务高峰准入，不接管分配器。
 * English: Fixed-size business peak admission, not an allocator.
 */
#include "memory_peak.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define INTERNAL_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef struct {
    memory_peak_token_t token;
    memory_peak_kind_t kind;
} operation_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static operation_t s_active;
static uint32_t s_generation;

memory_peak_heap_t memory_peak_snapshot(void) {
    return (memory_peak_heap_t){
        heap_caps_get_free_size(INTERNAL_CAPS), heap_caps_get_largest_free_block(INTERNAL_CAPS),
        heap_caps_get_free_size(PSRAM_CAPS), heap_caps_get_largest_free_block(PSRAM_CAPS),
    };
}

esp_err_t memory_peak_try_begin(memory_peak_kind_t kind, memory_peak_token_t *token) {
    if (!token) return ESP_ERR_INVALID_ARG;
    *token = 0;
    if ((unsigned)kind > MEMORY_PEAK_CLOUDZAO) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (s_active.token) {
        const memory_peak_kind_t owner = s_active.kind;
        portEXIT_CRITICAL(&s_lock);
        return MEMORY_PEAK_BUSY_OWNER_BASE + owner;
    }
    if (++s_generation == 0) ++s_generation;
    *token = s_generation;
    s_active = (operation_t){.token = *token, .kind = kind};
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t memory_peak_check(memory_peak_heap_t need) {
    return memory_peak_check_at(memory_peak_snapshot(), need);
}

esp_err_t memory_peak_check_at(memory_peak_heap_t now, memory_peak_heap_t need) {
    if (now.internal_free < need.internal_free || now.psram_free < need.psram_free)
        return MEMORY_PEAK_CAPACITY;
    if (now.internal_block < need.internal_block || now.psram_block < need.psram_block)
        return MEMORY_PEAK_FRAGMENT;
    return ESP_OK;
}

void memory_peak_finish(memory_peak_token_t token) {
    if (!token) return;
    portENTER_CRITICAL(&s_lock);
    if (s_active.token == token) s_active.token = 0;
    portEXIT_CRITICAL(&s_lock);
}

bool memory_peak_is_memory_error(esp_err_t error) {
    return error == ESP_ERR_NO_MEM || error == MEMORY_PEAK_CAPACITY || error == MEMORY_PEAK_FRAGMENT;
}

const char *memory_peak_hint(esp_err_t error) {
    if (error >= MEMORY_PEAK_BUSY_OWNER_BASE && memory_peak_is_busy(error)) {
        switch (error - MEMORY_PEAK_BUSY_OWNER_BASE) {
        case MEMORY_PEAK_READER: return "正在打开书籍，请稍后重试";
        case MEMORY_PEAK_WIFI: return "Wi-Fi 正在启动，请稍后重试";
        case MEMORY_PEAK_WIFI_SCAN: return "正在扫描 Wi-Fi，请稍后重试";
        case MEMORY_PEAK_WIFI_TIME: return "正在准备网络对时，请稍后重试";
        case MEMORY_PEAK_WIFI_CONNECT: return "Wi-Fi 正在连接，请稍后重试";
        case MEMORY_PEAK_CELLULAR: return "4G 正在启动，请稍后重试";
        case MEMORY_PEAK_BLE: return "蓝牙正在启动，请稍后重试";
        case MEMORY_PEAK_BLE_CONNECT: return "蓝牙翻页器正在连接，请稍后重试";
        case MEMORY_PEAK_WEATHER:
        case MEMORY_PEAK_OTA_CHECK:
        case MEMORY_PEAK_CLOUDZAO: return "资源切换中，请稍后重试";
        default: break;
        }
    }
    switch (error) {
    case MEMORY_PEAK_BUSY: return "资源切换中，请稍后重试";
    case MEMORY_PEAK_CAPACITY: return "可用内存不足，请稍后重试";
    case MEMORY_PEAK_FRAGMENT: return "连续内存不足，请稍后重试";
    case MEMORY_PEAK_CONFLICT: return "翻页器正在使用，请先关闭蓝牙";
    default: return NULL;
    }
}
