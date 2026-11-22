/* SPDX-License-Identifier: Apache-2.0
 *
 * The service is the only owner of the external business bearer decision.
 * Manual Wi-Fi and cellular enable actions are mutually exclusive: enabling
 * one bearer stops the other when it is idle and selects that bearer for
 * business traffic. Wi-Fi transfer remains the owner of its AP/STA server;
 * this layer only borrows its network-only STA path for time, weather and OTA.
 */
#include "network_service.h"
#include "network_local_wifi.h"

#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "cellular_service.h"
#include "ble_page_turner.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "wifi_transfer.h"

static _Atomic network_route_t s_route = NETWORK_ROUTE_AUTO;
static _Atomic network_auto_priority_t s_priority = NETWORK_AUTO_WIFI_FIRST;
static atomic_uint s_cellular_time_pending;
static atomic_bool s_time_active;
static atomic_uint s_wifi_leases;
static atomic_uint s_cellular_leases;
static atomic_bool s_cellular_control;
static atomic_bool s_wifi_owned_by_business;
static bool s_cellular_owned_by_business; // 受承载控制门保护。/ Protected by the bearer gate.
static atomic_bool s_resume_pending;
static int64_t s_resume_retry_at;
static bool s_radios_paused;
static uint32_t s_local_wifi_generation;
static atomic_uint s_drop_wifi_session, s_drop_media_wifi_session;
// 仅显式切换记录回滚；受同一个承载控制门保护。/ Only explicit switches track rollback, under the bearer control gate.
static struct {
    network_bearer_t target;
    network_route_t previous_route;
    bool target_stopped;
    int64_t retry_at;
} s_switch;

static esp_err_t sync_time_cellular(uint32_t *utc_seconds, bool (*cancelled)(void)) {
    if (!utc_seconds || (cancelled && cancelled())) return ESP_ERR_INVALID_STATE;
    *utc_seconds = 0;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3,
        ESP_SNTP_SERVER_LIST("203.107.6.88", "162.159.200.123", "ntp.aliyun.com"));
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) return err;
    const int64_t deadline = esp_timer_get_time() + 20000000;
    do {
        if (cancelled && cancelled()) { err = ESP_ERR_INVALID_STATE; break; }
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(100));
    } while (err == ESP_ERR_TIMEOUT && esp_timer_get_time() < deadline);
    if (err == ESP_OK && cancelled && cancelled()) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        time_t now = time(NULL);
        if (now >= 1704067200 && now <= 4102444799U) {
            *utc_seconds = (uint32_t)now;
        } else {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    esp_netif_sntp_deinit();
    return err;
}

static esp_err_t sync_time_on_lease(network_lease_t *lease, uint32_t *utc_seconds, bool (*cancelled)(void)) {
    if (!lease || !lease->active || !utc_seconds) return ESP_ERR_INVALID_ARG;
    *utc_seconds = 0;
    if (cancelled && cancelled()) return ESP_ERR_INVALID_STATE;
    if (!network_service_wait_ready_cancelable(lease, 30000, cancelled))
        return cancelled && cancelled() ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
    if (lease->bearer == NETWORK_BEARER_WIFI) {
        if (lease->use == NETWORK_USE_TIME_MANUAL) return wifi_transfer_sync_time_online_manual(utc_seconds);
        return wifi_transfer_sync_time_online(utc_seconds);
    }
    if (lease->bearer == NETWORK_BEARER_CELLULAR)
        return sync_time_cellular(utc_seconds, cancelled);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t network_service_sync_time_on_lease(network_lease_t *lease, uint32_t *utc_seconds, bool (*cancelled)(void)) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_time_active, &expected, true)) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = sync_time_on_lease(lease, utc_seconds, cancelled);
    atomic_store(&s_time_active, false);
    return err;
}

static network_bearer_t resolve_ready_bearer(const network_status_t *status) {
    if (!status) return NETWORK_BEARER_NONE;
    if (status->route == NETWORK_ROUTE_WIFI)
        return status->wifi_ready ? NETWORK_BEARER_WIFI : NETWORK_BEARER_NONE;
    if (status->route == NETWORK_ROUTE_CELLULAR)
        return status->cellular_ready ? NETWORK_BEARER_CELLULAR : NETWORK_BEARER_NONE;
    if (status->auto_priority == NETWORK_AUTO_CELLULAR_FIRST) {
        if (status->cellular_ready) return NETWORK_BEARER_CELLULAR;
        if (status->wifi_ready) return NETWORK_BEARER_WIFI;
    } else {
        if (status->wifi_ready) return NETWORK_BEARER_WIFI;
        if (status->cellular_ready) return NETWORK_BEARER_CELLULAR;
    }
    return NETWORK_BEARER_NONE;
}

static bool saved_wifi(void) {
    char ssid[33] = {0};
    bool configured = false;
    return wifi_transfer_get_saved_wifi(ssid, &configured) == ESP_OK && configured;
}

