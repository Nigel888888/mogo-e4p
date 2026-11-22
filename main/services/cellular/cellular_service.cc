/* SPDX-License-Identifier: Apache-2.0
 * Runtime NT26 capability probe and cellular bearer adapter.
 *
 * The Wi-Fi and 4G boards use one firmware image. The fixed UART pins are
 * only a transport contract; capability is granted after the modem answers
 * AT and remains separate from SIM and registration state.
 */

#include "cellular_service.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#include "board_hardware.h"
#include "ble_page_turner.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "settings.h"
#include "uart_eth_modem.h"

namespace {

constexpr char kTag[] = "cellular_service";
constexpr int64_t kProbeTimeoutUs = 15000000;
constexpr int64_t kRetryIntervalUs = 10000000;
constexpr gpio_num_t kTx = (gpio_num_t)BOARD_CELLULAR_UART_TX_GPIO;
constexpr gpio_num_t kRx = (gpio_num_t)BOARD_CELLULAR_UART_RX_GPIO;
constexpr gpio_num_t kMrdy = (gpio_num_t)BOARD_CELLULAR_MRDY_GPIO;
constexpr gpio_num_t kSrdy = (gpio_num_t)BOARD_CELLULAR_SRDY_GPIO;

struct Service {
    std::mutex mutex;
    std::recursive_mutex lifecycle;
    std::unique_ptr<UartEthModem> modem;
    cellular_service_status_t status = {};
    bool user_enabled = false;
    bool temporary = false;
    bool probe_active = false;
    bool stop_after_probe = false;
    bool paused = false;
    bool capability_persisted = false;
    int64_t probe_deadline_us = 0;
    int64_t retry_start_us = 0;
};

Service s_service;

void set_detail(cellular_service_status_t *status, const std::string &detail) {
    if (!status) return;
    std::snprintf(status->detail, sizeof(status->detail), "%s", detail.c_str());
}

void on_modem_event(UartEthModem::UartEthModemEvent event, const std::string &detail) {
    std::lock_guard<std::mutex> lock(s_service.mutex);
    if (!s_service.modem) return;
    const bool wanted = s_service.user_enabled || s_service.temporary;
    const bool module_replied = event != UartEthModem::UartEthModemEvent::ErrorInitFailed ||
        detail.find("Modem not detected") == std::string::npos;
    switch (event) {
        case UartEthModem::UartEthModemEvent::Connecting:
            s_service.status.capability_known = true;
            s_service.status.capable = true;
            s_service.status.state = CELLULAR_SERVICE_STARTING;
            s_service.status.ready = false;
            s_service.probe_active = false;
            s_service.stop_after_probe = !wanted;
            break;
        case UartEthModem::UartEthModemEvent::Connected:
            s_service.status.capability_known = true;
            s_service.status.capable = true;
            s_service.status.state = CELLULAR_SERVICE_READY;
            s_service.status.ready = true;
            s_service.probe_active = false;
            s_service.stop_after_probe = !wanted;
            break;
        case UartEthModem::UartEthModemEvent::ErrorNoSim:
        case UartEthModem::UartEthModemEvent::ErrorSimNotReady:
            s_service.status.capability_known = true;
            s_service.status.capable = true;
            s_service.status.state = event == UartEthModem::UartEthModemEvent::ErrorNoSim
                ? CELLULAR_SERVICE_NO_SIM : CELLULAR_SERVICE_SIM_NOT_READY;
            s_service.status.ready = false;
            s_service.probe_active = false;
            s_service.stop_after_probe = !wanted;
            break;
        case UartEthModem::UartEthModemEvent::ErrorRegistrationDenied:
            s_service.status.capability_known = true;
            s_service.status.capable = true;
            s_service.status.state = CELLULAR_SERVICE_NO_REGISTRATION;
            s_service.status.ready = false;
            s_service.probe_active = false;
            s_service.stop_after_probe = !wanted;
            break;
        case UartEthModem::UartEthModemEvent::ErrorInitFailed:
            s_service.status.startup_failed = true;
            if (s_service.temporary) {
                // 保留已确认的硬件能力，失败会话由租约释放，不做后台重试。
                // Preserve known hardware capability; the lease releases failures without background retries.
                s_service.status.state = CELLULAR_SERVICE_ERROR;
                s_service.status.ready = false;
                break;
            }
            if (detail == "移动网络检测内存不足") {
                s_service.status.state = CELLULAR_SERVICE_ERROR;
                s_service.status.ready = false;
                s_service.probe_active = false;
                s_service.stop_after_probe = true;
                s_service.retry_start_us = esp_timer_get_time() + kRetryIntervalUs;
                break;
            }
            s_service.status.capability_known = true;
            s_service.status.capable = module_replied;
            s_service.status.state = module_replied ? CELLULAR_SERVICE_ERROR : CELLULAR_SERVICE_STOPPED;
            s_service.status.ready = false;
            s_service.probe_active = false;
            s_service.stop_after_probe = true;
            break;
        case UartEthModem::UartEthModemEvent::Disconnected:
        default:
            s_service.status.state = (s_service.status.enabled || s_service.temporary)
                ? CELLULAR_SERVICE_STARTING : CELLULAR_SERVICE_STOPPED;
            s_service.status.ready = false;
            break;
    }
    set_detail(&s_service.status, detail);
}

esp_err_t ensure_netif_runtime(void) {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    return ESP_OK;
}

std::unique_ptr<UartEthModem> make_modem() {
    UartEthModem::Config config = {
        .uart_num = UART_NUM_2,
        .baud_rate = 2000000,
        .tx_pin = kTx,
        .rx_pin = kRx,
        .mrdy_pin = kMrdy,
        .srdy_pin = kSrdy,
    };
    auto* raw = new (std::nothrow) UartEthModem(config);
    if (!raw) return {};
    std::unique_ptr<UartEthModem> modem(raw);
    modem->SetNetworkEventCallback(on_modem_event);
    return modem;
}

struct StartupPeak {
    memory_peak_token_t token = 0;
    ~StartupPeak() { memory_peak_finish(token); }
};

struct StartupBlePause {
    bool held = false;
    ~StartupBlePause() { if (held) ble_pt_cellular_release(); }
};

esp_err_t start_modem(bool enabled, bool probing, memory_peak_token_t *admitted = nullptr, bool temporary = false) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(s_service.lifecycle);
    {
        std::lock_guard<std::mutex> lock(s_service.mutex);
        if (s_service.modem) return ESP_OK;
    }
    StartupPeak peak;
    esp_err_t startup = admitted ? ESP_OK : memory_peak_try_begin(MEMORY_PEAK_CELLULAR, &peak.token);
    if (startup != ESP_OK) return startup;
    // 与 BLE 起栈、连接和扫描共用生命周期门；只有空闲蓝牙让位。
    // Share the BLE lifecycle gate with startup, connection and scans; only idle BLE yields.
    StartupBlePause pause;
    startup = ble_pt_cellular_acquire();
    if (startup != ESP_OK) return startup;
    pause.held = true;
    startup = memory_peak_check(memory_peak_heap_t{11264, 4096, 0, 0});
    if (startup != ESP_OK) return startup;
    startup = ensure_netif_runtime();
    if (startup != ESP_OK) return startup;
    startup = board_ioe_wake();
    if (startup != ESP_OK) return startup;