static esp_err_t start_wifi_for_lease(network_lease_t *lease, memory_peak_token_t admitted) {
    wifi_transfer_status_t status;
    wifi_transfer_get_status(&status);
    if (status.mode == WIFI_TRANSFER_MODE_STA &&
        (status.network_ready || status.state == WIFI_TRANSFER_STARTING)) {
        lease->owns_wifi = false;
        return ESP_OK;
    }
    const bool retry = status.owns_radio && status.state == WIFI_TRANSFER_ERROR &&
        status.network_only && status.mode == WIFI_TRANSFER_MODE_STA;
    if (status.owns_radio && !retry) return ESP_ERR_INVALID_STATE;
    if (!saved_wifi()) return ESP_ERR_NOT_FOUND;
    // 准入与无线切换共用控制门；仅重建没有其他租约的纯联网失败会话。
    // Admission shares the radio-control gate; rebuild failed network-only sessions without other leases.
    if (retry && (atomic_load(&s_wifi_leases) || !wifi_transfer_try_stop_if_idle()))
        return ESP_ERR_INVALID_STATE;
    const wifi_transfer_cfg_t cfg = {.mode = WIFI_TRANSFER_MODE_STA, .network_only = true};
    esp_err_t err = wifi_transfer_start_admitted(&cfg, admitted);
    if (err == ESP_OK) lease->owns_wifi = true;
    return err;
}

static bool cellular_can_start_for_lease(const network_status_t *status) {
    if (!status->cellular_capable || !status->cellular_enabled) return false;
    // SIM/注册终止状态可以回退，但不能把原会话当成可联网。/ Terminal SIM/registration states allow fallback, not data use.
    return status->cellular_state != CELLULAR_SERVICE_NO_SIM &&
        status->cellular_state != CELLULAR_SERVICE_SIM_NOT_READY &&
        status->cellular_state != CELLULAR_SERVICE_NO_REGISTRATION &&
        status->cellular_state != CELLULAR_SERVICE_ERROR;
}

static bool select_bearer_netif(network_bearer_t bearer) {
    void *netif = bearer == NETWORK_BEARER_CELLULAR ? cellular_service_netif()
        : bearer == NETWORK_BEARER_WIFI ? wifi_transfer_netif() : NULL;
    return netif && esp_netif_set_default_netif((esp_netif_t *)netif) == ESP_OK;
}

static esp_err_t acquire_bearer(network_bearer_t bearer, network_use_t use, network_lease_t *lease) {
    if (!lease) return ESP_ERR_INVALID_ARG;
    *lease = (network_lease_t){0};
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_cellular_control, &idle, true))
        return MEMORY_PEAK_BUSY;
    if (s_radios_paused) {
        atomic_store(&s_cellular_control, false);
        return MEMORY_PEAK_BUSY;
    }
    network_status_t status;
    network_service_get_status(&status);
    esp_err_t error = ESP_ERR_NOT_FOUND;
    if (use == NETWORK_USE_WEATHER_BACKGROUND) {
        // 静默刷新只借用当前路由的就绪承载，不启动或切换无线。/ Quiet refresh only borrows the selected ready bearer; never start or switch radios.
        wifi_transfer_status_t wifi = {0};
        wifi_transfer_get_status(&wifi);
        bearer = status.active_bearer;
        if (s_switch.target != NETWORK_BEARER_NONE)
            error = MEMORY_PEAK_BUSY;
        else if (bearer == NETWORK_BEARER_WIFI && wifi.network_only && !atomic_load(&s_cellular_leases))
            error = ESP_OK;
        else if (bearer == NETWORK_BEARER_CELLULAR && !wifi.owns_radio && !atomic_load(&s_wifi_leases))
            error = ESP_OK;
    } else if (bearer == NETWORK_BEARER_WIFI) {
        cellular_service_status_t cellular;
        cellular_service_get_status(&cellular);
        memory_peak_token_t peak = 0;
        if (atomic_load(&s_cellular_leases)) {
            error = ESP_ERR_INVALID_STATE;
        } else if (!cellular.running) {
            error = status.wifi_ready ? ESP_OK : start_wifi_for_lease(lease, 0);
        } else if (status.route == NETWORK_ROUTE_AUTO && !status.wifi_enabled && !cellular.ready &&
                   (cellular.state == CELLULAR_SERVICE_NO_SIM || cellular.state == CELLULAR_SERVICE_SIM_NOT_READY ||
                    cellular.state == CELLULAR_SERVICE_NO_REGISTRATION || cellular.startup_failed) && saved_wifi()) {
            // AUTO 可从无数据链路的 4G 回退，但不抢占正在探测或联网的 4G。
            // AUTO can fall back from terminal 4G without taking over an active probe or link.
            error = ble_pt_network_check();
            if (error == ESP_OK) error = memory_peak_try_begin(MEMORY_PEAK_WIFI, &peak);
            if (error == ESP_OK) error = cellular_service_yield_idle()
                ? start_wifi_for_lease(lease, peak) : ESP_ERR_INVALID_STATE;
            memory_peak_finish(peak);
        } else {
            error = ESP_ERR_INVALID_STATE;
        }
    } else if (bearer == NETWORK_BEARER_CELLULAR) {
        wifi_transfer_status_t wifi;
        wifi_transfer_get_status(&wifi);
        const bool temporary = network_use_allows_temporary_cellular(use) &&
            status.cellular_capability_known && status.cellular_capable && !status.cellular_enabled;
        if (atomic_load(&s_wifi_leases)) {
            error = ESP_ERR_INVALID_STATE;
        } else if (status.cellular_ready || cellular_can_start_for_lease(&status) || temporary) {
            // AUTO 只清理已失败的纯联网 STA，不拆热点、传书或仍在关联的 Wi-Fi。
            // AUTO cleans up a failed network-only STA, not AP, transfers or an active association.
            const bool can_yield = status.route == NETWORK_ROUTE_AUTO && wifi.network_only &&
                !wifi.network_ready && wifi.state == WIFI_TRANSFER_ERROR;
            error = status.cellular_ready ? ESP_OK : ble_pt_network_check();
            if (error == ESP_OK) {
                if (wifi.owns_radio && (!can_yield || !wifi_transfer_try_stop_if_idle())) {
                    error = ESP_ERR_INVALID_STATE;
                } else if (!status.cellular_ready && temporary) {
                    error = cellular_service_start_temporary();
                    if (error == ESP_OK) {
                        lease->owns_cellular = !s_cellular_owned_by_business;
                        s_cellular_owned_by_business = true;
                    }
                } else if (!status.cellular_ready) {
                    error = cellular_service_set_enabled(true);
                }
            }
        }
    }
    if (error == ESP_OK) {
        lease->bearer = bearer;
        lease->use = use;
        lease->active = true;
    }
    if (lease->active && lease->bearer == NETWORK_BEARER_WIFI) {
        if (lease->owns_wifi) atomic_store(&s_wifi_owned_by_business, true);
        atomic_fetch_add(&s_wifi_leases, 1);
    }
    else if (lease->active) atomic_fetch_add(&s_cellular_leases, 1);
    if ((lease->bearer == NETWORK_BEARER_WIFI && status.wifi_ready) ||
        (lease->bearer == NETWORK_BEARER_CELLULAR && status.cellular_ready))
        (void)select_bearer_netif(lease->bearer);
    atomic_store(&s_cellular_control, false);
    return error;
}

void network_service_init(void) {
    s_route = (network_route_t)app_settings_network_route();
    s_priority = (network_auto_priority_t)app_settings_network_auto_priority();
    if (!network_route_valid(s_route)) s_route = NETWORK_ROUTE_AUTO;
    if (!network_auto_priority_valid(s_priority)) s_priority = NETWORK_AUTO_WIFI_FIRST;
    atomic_store(&s_cellular_time_pending, 0);
    atomic_store(&s_time_active, false);
    atomic_store(&s_wifi_leases, 0);
    atomic_store(&s_cellular_leases, 0);
    atomic_store(&s_cellular_control, false);
    atomic_store(&s_wifi_owned_by_business, false);
    s_cellular_owned_by_business = false;
    s_switch.target = NETWORK_BEARER_NONE;
    atomic_store(&s_resume_pending, false);
    atomic_store(&s_drop_wifi_session, 0);
    atomic_store(&s_drop_media_wifi_session, 0);
    s_radios_paused = false;
    s_resume_retry_at = 0;
}

static void stop_unused_business_wifi(void) {
    if (!atomic_load(&s_wifi_owned_by_business) || atomic_load(&s_wifi_leases)) return;
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_cellular_control, &idle, true)) return;
    if (!atomic_load(&s_wifi_leases) && atomic_load(&s_wifi_owned_by_business) && wifi_transfer_try_stop_if_idle())
        atomic_store(&s_wifi_owned_by_business, false);
    atomic_store(&s_cellular_control, false);
}

static void stop_unused_business_cellular_under_gate(void) {
    if (s_cellular_owned_by_business && !atomic_load(&s_cellular_leases) && cellular_service_stop_temporary())
        s_cellular_owned_by_business = false;
}