    auto modem = make_modem();
    if (!modem) {
        std::lock_guard<std::mutex> lock(s_service.mutex);
        s_service.status.enabled = false;
        s_service.status.ready = false;
        s_service.status.state = CELLULAR_SERVICE_ERROR;
        s_service.probe_active = false;
        s_service.stop_after_probe = false;
        set_detail(&s_service.status, "移动网络驱动内存不足");
        return ESP_ERR_NO_MEM;
    }
    UartEthModem *raw_modem = modem.get();
    {
        std::lock_guard<std::mutex> lock(s_service.mutex);
        // A concurrent wake/lease may have won the start race after the first
        // check; let the temporary object clean itself up before returning.
        if (s_service.modem) return ESP_OK;
        s_service.modem = std::move(modem);
        s_service.temporary = temporary;
        s_service.status.enabled = enabled;
        s_service.status.ready = false;
        s_service.status.startup_failed = false;
        s_service.status.running = true;
        s_service.status.state = probing ? CELLULAR_SERVICE_PROBING : CELLULAR_SERVICE_STARTING;
        s_service.status.signal_strength = 99;
        s_service.status.detail[0] = 0;
        s_service.probe_active = probing;
        s_service.stop_after_probe = false;
        s_service.probe_deadline_us = esp_timer_get_time() + kProbeTimeoutUs;
    }