// 调用者持有承载控制门。先清理失败目标，再申请恢复用的高峰名额。
// Caller holds the bearer gate. Clean up the failed target before admitting restoration.
static void poll_switch_rollback(void) {
    const network_bearer_t target = s_switch.target;
    if (target == NETWORK_BEARER_NONE || esp_timer_get_time() < s_switch.retry_at) return;
    if (!s_switch.target_stopped) {
        cellular_service_status_t cellular;
        wifi_transfer_status_t wifi;
        cellular_service_get_status(&cellular);
        wifi_transfer_get_status(&wifi);
        if ((target == NETWORK_BEARER_WIFI && wifi.network_ready) ||
            (target == NETWORK_BEARER_CELLULAR && (cellular.ready ||
                cellular.state == CELLULAR_SERVICE_NO_SIM || cellular.state == CELLULAR_SERVICE_SIM_NOT_READY ||
                cellular.state == CELLULAR_SERVICE_NO_REGISTRATION))) {
            s_switch.target = NETWORK_BEARER_NONE;
            return;
        }
        if (target == NETWORK_BEARER_WIFI ? wifi.state != WIFI_TRANSFER_ERROR : !cellular.startup_failed) return;
        if (atomic_load(&s_wifi_leases) || atomic_load(&s_cellular_leases)) return;
        s_switch.target_stopped = target == NETWORK_BEARER_WIFI ? wifi_transfer_try_stop_if_idle()
            : cellular_service_set_enabled(false) == ESP_OK;
        if (!s_switch.target_stopped) {
            s_switch.retry_at = esp_timer_get_time() + 1000000;
            return;
        }
        s_route = s_switch.previous_route;
        app_settings_set_network_route(s_route);
    }
    // 切换间隙允许重试，不得拆除这期间已被其他业务使用的承载。
    // Retried restoration never tears down a bearer borrowed in the meantime.
    if (atomic_load(&s_wifi_leases) || atomic_load(&s_cellular_leases)) return;
    if (target == NETWORK_BEARER_WIFI) {
        wifi_transfer_status_t wifi;
        wifi_transfer_get_status(&wifi);
        if (wifi.owns_radio) return;
    } else {
        cellular_service_status_t cellular;
        cellular_service_get_status(&cellular);
        if (cellular.running) return;
    }
    memory_peak_token_t peak = 0;
    esp_err_t result = memory_peak_try_begin(target == NETWORK_BEARER_WIFI ? MEMORY_PEAK_CELLULAR : MEMORY_PEAK_WIFI, &peak);
    if (result == ESP_OK) {
        if (target == NETWORK_BEARER_WIFI) {
            result = cellular_service_enable_admitted(&peak);
        } else {
            result = ble_pt_network_check();
            if (result == ESP_OK) result = wifi_transfer_restore_admitted(peak);
        }
        memory_peak_finish(peak);
    }
    if (memory_peak_is_busy(result) || memory_peak_is_memory_error(result) || result == MEMORY_PEAK_CONFLICT) {
        s_switch.retry_at = esp_timer_get_time() + 1000000;
    } else {
        s_switch.target = NETWORK_BEARER_NONE;
    }
}

static void resume_under_gate(void) {
    if (!atomic_load(&s_resume_pending) || esp_timer_get_time() < s_resume_retry_at) return;
    if (memory_peak_is_busy(wifi_transfer_resume_after_sleep())) {
        s_resume_retry_at = esp_timer_get_time() + 1000000;
        return;
    }
    atomic_store(&s_resume_pending, false);
    s_resume_retry_at = 0;
    cellular_service_resume_after_sleep();
    s_radios_paused = false;
}

void network_service_resume_after_sleep(void) {
    atomic_store(&s_resume_pending, true);
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_cellular_control, &idle, true)) return;
    resume_under_gate();
    atomic_store(&s_cellular_control, false);
}

bool network_service_pause_for_sleep(void) {
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_cellular_control, &idle, true)) return false;
    bool paused = s_radios_paused;
    if (!paused && !atomic_load(&s_wifi_leases) && !atomic_load(&s_cellular_leases) && wifi_transfer_pause_for_sleep()) {
        paused = cellular_service_pause_for_sleep();
        if (!paused) {
            s_radios_paused = true;
            atomic_store(&s_resume_pending, true);
            resume_under_gate();
        }
    }
    if (paused) s_radios_paused = true;
    atomic_store(&s_cellular_control, false);
    return paused;
}

static bool stop_owned_wifi_under_gate(uint32_t session_id, bool media_lost);

void network_service_poll(void) {
    stop_unused_business_wifi();
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_cellular_control, &idle, true)) return;
    stop_unused_business_cellular_under_gate();
    resume_under_gate();
    if (s_radios_paused) {
        atomic_store(&s_cellular_control, false);
        return;
    }
    uint32_t lost = atomic_load(&s_drop_media_wifi_session);
    if (lost && stop_owned_wifi_under_gate(lost, true))
        (void)atomic_compare_exchange_strong(&s_drop_media_wifi_session, &lost, 0);
    uint32_t dropped = atomic_load(&s_drop_wifi_session);
    if (dropped && stop_owned_wifi_under_gate(dropped, false))
        (void)atomic_compare_exchange_strong(&s_drop_wifi_session, &dropped, 0);
    if (!atomic_load(&s_wifi_leases)) wifi_transfer_recover_stuck_sta();
    wifi_transfer_status_t wifi;
    wifi_transfer_get_status(&wifi);
    // 生命周期锁只在承载控制门之内获取；蜂窝回调不反向获取控制门。
    // Lifecycle is acquired inside the bearer gate; cellular callbacks never acquire the gate.
    cellular_service_poll(!wifi.owns_radio && !atomic_load(&s_wifi_leases) &&
                          s_switch.target == NETWORK_BEARER_NONE);
    poll_switch_rollback();
    network_status_t status;
    network_service_get_status(&status);
    network_bearer_t bearer = status.active_bearer;
    if (atomic_load(&s_cellular_leases))
        bearer = status.cellular_ready ? NETWORK_BEARER_CELLULAR : NETWORK_BEARER_NONE;
    else if (atomic_load(&s_wifi_leases))
        bearer = status.wifi_ready ? NETWORK_BEARER_WIFI : NETWORK_BEARER_NONE;
    (void)select_bearer_netif(bearer);
    atomic_store(&s_cellular_control, false);
}