    const esp_err_t err = raw_modem->Start(admitted ? admitted : &peak.token);
    if (err == ESP_OK) {
        pause.held = false;
        return ESP_OK;
    }
    const bool stopped = raw_modem->Stop() == ESP_OK;
    std::lock_guard<std::mutex> lock(s_service.mutex);
    if (stopped) {
        s_service.modem.reset();
        s_service.temporary = false;
    } else pause.held = false;
    s_service.status.running = !stopped;
    s_service.status.enabled = false;
    s_service.status.ready = false;
    s_service.status.state = CELLULAR_SERVICE_ERROR;
    s_service.probe_active = false;
    s_service.stop_after_probe = !stopped;
    set_detail(&s_service.status, "移动网络驱动启动失败");
    return err;
}

bool stop_modem(bool preserve_status) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(s_service.lifecycle);
    std::unique_ptr<UartEthModem> modem;
    {
        std::lock_guard<std::mutex> lock(s_service.mutex);
        modem = std::move(s_service.modem);
        // `enabled` is the user's bearer preference, while `modem` is the
        // live transport. Sleep/probe handoff can stop the latter without
        // making a saved-on radio look permanently disabled.
        s_service.status.enabled = preserve_status ? s_service.user_enabled : false;
        s_service.status.ready = false;
        if (!preserve_status) {
            s_service.status.state = CELLULAR_SERVICE_STOPPED;
            s_service.status.startup_failed = false;
        }
        s_service.status.signal_strength = 99;
        s_service.probe_active = false;
        s_service.stop_after_probe = false;
        s_service.probe_deadline_us = 0;
    }
    const bool had_modem = modem != nullptr;
    const bool stopped = !modem || modem->Stop() == ESP_OK;
    if (!stopped) {
        std::lock_guard<std::mutex> lock(s_service.mutex);
        if (!s_service.modem) s_service.modem = std::move(modem);
        s_service.status.enabled = preserve_status ? s_service.user_enabled : false;
        s_service.status.ready = false;
    }
    if (stopped && had_modem) {
        modem.reset();
        {
            std::lock_guard<std::mutex> lock(s_service.mutex);
            s_service.status.running = false;
            s_service.temporary = false;
        }
        ble_pt_cellular_release();
    }
    return stopped;
}

void start_background(bool enabled, bool probing) {
    const esp_err_t result = start_modem(enabled, probing);
    if (result == ESP_OK) return;
    if (memory_peak_is_busy(result) || memory_peak_is_memory_error(result) || result == MEMORY_PEAK_CONFLICT) {
        std::lock_guard<std::mutex> lock(s_service.mutex);
        s_service.retry_start_us = esp_timer_get_time() + kRetryIntervalUs;
    }
}

}  // namespace