void network_service_get_status(network_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    wifi_transfer_status_t wifi = {0};
    cellular_service_status_t cellular = {0};
    wifi_transfer_get_status(&wifi);
    cellular_service_get_status(&cellular);
    out->cellular_capability_known = cellular.capability_known;
    out->cellular_capable = cellular.capable;
    out->cellular_enabled = cellular.enabled;
    out->cellular_ready = cellular.ready;
    out->cellular_signal = cellular.signal_strength;
    out->cellular_state = (uint8_t)cellular.state;
    memcpy(out->cellular_detail, cellular.detail, sizeof(out->cellular_detail));
    out->cellular_detail[sizeof(out->cellular_detail) - 1] = '\0';
    out->wifi_enabled = wifi.state != WIFI_TRANSFER_STOPPED;
    out->wifi_ready = wifi.mode == WIFI_TRANSFER_MODE_STA && wifi.network_ready;
    out->route = s_route;
    out->auto_priority = s_priority;
    out->active_bearer = resolve_ready_bearer(out);
}

static void remember_rollback(network_bearer_t target) {
    s_switch.previous_route = s_route;
    s_switch.target = target;
    s_switch.target_stopped = false;
    s_switch.retry_at = 0;
}

static void restore_cellular_after_wifi_error(memory_peak_token_t *peak) {
    wifi_transfer_status_t wifi;
    wifi_transfer_get_status(&wifi);
    // 同步启动失败也可能尚未拆完 Wi-Fi；确认停止前不能恢复 4G。
    // A synchronous start error can leave Wi-Fi in teardown; do not restore 4G until it stops.
    if (wifi.owns_radio) {
        remember_rollback(NETWORK_BEARER_WIFI);
    } else {
        const esp_err_t restored = cellular_service_enable_admitted(peak);
        if (restored != ESP_OK) ESP_LOGW("network", "cellular rollback failed: 0x%x", (unsigned)restored);
    }
}

esp_err_t network_service_set_enabled(network_bearer_t bearer, bool enabled) {
    if (bearer != NETWORK_BEARER_WIFI && bearer != NETWORK_BEARER_CELLULAR)
        return ESP_ERR_INVALID_ARG;
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_cellular_control, &idle, true))
        return MEMORY_PEAK_BUSY;

    if (s_radios_paused) {
        atomic_store(&s_cellular_control, false);
        return MEMORY_PEAK_BUSY;
    }
    esp_err_t err = ESP_OK;
    if (!enabled) {
        if (bearer == NETWORK_BEARER_CELLULAR) {
            err = atomic_load(&s_cellular_leases) != 0
                ? ESP_ERR_INVALID_STATE : cellular_service_set_enabled(false);
        } else {
            err = atomic_load(&s_wifi_leases) != 0
                ? ESP_ERR_INVALID_STATE
                : (wifi_transfer_try_stop_if_idle() ? ESP_OK : ESP_ERR_INVALID_STATE);
        }
        if (err == ESP_OK) s_switch.target = NETWORK_BEARER_NONE;
        if (err == ESP_OK && s_switch.target == bearer) s_switch.target = NETWORK_BEARER_NONE;
        atomic_store(&s_cellular_control, false);
        return err;
    }

    network_bearer_t switched_to = NETWORK_BEARER_NONE;
    memory_peak_token_t peak = 0;
    if (bearer == NETWORK_BEARER_CELLULAR) {
        wifi_transfer_status_t wifi = {0};
        wifi_transfer_get_status(&wifi);
        const bool wifi_enabled = wifi.owns_radio;
        bool wifi_stopped = false;
        if (atomic_load(&s_wifi_leases)) err = ESP_ERR_INVALID_STATE;
        cellular_service_status_t cellular = {0};
        cellular_service_get_status(&cellular);
        if (err == ESP_OK && !cellular.capability_known) err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK && !cellular.capable) err = ESP_ERR_NOT_SUPPORTED;
        if (err == ESP_OK && !cellular.running) err = ble_pt_network_check();
        if (err == ESP_OK && wifi_enabled) err = memory_peak_try_begin(MEMORY_PEAK_CELLULAR, &peak);
        if (err == ESP_OK && wifi_enabled) {
            wifi_stopped = wifi_transfer_try_stop_if_idle();
            if (!wifi_stopped) err = ESP_ERR_INVALID_STATE;
        }
        if (err == ESP_OK) err = peak ? cellular_service_enable_admitted(&peak) : cellular_service_set_enabled(true);
        if (err == ESP_OK && wifi_stopped) switched_to = bearer;
        if (err != ESP_OK && wifi_stopped) {
            const esp_err_t restored = wifi_transfer_restore_admitted(peak);
            if (restored != ESP_OK) ESP_LOGW("network", "Wi-Fi rollback failed: 0x%x", (unsigned)restored);
        }
    } else {
        cellular_service_status_t cellular = {0};
        cellular_service_get_status(&cellular);
        bool cellular_stopped = false;
        if (atomic_load(&s_cellular_leases)) err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK && !saved_wifi()) err = ESP_ERR_NOT_FOUND;
        if (err == ESP_OK && (cellular.running || cellular.enabled)) err = memory_peak_try_begin(MEMORY_PEAK_WIFI, &peak);
        if (err == ESP_OK && peak) err = ble_pt_network_check();
        if (err == ESP_OK && peak) {
            err = cellular_service_set_enabled(false);
            cellular_stopped = err == ESP_OK && cellular.enabled;
        }
        if (err == ESP_OK) {
            network_lease_t connection = {0};
            err = start_wifi_for_lease(&connection, peak);
        }
        if (err == ESP_OK && cellular_stopped) switched_to = bearer;
        if (err != ESP_OK && cellular_stopped) restore_cellular_after_wifi_error(&peak);
    }
    if (err == ESP_OK) {
        remember_rollback(switched_to);
        atomic_store(&s_wifi_owned_by_business, false);
        s_route = bearer == NETWORK_BEARER_WIFI ? NETWORK_ROUTE_WIFI : NETWORK_ROUTE_CELLULAR;
        app_settings_set_network_route(s_route);
    }
    memory_peak_finish(peak);
    atomic_store(&s_cellular_control, false);
    return err;
}

static esp_err_t local_wifi(const wifi_transfer_cfg_t *cfg, wifi_transfer_network_t *out, size_t *count,
                            uint32_t *session_id) {
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_cellular_control, &idle, true)) return MEMORY_PEAK_BUSY;
    memory_peak_token_t peak = 0;
    esp_err_t err = s_radios_paused ? MEMORY_PEAK_BUSY : ESP_ERR_INVALID_STATE;
    if (!s_radios_paused && !atomic_load(&s_cellular_leases) && !atomic_load(&s_wifi_leases)) {
        err = memory_peak_try_begin(cfg ? MEMORY_PEAK_WIFI : MEMORY_PEAK_WIFI_SCAN, &peak);
        if (err == ESP_OK) err = ble_pt_network_check();
        wifi_transfer_status_t wifi = {0};
        wifi_transfer_get_status(&wifi);
        cellular_service_status_t cellular;
        cellular_service_get_status(&cellular);
        bool cellular_stopped = false, wifi_stopped = false;
        if (err == ESP_OK && wifi.owns_radio) {
            wifi_stopped = wifi_transfer_try_stop_if_idle();
            if (!wifi_stopped) err = ESP_ERR_INVALID_STATE;
        }
        if (err == ESP_OK && (cellular.running || cellular.enabled)) {
            err = cellular_service_set_enabled(false);
            cellular_stopped = err == ESP_OK && cellular.enabled;
        }
        if (err == ESP_OK && cfg) {
            wifi_transfer_cfg_t owned = *cfg;
            if (++s_local_wifi_generation == 0) ++s_local_wifi_generation;
            owned.session_id = s_local_wifi_generation;
            err = wifi_transfer_start_admitted(&owned, peak);
            wifi_transfer_get_status(&wifi);
            if (session_id && wifi.owns_radio && wifi.session_id == owned.session_id) *session_id = owned.session_id;
        } else if (err == ESP_OK) {
            err = wifi_transfer_scan_admitted(out, count, peak);
        }
        if (cellular_stopped && (!cfg || err != ESP_OK)) {
            restore_cellular_after_wifi_error(&peak);
        } else if (wifi_stopped && (!cfg || err != ESP_OK)) {
            const esp_err_t restored = wifi_transfer_restore_admitted(peak);
            if (restored != ESP_OK) ESP_LOGW("network", "Wi-Fi rollback after local Wi-Fi: 0x%x", (unsigned)restored);
        }
        if (err == ESP_OK && cfg) {
            remember_rollback(cellular_stopped ? NETWORK_BEARER_WIFI : NETWORK_BEARER_NONE);
            atomic_store(&s_wifi_owned_by_business, false);
        }
    }
    memory_peak_finish(peak);
    atomic_store(&s_cellular_control, false);
    return err;
}

bool network_service_stop_local_wifi(void) {
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_cellular_control, &idle, true)) return false;
    const bool stopped = !atomic_load(&s_wifi_leases) && wifi_transfer_try_stop_if_idle();
    if (stopped && s_switch.target == NETWORK_BEARER_WIFI) s_switch.target = NETWORK_BEARER_NONE;
    atomic_store(&s_cellular_control, false);
    return stopped;
}

static bool stop_owned_wifi_under_gate(uint32_t session_id, bool media_lost) {
    wifi_transfer_status_t status;
    wifi_transfer_get_status(&status);
    if (!session_id || status.session_id != session_id || !status.owns_radio) return true;
    bool stopped;
    if (atomic_load(&s_wifi_leases)) {
        stopped = status.network_only || wifi_transfer_detach_http(media_lost);
        if (stopped) atomic_store(&s_wifi_owned_by_business, true);
    } else if (media_lost) {
        // 无租约却仍在使用已拔出的卡：等 HTTP 退场后才返回。/ No lease but media is gone: join HTTP before returning.
        wifi_transfer_stop();
        wifi_transfer_get_status(&status);
        stopped = status.state == WIFI_TRANSFER_STOPPED;
    } else {
        stopped = wifi_transfer_try_stop_if_idle();
    }
    if (stopped && s_switch.target == NETWORK_BEARER_WIFI) s_switch.target = NETWORK_BEARER_NONE;
    return stopped;
}