extern "C" void cellular_service_init(void) {
    const bool enabled = app_settings_cellular_enabled();
    const bool capability_known = app_settings_cellular_capability_known();
    const bool capable = app_settings_cellular_capable();
    {
        std::lock_guard<std::mutex> lock(s_service.mutex);
        s_service.status = (cellular_service_status_t){
            .capability_known = capability_known,
            .capable = capable,
            .enabled = enabled && (!capability_known || capable),
            .ready = false,
            .running = false,
            .startup_failed = false,
            .state = capability_known ? CELLULAR_SERVICE_STOPPED : CELLULAR_SERVICE_PROBING,
            .signal_strength = 99,
            .detail = "",
        };
        set_detail(&s_service.status, capability_known
            ? (capable ? "已检测到移动网络模块" : "未检测到移动网络模块")
            : "正在检测移动网络");
        s_service.user_enabled = enabled;
        s_service.paused = false;
        s_service.capability_persisted = capability_known;
        s_service.retry_start_us = 0;
    }
    // Probe an unknown board once. A known Wi-Fi board stays stopped, while a
    // known 4G board starts only when the user enabled its bearer.
    if (!capability_known || (enabled && capable)) start_background(enabled, !capability_known);
}

static esp_err_t set_enabled(bool enabled, memory_peak_token_t *admitted) {
    cellular_service_status_t status;
    std::lock_guard<std::recursive_mutex> lifecycle_lock(s_service.lifecycle);
    cellular_service_get_status(&status);
    if (enabled) {
        if (!status.capability_known) return ESP_ERR_INVALID_STATE;
        if (!status.capable) return ESP_ERR_NOT_SUPPORTED;
        bool already_running = false;
        bool previously_enabled;
        {
            std::lock_guard<std::mutex> lock(s_service.mutex);
            previously_enabled = s_service.user_enabled;
            s_service.user_enabled = true;
            s_service.stop_after_probe = false;
            if (s_service.modem) {
                s_service.temporary = false;
                s_service.status.enabled = true;
                if (s_service.status.state == CELLULAR_SERVICE_STOPPED)
                    s_service.status.state = CELLULAR_SERVICE_STARTING;
                already_running = true;
            }
        }
        if (already_running) {
            app_settings_set_cellular_enabled(true);
            return ESP_OK;
        }
        esp_err_t err = start_modem(true, false, admitted);
        if (err == ESP_OK) app_settings_set_cellular_enabled(true);
        else {
            std::lock_guard<std::mutex> lock(s_service.mutex);
            s_service.user_enabled = previously_enabled;
        }
        return err;
    }
    {
        std::lock_guard<std::mutex> lock(s_service.mutex);
        s_service.user_enabled = false;
        s_service.retry_start_us = 0;
    }
    const bool stopped = stop_modem(false);
    if (stopped) app_settings_set_cellular_enabled(false);
    return stopped ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t cellular_service_set_enabled(bool enabled) {
    return set_enabled(enabled, nullptr);
}

extern "C" esp_err_t cellular_service_start_temporary(void) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(s_service.lifecycle);
    {
        std::lock_guard<std::mutex> lock(s_service.mutex);
        if (s_service.paused || s_service.user_enabled || !s_service.status.capability_known)
            return ESP_ERR_INVALID_STATE;
        if (!s_service.status.capable) return ESP_ERR_NOT_SUPPORTED;
        if (s_service.modem) {
            const auto state = s_service.status.state;
            return s_service.temporary && (state == CELLULAR_SERVICE_STARTING || state == CELLULAR_SERVICE_READY)
                ? ESP_OK : ESP_ERR_INVALID_STATE;
        }
    }
    return start_modem(false, false, nullptr, true);
}

extern "C" bool cellular_service_stop_temporary(void) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(s_service.lifecycle);
    {
        std::lock_guard<std::mutex> lock(s_service.mutex);
        if (!s_service.temporary) return true;
    }
    return stop_modem(false);
}

extern "C" esp_err_t cellular_service_enable_admitted(memory_peak_token_t *admitted) {
    return set_enabled(true, admitted);
}

extern "C" bool cellular_service_pause_for_sleep(void) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(s_service.lifecycle);
    bool has_modem;
    {
        std::lock_guard<std::mutex> lock(s_service.mutex);
        if (s_service.paused) return true;
        s_service.paused = true;
        has_modem = s_service.modem != nullptr;
    }
    const bool stopped = !has_modem || stop_modem(true);
    if (!stopped) {
        std::lock_guard<std::mutex> lock(s_service.mutex);
        s_service.paused = false;
    }
    return stopped;
}

extern "C" void cellular_service_resume_after_sleep(void) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(s_service.lifecycle);
    std::lock_guard<std::mutex> lock(s_service.mutex);
    if (!s_service.paused) return;
    s_service.paused = false;
    if (!s_service.status.capability_known || (s_service.user_enabled && s_service.status.capable)) {
        // 苏醒只排定一次请求；实际启动通过网络服务控制门。/ Wake queues one request; the network gate owns the actual start.
        s_service.retry_start_us = esp_timer_get_time() + 1;
    }
}

extern "C" bool cellular_service_yield_idle(void) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(s_service.lifecycle);
    {
        std::lock_guard<std::mutex> lock(s_service.mutex);
        if (!s_service.modem) return true;
        const auto state = s_service.status.state;
        if (s_service.status.ready || (!s_service.status.startup_failed &&
            state != CELLULAR_SERVICE_NO_SIM && state != CELLULAR_SERVICE_SIM_NOT_READY &&
            state != CELLULAR_SERVICE_NO_REGISTRATION)) return false;
    }
    return stop_modem(true);
}

extern "C" void cellular_service_poll(bool allow_start) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(s_service.lifecycle);
    UartEthModem *modem = nullptr;
    bool stop_pending = false;
    bool probe_timed_out = false;
    bool record_capability = false;
    bool capability = false;
    bool retry = false, retry_enabled = false, retry_probe = false;
    {
        std::lock_guard<std::mutex> lock(s_service.mutex);
        modem = s_service.modem.get();
        stop_pending = s_service.stop_after_probe;
        probe_timed_out = s_service.probe_active && esp_timer_get_time() >= s_service.probe_deadline_us;
        const int64_t now = esp_timer_get_time();
        if (!s_service.paused && !modem && s_service.retry_start_us && now >= s_service.retry_start_us) {
            s_service.retry_start_us = allow_start ? 0 : now + kRetryIntervalUs;
            retry_probe = !s_service.status.capability_known;
            retry_enabled = s_service.user_enabled;
            retry = allow_start && (retry_probe || (retry_enabled && s_service.status.capable));
        }
        if (s_service.status.capability_known && !s_service.capability_persisted) {
            record_capability = true;
            capability = s_service.status.capable;
            s_service.capability_persisted = true;
        }
    }
    if (probe_timed_out) {
        ESP_LOGW(kTag, "runtime cellular probe timed out");
        (void)stop_modem(false);
        std::lock_guard<std::mutex> lock(s_service.mutex);
        s_service.status.capability_known = true;
        s_service.status.capable = false;
        s_service.status.state = CELLULAR_SERVICE_STOPPED;
        s_service.status.startup_failed = true;
        set_detail(&s_service.status, "未检测到移动网络模块");
    } else if (stop_pending) {
        (void)stop_modem(true);
    }
    if (record_capability) app_settings_set_cellular_capability(capability);
    if (retry) start_background(retry_enabled, retry_probe);
}

extern "C" void cellular_service_get_status(cellular_service_status_t *out) {
    if (!out) return;
    std::lock_guard<std::mutex> lock(s_service.mutex);
    *out = s_service.status;
    if (s_service.modem) out->signal_strength = s_service.modem->GetSignalStrength();
}

extern "C" bool cellular_service_capable(void) {
    std::lock_guard<std::mutex> lock(s_service.mutex);
    return s_service.status.capable;
}

extern "C" bool cellular_service_enabled(void) {
    std::lock_guard<std::mutex> lock(s_service.mutex);
    return s_service.status.enabled;
}

extern "C" bool cellular_service_ready(void) {
    std::lock_guard<std::mutex> lock(s_service.mutex);
    return s_service.status.ready;
}

extern "C" void *cellular_service_netif(void) {
    std::lock_guard<std::mutex> lock(s_service.mutex);
    return s_service.modem ? reinterpret_cast<void *>(s_service.modem->GetNetif()) : nullptr;
}