bool network_service_stop_owned_wifi(uint32_t session_id, bool media_lost) {
    if (!session_id) return true;
    if (media_lost) wifi_transfer_block_local_files(session_id);
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_cellular_control, &idle, true)) return false;
    const bool stopped = stop_owned_wifi_under_gate(session_id, media_lost);
    atomic_store(&s_cellular_control, false);
    return stopped;
}

void network_service_drop_owned_wifi(uint32_t session_id, bool media_lost) {
    if (session_id) atomic_store(media_lost ? &s_drop_media_wifi_session : &s_drop_wifi_session, session_id);
}

esp_err_t network_service_start_local_wifi(const wifi_transfer_cfg_t *cfg, uint32_t *session_id) {
    if (session_id) *session_id = 0;
    return cfg ? local_wifi(cfg, NULL, NULL, session_id) : ESP_ERR_INVALID_ARG;
}

esp_err_t network_service_scan_wifi(wifi_transfer_network_t out[WIFI_TRANSFER_SCAN_MAX], size_t *count) {
    return out && count ? local_wifi(NULL, out, count, NULL) : ESP_ERR_INVALID_ARG;
}

esp_err_t network_service_set_route(network_route_t route, network_auto_priority_t priority) {
    if (!network_route_valid(route) || !network_auto_priority_valid(priority)) return ESP_ERR_INVALID_ARG;
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_cellular_control, &idle, true)) return ESP_ERR_INVALID_STATE;
    cellular_service_status_t cellular = {0};
    cellular_service_get_status(&cellular);
    if (route == NETWORK_ROUTE_CELLULAR && cellular.capability_known && !cellular.capable) {
        atomic_store(&s_cellular_control, false);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_switch.target = NETWORK_BEARER_NONE;
    s_route = route;
    s_priority = priority;
    app_settings_set_network_route(route);
    app_settings_set_network_auto_priority(priority);
    atomic_store(&s_cellular_control, false);
    return ESP_OK;
}

network_route_t network_service_route(void) { return s_route; }
network_auto_priority_t network_service_auto_priority(void) { return s_priority; }

esp_err_t network_service_acquire(network_use_t use, network_lease_t *lease) {
    if (!lease) return ESP_ERR_INVALID_ARG;
    *lease = (network_lease_t){0};
    if (use == NETWORK_USE_WEATHER_BACKGROUND) return acquire_bearer(NETWORK_BEARER_NONE, use, lease);
    network_status_t status;
    network_service_get_status(&status);
    network_bearer_t first = resolve_ready_bearer(&status);
    if (first == NETWORK_BEARER_NONE) {
        first = status.route == NETWORK_ROUTE_CELLULAR ||
            (status.route == NETWORK_ROUTE_AUTO && status.auto_priority == NETWORK_AUTO_CELLULAR_FIRST)
            ? NETWORK_BEARER_CELLULAR : NETWORK_BEARER_WIFI;
    }
    esp_err_t err = acquire_bearer(first, use, lease);
    if (err == ESP_OK || status.route != NETWORK_ROUTE_AUTO) return err;
    esp_err_t fallback_error = acquire_bearer(first == NETWORK_BEARER_WIFI
        ? NETWORK_BEARER_CELLULAR : NETWORK_BEARER_WIFI, use, lease);
    // 回退承载不存在时保留首选承载的真实启动错误。/ Keep the first startup error if the fallback is absent.
    return fallback_error == ESP_ERR_NOT_FOUND ? err : fallback_error;
}

esp_err_t network_service_borrow_ready_wifi(network_lease_t *lease) {
    if (!lease) return ESP_ERR_INVALID_ARG;
    *lease = (network_lease_t){0};
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_cellular_control, &idle, true)) return MEMORY_PEAK_BUSY;
    wifi_transfer_status_t wifi = {0};
    wifi_transfer_get_status(&wifi);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (!s_radios_paused && !atomic_load(&s_cellular_leases) && wifi.network_ready &&
        wifi.network_only && wifi.mode == WIFI_TRANSFER_MODE_STA && select_bearer_netif(NETWORK_BEARER_WIFI)) {
        *lease = (network_lease_t){.bearer = NETWORK_BEARER_WIFI, .use = NETWORK_USE_CLOUDZAO, .active = true};
        atomic_fetch_add(&s_wifi_leases, 1);
        error = ESP_OK;
    }
    atomic_store(&s_cellular_control, false);
    return error;
}

bool network_service_wait_ready(network_lease_t *lease, uint32_t timeout_ms) {
    return network_service_wait_ready_cancelable(lease, timeout_ms, NULL);
}

bool network_service_wait_ready_cancelable(network_lease_t *lease, uint32_t timeout_ms,
                                           bool (*cancelled)(void)) {
    if (!lease || !lease->active) return false;
    if (lease->use == NETWORK_USE_WEATHER_BACKGROUND) {
        network_status_t status;
        network_service_get_status(&status);
        return (!cancelled || !cancelled()) && status.active_bearer == lease->bearer &&
            select_bearer_netif(lease->bearer);
    }
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (unsigned attempt = 0; attempt < 2 && lease->active; ++attempt) {
        bool failed = false;
        while (esp_timer_get_time() < deadline) {
            if (cancelled && cancelled()) return false;
            network_service_poll();
            if (cancelled && cancelled()) return false;
            network_status_t status;
            network_service_get_status(&status);
            if ((lease->bearer == NETWORK_BEARER_WIFI && status.wifi_ready) ||
                (lease->bearer == NETWORK_BEARER_CELLULAR && status.cellular_ready)) {
                // Readiness can change between poll() and the snapshot above.
                // Establish the lease's route before any DNS/TCP/TLS operation.
                if (select_bearer_netif(lease->bearer)) return true;
            }
            if (lease->bearer == NETWORK_BEARER_CELLULAR &&
                (status.cellular_state == CELLULAR_SERVICE_NO_SIM ||
                 status.cellular_state == CELLULAR_SERVICE_SIM_NOT_READY ||
                 status.cellular_state == CELLULAR_SERVICE_NO_REGISTRATION ||
                 status.cellular_state == CELLULAR_SERVICE_ERROR ||
                 (status.cellular_state == CELLULAR_SERVICE_STOPPED &&
                  status.cellular_capability_known))) {
                failed = true;
                break;
            }
            wifi_transfer_status_t wifi = {0};
            wifi_transfer_get_status(&wifi);
            if (lease->bearer == NETWORK_BEARER_WIFI && wifi.state == WIFI_TRANSFER_ERROR) {
                failed = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!failed && esp_timer_get_time() >= deadline) break;
        // AUTO may use the other bearer after a terminal registration or
        // connection error. Forced routes intentionally fail closed.
        if (s_route != NETWORK_ROUTE_AUTO || attempt != 0 || (cancelled && cancelled())) break;
        const network_bearer_t fallback = lease->bearer == NETWORK_BEARER_WIFI
            ? NETWORK_BEARER_CELLULAR : NETWORK_BEARER_WIFI;
        const network_use_t use = lease->use;
        network_service_release(lease);
        if (cancelled && cancelled()) break;
        network_lease_t replacement;
        if (acquire_bearer(fallback, use, &replacement) != ESP_OK) break;
        *lease = replacement;
    }
    return false;
}

void network_service_release(network_lease_t *lease) {
    if (!lease || !lease->active) return;
    const network_bearer_t bearer = lease->bearer;
    *lease = (network_lease_t){0};
    if (bearer == NETWORK_BEARER_WIFI) {
        atomic_fetch_sub(&s_wifi_leases, 1);
        stop_unused_business_wifi();
    } else if (bearer == NETWORK_BEARER_CELLULAR) {
        // 优先在业务线程持门归还并回收，避免 UI 轮询先接手驱动停止。
        // Return and clean up under the gate in the worker when possible, rather than on a UI poll.
        bool idle = false;
        const bool owns_gate = atomic_compare_exchange_strong(&s_cellular_control, &idle, true);
        const bool last = atomic_fetch_sub(&s_cellular_leases, 1) == 1;
        if (owns_gate) {
            if (last) stop_unused_business_cellular_under_gate();
            atomic_store(&s_cellular_control, false);
        }
    }
}

esp_err_t network_service_sync_time(uint32_t *utc_seconds, bool automatic, bool (*cancelled)(void)) {
    if (!utc_seconds) return ESP_ERR_INVALID_ARG;
    *utc_seconds = 0;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_time_active, &expected, true)) return ESP_ERR_INVALID_STATE;
    network_lease_t lease = {0};
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!cancelled || !cancelled()) {
        err = network_service_acquire(automatic ? NETWORK_USE_TIME : NETWORK_USE_TIME_MANUAL, &lease);
        if (err == ESP_OK) {
            err = sync_time_on_lease(&lease, utc_seconds, cancelled);
            if (err == ESP_OK && automatic && lease.bearer == NETWORK_BEARER_CELLULAR)
                atomic_store(&s_cellular_time_pending, *utc_seconds);
        }
    }
    network_service_release(&lease);
    atomic_store(&s_time_active, false);
    return err;
}

void network_service_cancel_time_sync(void) {
    (void)wifi_transfer_cancel_time_sync();
    const int64_t deadline = esp_timer_get_time() + 2000000;
    while (atomic_load(&s_time_active) && esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
}

bool network_service_take_time_sync(uint32_t *utc_seconds) {
    if (!utc_seconds) return false;
    if (wifi_transfer_take_time_sync(utc_seconds)) return true;
    uint32_t pending = atomic_exchange(&s_cellular_time_pending, 0);
    *utc_seconds = pending;
    return pending != 0;
}
