#!/usr/bin/env python3
"""运行真实租约准入与 OTA 设备分支，使用无线模型而不访问设备。
Run real bearer admission and the device OTA branch against offline radio models.
SPDX-License-Identifier: Apache-2.0
"""
from pathlib import Path
import subprocess
import tempfile

from home_cover_cache_host_test import function
from run_reader_baseline import toolchain

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main/services/network_service.c"

MODEL = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "network_service.h"
#include "cellular_service.h"
typedef void esp_netif_t;
enum { WIFI_TRANSFER_STOPPED, WIFI_TRANSFER_STARTING, WIFI_TRANSFER_READY,
       WIFI_TRANSFER_UPLOADING, WIFI_TRANSFER_ERROR };
enum { WIFI_TRANSFER_MODE_AP, WIFI_TRANSFER_MODE_STA };
typedef struct { unsigned session_id; int state, mode; bool network_ready, network_only, owns_radio; } wifi_transfer_status_t;
typedef struct { unsigned session_id; int mode; bool network_only; } wifi_transfer_cfg_t;
typedef struct { int placeholder; } wifi_transfer_network_t;
#define WIFI_TRANSFER_SCAN_MAX 8
static _Atomic network_route_t s_route;
static _Atomic network_auto_priority_t s_priority;
static atomic_uint s_wifi_leases, s_cellular_leases;
static atomic_bool s_cellular_control, s_wifi_owned_by_business;
static bool s_cellular_owned_by_business;
static atomic_bool s_resume_pending;
static int64_t s_resume_retry_at;
static bool s_radios_paused;
static uint32_t s_local_wifi_generation;
static atomic_uint s_drop_wifi_session, s_drop_media_wifi_session;
static struct {
    network_bearer_t target;
    network_route_t previous_route;
    bool target_stopped;
    int64_t retry_at;
} s_switch;
static cellular_service_status_t cellular;
static wifi_transfer_status_t wifi;
static bool configured, deny_registration, fail_wifi, cell_recovers;
static bool missing_wifi_netif, missing_cell_netif, fail_default;
static bool deny_wifi_stop, attempt_during_start;
static bool temporary_cell, deny_temporary_stop;
static esp_err_t nested_acquire_error, nested_disable_error;
static int wifi_start_error;
static bool cell_retry_due, peak_busy;
#undef memory_peak_try_begin
#define memory_peak_try_begin(kind, token) (*(token)=peak_busy ? 0 : 1, (void)(kind), peak_busy ? MEMORY_PEAK_BUSY : ESP_OK)
static unsigned wifi_detaches, wifi_blocks, radio_recoveries;
static int64_t wifi_ready_at;
static unsigned default_calls, cell_status_reads, ready_on_cell_read;
static unsigned wifi_starts, wifi_stops, wifi_scans, cell_starts, cell_stops;
static int64_t now_us;
static int wifi_marker, cell_marker;
static void *default_netif;
static int64_t esp_timer_get_time(void) { return now_us; }
static bool cancel_at_first_wait(void) { return now_us >= 100000; }
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(unsigned ms) {
    now_us += (int64_t)ms * 1000;
    if (wifi_ready_at && now_us >= wifi_ready_at) {
        wifi.network_ready = true; wifi.state = WIFI_TRANSFER_READY;
    }
}
static int esp_netif_set_default_netif(void *netif) {
    ++default_calls;
    if (fail_default) return ESP_ERR_INVALID_STATE;
    default_netif = netif; return ESP_OK;
}
static void wifi_transfer_get_status(wifi_transfer_status_t *out) { *out = wifi; }
static int wifi_transfer_get_saved_wifi(char *ssid, bool *saved) {
    strcpy(ssid, configured ? "saved-network" : ""); *saved = configured; return ESP_OK;
}
#define ESP_LOGW(...) ((void)0)
static esp_err_t ble_conflict, cell_start_error;
static esp_err_t ble_pt_network_check(void) { return ble_conflict; }
static int wifi_transfer_start_admitted(const wifi_transfer_cfg_t *cfg, memory_peak_token_t admitted) {
    (void)admitted;
    ++wifi_starts; wifi.session_id = cfg->session_id; wifi.mode = cfg->mode; wifi.network_only = cfg->network_only;
    if (attempt_during_start) {
        network_lease_t other;
        nested_acquire_error = network_service_acquire(NETWORK_USE_WEATHER, &other);
        nested_disable_error = network_service_set_enabled(NETWORK_BEARER_WIFI, false);
        assert(!other.active);
    }
    if (wifi_start_error) { wifi.state = WIFI_TRANSFER_ERROR; return wifi_start_error; }
    wifi.owns_radio = true;
    wifi.network_ready = !fail_wifi;
    wifi.state = fail_wifi ? WIFI_TRANSFER_STARTING : WIFI_TRANSFER_READY;
    return ESP_OK;
}
static esp_err_t wifi_transfer_restore_admitted(memory_peak_token_t admitted) {
    const wifi_transfer_cfg_t cfg = {.mode=WIFI_TRANSFER_MODE_STA, .network_only=true};
    const esp_err_t saved_error = wifi_start_error;
    wifi_start_error = ESP_OK;
    const esp_err_t result = wifi_transfer_start_admitted(&cfg, admitted);
    wifi_start_error = saved_error;
    return result;
}
static esp_err_t wifi_transfer_scan_admitted(wifi_transfer_network_t *out, size_t *count,
                                            memory_peak_token_t admitted) {
    assert(out && admitted); *count = 1; ++wifi_scans; return ESP_OK;
}
static void wifi_transfer_stop(void) { ++wifi_stops; wifi = (wifi_transfer_status_t){0}; }
static void *wifi_transfer_netif(void) { return missing_wifi_netif ? NULL : &wifi_marker; }
void *cellular_service_netif(void) { return missing_cell_netif ? NULL : &cell_marker; }
void cellular_service_get_status(cellular_service_status_t *out) {
    if (++cell_status_reads == ready_on_cell_read) cellular.ready = true;
    *out = cellular;
    out->running = cellular.running || cellular.ready;
}
static void app_settings_set_network_route(network_route_t route) { (void)route; }
static void app_settings_set_network_auto_priority(network_auto_priority_t priority) { (void)priority; }
static esp_err_t wifi_wake_result;
static unsigned wifi_wakes, cell_wakes;
static esp_err_t wifi_transfer_resume_after_sleep(void) { ++wifi_wakes; return wifi_wake_result; }
void cellular_service_resume_after_sleep(void) { ++cell_wakes; }
static bool wifi_transfer_pause_for_sleep(void) { return true; }
bool cellular_service_pause_for_sleep(void) { return true; }
static bool wifi_transfer_detach_http(bool media_lost) {
    assert(wifi.mode == WIFI_TRANSFER_MODE_STA && !wifi.network_only);
    wifi_detaches++;
    if (media_lost) assert(wifi_blocks);
    wifi.network_only = true;
    return true;
}
static void wifi_transfer_block_local_files(uint32_t id) {
    if (wifi.session_id == id && !wifi.network_only) ++wifi_blocks;
}
static void wifi_transfer_recover_stuck_sta(void) { ++radio_recoveries; }
esp_err_t cellular_service_set_enabled(bool enabled) {
    if (enabled && cell_start_error) return cell_start_error;
    if (enabled) ++cell_starts; else ++cell_stops;
    cellular.enabled = cellular.running = enabled;
    temporary_cell = false;
    cellular.startup_failed = false;
    if (!enabled) cellular.ready = false;
    return ESP_OK;
}
esp_err_t cellular_service_start_temporary(void) {
    assert(cellular.capability_known && cellular.capable && !cellular.enabled && !wifi.owns_radio);
    if (cell_start_error) return cell_start_error;
    if (!cellular.running) ++cell_starts;
    else assert(temporary_cell);
    temporary_cell = cellular.running = true;
    cellular.state = CELLULAR_SERVICE_STARTING;
    return ESP_OK;
}
bool cellular_service_stop_temporary(void) {
    if (!temporary_cell) return true;
    if (deny_temporary_stop) return false;
    return cellular_service_set_enabled(false) == ESP_OK;
}
esp_err_t cellular_service_enable_admitted(memory_peak_token_t *admitted) {
    assert(admitted && *admitted);
    return cellular_service_set_enabled(true);
}
static bool wifi_transfer_try_stop_if_idle(void) {
    if (deny_wifi_stop) return false;
    wifi_transfer_stop(); return true;
}
bool cellular_service_yield_idle(void) {
    assert(!cellular.ready && cellular.running);
    ++cell_stops;
    cellular.running = false;
    return true;
}
void cellular_service_poll(bool allow_start) {
    if (cell_retry_due && allow_start) {
        assert(!wifi.owns_radio);
        cell_retry_due = false;
        (void)cellular_service_set_enabled(true);
    }
    if (deny_registration) { cellular.state = CELLULAR_SERVICE_NO_REGISTRATION; cellular.ready = false; }
    if (fail_wifi && wifi.state == WIFI_TRANSFER_STARTING) wifi.state = WIFI_TRANSFER_ERROR;
    if (cell_recovers && cell_starts) { cellular.state = CELLULAR_SERVICE_READY; cellular.ready = true; }
}
static void reset(network_route_t route, network_auto_priority_t priority) {
    s_route = route; s_priority = priority; now_us = 0;
    atomic_store(&s_wifi_leases, 0); atomic_store(&s_cellular_leases, 0);
    atomic_store(&s_cellular_control, false);
    atomic_store(&s_wifi_owned_by_business, false);
    s_cellular_owned_by_business = temporary_cell = deny_temporary_stop = false;
    atomic_store(&s_resume_pending, false);
    atomic_store(&s_drop_wifi_session, 0);
    atomic_store(&s_drop_media_wifi_session, 0);
    s_radios_paused = cell_retry_due = peak_busy = false;
    s_resume_retry_at = wifi_wake_result = wifi_wakes = cell_wakes = 0;
    wifi_detaches = wifi_blocks = radio_recoveries = 0;
    memset(&s_switch, 0, sizeof(s_switch));
    cellular = (cellular_service_status_t){.capability_known=true, .capable=true,
        .enabled=true, .state=CELLULAR_SERVICE_STARTING};
    wifi = (wifi_transfer_status_t){0};
    cell_start_error = ble_conflict = ESP_OK;
    configured = true; deny_registration = fail_wifi = cell_recovers = false;
    missing_wifi_netif = missing_cell_netif = fail_default = false;
    deny_wifi_stop = attempt_during_start = false; wifi_start_error = 0; wifi_ready_at = 0;
    nested_acquire_error = nested_disable_error = ESP_OK;
    default_calls = cell_status_reads = ready_on_cell_read = 0;
    wifi_starts = wifi_stops = wifi_scans = cell_starts = cell_stops = 0; default_netif = NULL;
}
'''

CASES = r'''
static void manual_weather_cellular(void) {
    network_lease_t lease, borrower;
    reset(NETWORK_ROUTE_CELLULAR, NETWORK_AUTO_WIFI_FIRST);
    cellular.enabled = false; cellular.state = CELLULAR_SERVICE_STOPPED;
    for (int use = NETWORK_USE_TIME; use <= NETWORK_USE_OTA; ++use)
        assert(network_service_acquire((network_use_t)use, &lease) == ESP_ERR_NOT_FOUND && !lease.active);
    assert(!cell_starts && !wifi_starts);
    ble_conflict = MEMORY_PEAK_CONFLICT;
    assert(network_service_acquire(NETWORK_USE_WEATHER_MANUAL, &lease) == MEMORY_PEAK_CONFLICT && !cell_starts);
    ble_conflict = ESP_OK;
    assert(network_service_acquire(NETWORK_USE_WEATHER_MANUAL, &lease) == ESP_OK && lease.owns_cellular);
    assert(lease.bearer == NETWORK_BEARER_CELLULAR && cell_starts == 1 && !cellular.enabled && !wifi_starts);
    assert(!network_service_wait_ready_cancelable(&lease, 1000, cancel_at_first_wait));
    network_service_release(&lease);
    assert(!cellular.running && !cellular.enabled && cell_stops == 1 && !lease.active);

    deny_registration = true;
    assert(network_service_acquire(NETWORK_USE_WEATHER_MANUAL, &lease) == ESP_OK);
    assert(!network_service_wait_ready(&lease, 1000) && !wifi_starts);
    network_service_release(&lease);
    assert(!cellular.running && cell_stops == 2);
    deny_registration = false;

    assert(network_service_acquire(NETWORK_USE_WEATHER_MANUAL, &lease) == ESP_OK);
    cell_recovers = true;
    assert(network_service_wait_ready(&lease, 1000) && default_netif == &cell_marker);
    assert(network_service_acquire(NETWORK_USE_TIME, &borrower) == ESP_OK && !borrower.owns_cellular);
    network_service_release(&lease);
    assert(cellular.running && cell_stops == 2);
    atomic_store(&s_cellular_control, true);
    network_service_release(&borrower);
    atomic_store(&s_cellular_control, false);
    cell_recovers = false;
    deny_temporary_stop = true;
    network_service_poll();
    assert(cellular.running && s_cellular_owned_by_business);
    deny_temporary_stop = false;
    network_service_poll();
    assert(!cellular.running && !cellular.enabled && cell_stops == 3 && !s_cellular_owned_by_business);

    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    cellular.enabled = configured = false;
    assert(network_service_acquire(NETWORK_USE_WEATHER_MANUAL, &lease) == ESP_ERR_NOT_FOUND);
    assert(!cell_starts && !wifi_starts);

    reset(NETWORK_ROUTE_AUTO, NETWORK_AUTO_WIFI_FIRST);
    cellular.enabled = false; cellular.state = CELLULAR_SERVICE_STOPPED;
    assert(network_service_acquire(NETWORK_USE_WEATHER_MANUAL, &lease) == ESP_OK && lease.owns_wifi);
    network_service_release(&lease);
    assert(!cell_starts && wifi_starts == 1 && wifi_stops == 1);
    configured = false;
    assert(network_service_acquire(NETWORK_USE_WEATHER_MANUAL, &lease) == ESP_OK && lease.owns_cellular);
    network_service_release(&lease);

    reset(NETWORK_ROUTE_AUTO, NETWORK_AUTO_WIFI_FIRST);
    cellular.enabled = false; cellular.state = CELLULAR_SERVICE_STOPPED;
    fail_wifi = cell_recovers = true;
    assert(network_service_acquire(NETWORK_USE_WEATHER_MANUAL, &lease) == ESP_OK && lease.owns_wifi);
    assert(network_service_wait_ready(&lease, 1000) && lease.owns_cellular && wifi_stops == 1);
    network_service_release(&lease);
    assert(!cellular.running && !cellular.enabled && cell_stops == 1);

    reset(NETWORK_ROUTE_AUTO, NETWORK_AUTO_CELLULAR_FIRST);
    cellular.enabled = false; cellular.state = CELLULAR_SERVICE_STOPPED;
    deny_registration = true;
    assert(network_service_acquire(NETWORK_USE_WEATHER_MANUAL, &lease) == ESP_OK && lease.owns_cellular);
    assert(network_service_wait_ready(&lease, 1000) && lease.owns_wifi && cell_stops == 1);
    network_service_release(&lease);
    assert(!cellular.running && !cellular.enabled && wifi_stops == 1 && s_route == NETWORK_ROUTE_AUTO);

    reset(NETWORK_ROUTE_CELLULAR, NETWORK_AUTO_WIFI_FIRST);
    cellular.enabled = false; cellular.state = CELLULAR_SERVICE_STOPPED;
    wifi = (wifi_transfer_status_t){.state=WIFI_TRANSFER_READY, .mode=WIFI_TRANSFER_MODE_AP, .owns_radio=true};
    assert(network_service_acquire(NETWORK_USE_WEATHER_MANUAL, &lease) == ESP_ERR_INVALID_STATE);
    assert(!wifi_stops && !cell_starts);
    cellular = (cellular_service_status_t){.capability_known=true};
    assert(network_service_acquire(NETWORK_USE_WEATHER_MANUAL, &lease) == ESP_ERR_NOT_FOUND && !cell_starts);

    reset(NETWORK_ROUTE_CELLULAR, NETWORK_AUTO_WIFI_FIRST);
    cellular.running = cellular.ready = true; cellular.state = CELLULAR_SERVICE_READY;
    assert(network_service_acquire(NETWORK_USE_WEATHER_MANUAL, &lease) == ESP_OK && !lease.owns_cellular);
    network_service_release(&lease);
    assert(cellular.enabled && cellular.running && !cell_starts && !cell_stops);
}
static void cloudzao_borrows_only_connected_sta(void) {
    network_lease_t lease;
    reset(NETWORK_ROUTE_AUTO, NETWORK_AUTO_CELLULAR_FIRST);
    unsigned starts = wifi_starts + cell_starts;
    assert(network_service_borrow_ready_wifi(&lease) == ESP_ERR_INVALID_STATE && !lease.active);
    wifi.network_ready = true;
    wifi.mode = WIFI_TRANSFER_MODE_STA;
    assert(network_service_borrow_ready_wifi(&lease) == ESP_ERR_INVALID_STATE && !lease.active);
    wifi.network_only = true;
    atomic_store(&s_cellular_leases, 1);
    assert(network_service_borrow_ready_wifi(&lease) == ESP_ERR_INVALID_STATE && !lease.active);
    atomic_store(&s_cellular_leases, 0);
    assert(network_service_borrow_ready_wifi(&lease) == ESP_OK && lease.active && !lease.owns_wifi);
    assert(lease.bearer == NETWORK_BEARER_WIFI && !network_service_pause_for_sleep());
    unsigned stops = wifi_stops;
    network_service_release(&lease);
    assert(!lease.active && wifi_stops == stops && wifi_starts + cell_starts == starts);
    wifi = (wifi_transfer_status_t){0};
}

static void quiet_weather_ready_only(void) {
    network_lease_t lease;
    reset(NETWORK_ROUTE_AUTO, NETWORK_AUTO_WIFI_FIRST);
    assert(network_service_acquire(NETWORK_USE_WEATHER_BACKGROUND, &lease) != ESP_OK && !lease.active);
    assert(!wifi_starts && !cell_starts);
    wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_READY,
        .mode=WIFI_TRANSFER_MODE_STA,.network_ready=true};
    assert(network_service_acquire(NETWORK_USE_WEATHER_BACKGROUND, &lease) != ESP_OK);
    wifi.network_only = true;
    assert(network_service_acquire(NETWORK_USE_WEATHER_BACKGROUND, &lease) == ESP_OK && !lease.owns_wifi);
    assert(network_service_wait_ready(&lease, 30000) && default_netif == &wifi_marker);
    wifi.network_ready = false;
    assert(!network_service_wait_ready(&lease, 30000) && !wifi_starts && !cell_starts && now_us == 0);
    network_service_release(&lease);
    assert(!wifi_stops);
    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    cellular.ready = true;
    assert(network_service_acquire(NETWORK_USE_WEATHER_BACKGROUND, &lease) != ESP_OK);
    s_route = NETWORK_ROUTE_AUTO;
    assert(network_service_acquire(NETWORK_USE_WEATHER_BACKGROUND, &lease) == ESP_OK && !lease.owns_cellular);
    assert(network_service_wait_ready(&lease, 30000) && default_netif == &cell_marker);
    network_service_release(&lease);
    assert(!wifi_starts && !cell_starts && !cell_stops);
}

int main(void) {
    quiet_weather_ready_only();
    cloudzao_borrows_only_connected_sta();
    manual_weather_cellular();
    network_lease_t lease;
    reset(NETWORK_ROUTE_CELLULAR, NETWORK_AUTO_WIFI_FIRST);
    ble_conflict = MEMORY_PEAK_CONFLICT;
    assert(network_service_set_enabled(NETWORK_BEARER_CELLULAR, true) == MEMORY_PEAK_CONFLICT);
    assert(network_service_acquire(NETWORK_USE_WEATHER, &lease) == MEMORY_PEAK_CONFLICT && !lease.active);
    assert(!cell_starts && !wifi_stops && !atomic_load(&s_cellular_leases));

    // 已启动的 STA 连接可被借用，取得 IP 前不重启或误报不可用。
    reset(NETWORK_ROUTE_CELLULAR, NETWORK_AUTO_CELLULAR_FIRST);
    ble_conflict = MEMORY_PEAK_CONFLICT;
    assert(network_service_set_enabled(NETWORK_BEARER_WIFI, true) == MEMORY_PEAK_CONFLICT);
    assert(cellular.enabled && cell_stops == 0 && wifi_starts == 0 && s_route == NETWORK_ROUTE_CELLULAR);
    ble_conflict = ESP_OK;
    wifi_start_error = ESP_ERR_NO_MEM;
    assert(network_service_set_enabled(NETWORK_BEARER_WIFI, true) == ESP_ERR_NO_MEM);
    assert(cellular.enabled && cell_stops == 1 && cell_starts == 1 && s_route == NETWORK_ROUTE_CELLULAR);
    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    cellular.enabled = false;
    wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_READY, .mode=WIFI_TRANSFER_MODE_STA, .network_only=true};
    cell_start_error = ESP_ERR_NO_MEM;
    assert(network_service_set_enabled(NETWORK_BEARER_CELLULAR, true) == ESP_ERR_NO_MEM);
    assert(!cellular.enabled && wifi.network_ready && wifi_stops == 1 && wifi_starts == 1);

    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    cellular.enabled = false;
    wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_READY, .mode=WIFI_TRANSFER_MODE_STA, .network_only=true};
    assert(network_service_set_enabled(NETWORK_BEARER_CELLULAR, true) == ESP_OK);
    assert(s_switch.target == NETWORK_BEARER_CELLULAR && !wifi.network_ready);
    cellular.state = CELLULAR_SERVICE_STOPPED;
    cellular.running = false;
    network_service_poll();
    assert(s_switch.target == NETWORK_BEARER_CELLULAR && !wifi_starts);
    // AT 无应答有独立失败标志，普通 STOPPED 不触发回滚。/ No AT reply has its own failure flag; STOPPED alone does not roll back.
    cellular.startup_failed = true;
    network_service_poll();
    assert(s_switch.target == NETWORK_BEARER_NONE && wifi.network_ready && s_route == NETWORK_ROUTE_WIFI);

    // 拒绝时不能先停旧承载；扫描和本地启动均走事务入口。
    // Refusal leaves the old bearer running; scan and local bring-up use the same transaction.
    const wifi_transfer_cfg_t local = {.mode=WIFI_TRANSFER_MODE_STA, .network_only=false};
    wifi_transfer_network_t found[WIFI_TRANSFER_SCAN_MAX];
    size_t count = 0;
    reset(NETWORK_ROUTE_CELLULAR, NETWORK_AUTO_CELLULAR_FIRST);
    ble_conflict = MEMORY_PEAK_CONFLICT;
    assert(network_service_start_local_wifi(&local, NULL) == MEMORY_PEAK_CONFLICT);
    assert(network_service_scan_wifi(found, &count) == MEMORY_PEAK_CONFLICT);
    assert(cellular.enabled && !cell_stops && !wifi_starts && !wifi_scans);
    ble_conflict = ESP_OK;
    assert(network_service_scan_wifi(found, &count) == ESP_OK && count == 1);
    assert(cellular.enabled && cell_stops == 1 && cell_starts == 1);
    fail_wifi = true;
    assert(network_service_start_local_wifi(&local, NULL) == ESP_OK && !cellular.enabled);
    peak_busy = true;
    network_service_poll();
    assert(!wifi.owns_radio && s_switch.target_stopped && !cellular.enabled && cell_starts == 1);
    peak_busy = false;
    now_us += 999999;
    network_service_poll();
    assert(!cellular.enabled && cell_starts == 1);
    ++now_us;
    network_service_poll();
    assert(cellular.enabled && cell_stops == 2 && cell_starts == 2 && !wifi.network_ready);

    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    cellular.enabled = false;
    wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_READY, .mode=WIFI_TRANSFER_MODE_STA, .network_only=true};
    assert(network_service_scan_wifi(found, &count) == ESP_OK && wifi.network_ready && wifi.network_only);
    wifi_start_error = ESP_ERR_NO_MEM;
    assert(network_service_start_local_wifi(&local, NULL) == ESP_ERR_NO_MEM);
    assert(wifi.network_ready && wifi.network_only && wifi_stops == 2);

    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    network_lease_t borrower;
    assert(network_service_acquire(NETWORK_USE_WEATHER, &lease) == ESP_OK && lease.owns_wifi);
    assert(network_service_acquire(NETWORK_USE_TIME, &borrower) == ESP_OK && !borrower.owns_wifi);
    network_service_release(&lease);
    assert(wifi.network_ready && !wifi_stops);
    network_service_release(&borrower);
    assert(wifi_stops == 1 && !wifi.network_ready);

    // Borrow a connecting STA without restarting it or rejecting it before IP acquisition.
    for (int automatic = 0; automatic < 2; ++automatic) {
        for (int use = NETWORK_USE_TIME; use <= NETWORK_USE_OTA; ++use) {
            reset(automatic ? NETWORK_ROUTE_AUTO : NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
            cellular.enabled = false;
            wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_STARTING,
                .mode=WIFI_TRANSFER_MODE_STA, .network_only=true};
            wifi_ready_at = 200000;
            assert(network_service_acquire((network_use_t)use, &lease) == ESP_OK);
            assert(lease.active && !lease.owns_wifi && !wifi_starts);
            assert(network_service_wait_ready(&lease, 30000) && now_us == 200000);
            network_service_release(&lease);
            assert(!wifi_stops && !atomic_load(&s_wifi_leases));
        }
    }
    // 只有空闲纯联网会话可从 ERROR 重建；一次申请只启动一次。
    // Rebuild only an idle network-only ERROR session, starting at most once per acquisition.
    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_ERROR,
        .mode=WIFI_TRANSFER_MODE_STA, .network_only=true};
    assert(network_service_acquire(NETWORK_USE_OTA, &lease) == ESP_OK);
    assert(lease.owns_wifi && wifi_starts == 1 && wifi_stops == 1);
    assert(network_service_wait_ready(&lease, 30000));
    network_service_release(&lease);
    assert(wifi_stops == 2 && !atomic_load(&s_wifi_leases));
    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_ERROR,
        .mode=WIFI_TRANSFER_MODE_STA, .network_only=true};
    assert(network_service_set_enabled(NETWORK_BEARER_WIFI, true) == ESP_OK);
    assert(wifi_starts == 1 && wifi_stops == 1 && wifi.network_ready);
    for (int protected_session = 0; protected_session < 3; ++protected_session) {
        reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
        wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_ERROR,
            .mode=WIFI_TRANSFER_MODE_STA, .network_only=true};
        if (protected_session == 0) wifi.network_only = false;
        if (protected_session == 1) deny_wifi_stop = true;
        if (protected_session == 2) atomic_store(&s_wifi_leases, 1);
        assert(network_service_acquire(NETWORK_USE_OTA, &lease) == ESP_ERR_INVALID_STATE);
        assert(!lease.active && !wifi_starts && !wifi_stops);
    }
    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_STARTING, .mode=0};
    assert(network_service_acquire(NETWORK_USE_OTA, &lease) == ESP_ERR_INVALID_STATE);
    assert(!wifi_starts && !wifi_stops);
    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    wifi_start_error = ESP_ERR_NO_MEM;
    assert(network_service_acquire(NETWORK_USE_OTA, &lease) == ESP_ERR_NO_MEM);
    assert(!lease.active && wifi_starts == 1 && !atomic_load(&s_wifi_leases));
    reset(NETWORK_ROUTE_AUTO, NETWORK_AUTO_WIFI_FIRST);
    cellular.enabled = false; wifi_start_error = ESP_ERR_NO_MEM;
    assert(network_service_acquire(NETWORK_USE_OTA, &lease) == ESP_ERR_NO_MEM);
    assert(!lease.active && wifi_starts == 1 && !atomic_load(&s_wifi_leases));
    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    atomic_store(&s_cellular_control, true);
    assert(network_service_acquire(NETWORK_USE_OTA, &lease) == MEMORY_PEAK_BUSY);
    assert(!lease.active && !wifi_starts && !atomic_load(&s_wifi_leases));
    // 启动或恢复发布租约前，重入申请和关无线均须被同一控制门挡住。
    // The same gate rejects reentrant acquire/shutdown until startup publishes its lease.
    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST); attempt_during_start = true;
    assert(network_service_acquire(NETWORK_USE_OTA, &lease) == ESP_OK);
    assert(nested_acquire_error == MEMORY_PEAK_BUSY && nested_disable_error == MEMORY_PEAK_BUSY);
    assert(wifi_starts == 1 && !wifi_stops && atomic_load(&s_wifi_leases) == 1);
    assert(!atomic_load(&s_cellular_control));
    network_service_release(&lease);
    assert(wifi_stops == 1 && !atomic_load(&s_wifi_leases));
    // No ready bearer must leave an existing route alone, including at acquire.
    reset(NETWORK_ROUTE_CELLULAR, NETWORK_AUTO_WIFI_FIRST);
    default_netif = &wifi_marker;
    network_service_poll();
    assert(default_calls == 0 && default_netif == &wifi_marker);
    assert(network_service_acquire(NETWORK_USE_WEATHER, &lease) == ESP_OK);
    assert(default_calls == 0 && default_netif == &wifi_marker);
    // IP may arrive between poll's snapshot and wait_ready's second snapshot.
    cell_status_reads = 0; ready_on_cell_read = 2;
    assert(network_service_wait_ready(&lease, 300));
    assert(now_us == 0 && default_netif == &cell_marker && default_calls == 1);
    network_service_release(&lease);
    // Cancellation during association leaves cleanup and lease ownership with the caller.
    reset(NETWORK_ROUTE_AUTO, NETWORK_AUTO_WIFI_FIRST);
    cellular.enabled = false;
    wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_STARTING,
        .mode=WIFI_TRANSFER_MODE_STA, .network_only=true};
    wifi_ready_at = 200000;
    assert(network_service_acquire(NETWORK_USE_WEATHER, &lease) == ESP_OK);
    assert(!network_service_wait_ready_cancelable(&lease, 30000, cancel_at_first_wait));
    assert(now_us == 100000 && lease.active && lease.bearer == NETWORK_BEARER_WIFI && !cell_starts);
    network_service_release(&lease);
    assert(!atomic_load(&s_wifi_leases));
    // A ready flag alone is insufficient without a usable/default interface.
    for (int missing = 0; missing < 2; ++missing) {
        reset(NETWORK_ROUTE_CELLULAR, NETWORK_AUTO_WIFI_FIRST);
        cellular.ready = true; missing_cell_netif = missing; fail_default = !missing;
        assert(network_service_acquire(NETWORK_USE_OTA, &lease) == ESP_OK);
        assert(!network_service_wait_ready(&lease, 300));
        assert(!default_netif && now_us == 300000);
        network_service_release(&lease);
    }
    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    missing_wifi_netif = true;
    assert(network_service_acquire(NETWORK_USE_OTA, &lease) == ESP_OK);
    assert(!network_service_wait_ready(&lease, 300) && !default_netif);
    network_service_release(&lease);
    // Capability probes never rewrite Auto or a user's explicit route.
    for (int route = NETWORK_ROUTE_WIFI; route <= NETWORK_ROUTE_AUTO; ++route) {
        reset((network_route_t)route, NETWORK_AUTO_WIFI_FIRST);
        cellular.capable = false;
        network_service_poll();
        network_status_t status;
        network_service_get_status(&status);
        assert(status.route == (network_route_t)route);
        const esp_err_t result = network_service_acquire(NETWORK_USE_WEATHER, &lease);
        if (route == NETWORK_ROUTE_CELLULAR) {
            assert(result == ESP_ERR_NOT_FOUND && !lease.active && !wifi_starts);
        } else {
            assert(result == ESP_OK && lease.bearer == NETWORK_BEARER_WIFI);
            network_service_release(&lease);
        }
        cellular.capable = true;
        network_service_poll();
        network_service_get_status(&status);
        assert(status.route == (network_route_t)route);
    }
    // The automatic route immediately falls back after a registration denial.
    reset(NETWORK_ROUTE_AUTO, NETWORK_AUTO_CELLULAR_FIRST);
    assert(network_service_acquire(NETWORK_USE_WEATHER, &lease) == ESP_OK);
    assert(lease.bearer == NETWORK_BEARER_CELLULAR);
    deny_registration = true;
    assert(network_service_wait_ready(&lease, 30000));
    assert(lease.bearer == NETWORK_BEARER_WIFI && lease.owns_wifi);
    assert(wifi_starts == 1 && default_netif == &wifi_marker);
    assert(atomic_load(&s_cellular_leases) == 0 && atomic_load(&s_wifi_leases) == 1);
    // Recovery of the preferred 4G bearer cannot steal the fallback Wi-Fi lease.
    deny_registration = false; cellular.ready = true;
    network_service_poll();
    assert(default_netif == &wifi_marker);
    network_service_release(&lease);
    assert(wifi_stops == 1 && !lease.active && cellular.enabled);
    assert(atomic_load(&s_wifi_leases) == 0);
    network_service_poll();
    assert(default_netif == &cell_marker);

    // An explicitly selected 4G route reports failure and never starts Wi-Fi.
    reset(NETWORK_ROUTE_CELLULAR, NETWORK_AUTO_CELLULAR_FIRST);
    assert(network_service_acquire(NETWORK_USE_WEATHER, &lease) == ESP_OK);
    deny_registration = true;
    assert(!network_service_wait_ready(&lease, 30000));
    assert(!wifi_starts && lease.bearer == NETWORK_BEARER_CELLULAR);
    network_service_release(&lease);
    assert(atomic_load(&s_cellular_leases) == 0);
    for (int state = CELLULAR_SERVICE_NO_SIM; state <= CELLULAR_SERVICE_SIM_NOT_READY; ++state) {
        reset(NETWORK_ROUTE_CELLULAR, NETWORK_AUTO_WIFI_FIRST);
        cellular.state = state;
        assert(network_service_acquire(NETWORK_USE_WEATHER, &lease) == ESP_ERR_NOT_FOUND);
        assert(!lease.active && !wifi_starts && !cell_starts);
    }
    reset(NETWORK_ROUTE_AUTO, NETWORK_AUTO_CELLULAR_FIRST);
    cellular.state = CELLULAR_SERVICE_SIM_NOT_READY;
    strcpy(cellular.detail, "SIM 需要 PIN 解锁");
    network_status_t status;
    network_service_get_status(&status);
    assert(!strcmp(status.cellular_detail, cellular.detail));
    assert(network_service_acquire(NETWORK_USE_WEATHER, &lease) == ESP_OK);
    assert(lease.bearer == NETWORK_BEARER_WIFI && !cell_starts);
    network_service_release(&lease);

    // Wi-Fi failures can fall back to enabled 4G under the automatic route.
    reset(NETWORK_ROUTE_AUTO, NETWORK_AUTO_WIFI_FIRST);
    fail_wifi = cell_recovers = true;
    assert(network_service_acquire(NETWORK_USE_WEATHER, &lease) == ESP_OK);
    assert(network_service_wait_ready(&lease, 30000));
    assert(lease.bearer == NETWORK_BEARER_CELLULAR && default_netif == &cell_marker);
    assert(wifi_stops == 1 && cell_starts == 1);
    network_service_release(&lease);
    reset(NETWORK_ROUTE_AUTO, NETWORK_AUTO_WIFI_FIRST);
    wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_STARTING,
        .mode=WIFI_TRANSFER_MODE_STA,.network_only=true};
    fail_wifi = cell_recovers = true;
    assert(network_service_acquire(NETWORK_USE_WEATHER, &lease) == ESP_OK && !lease.owns_wifi);
    assert(network_service_wait_ready(&lease, 30000));
    assert(lease.bearer == NETWORK_BEARER_CELLULAR && wifi_stops == 1 && cell_starts == 1);
    // Likewise, a recovered preferred Wi-Fi bearer waits for the 4G lease to end.
    fail_wifi = false;
    wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_READY,
        .mode=WIFI_TRANSFER_MODE_STA, .network_ready=true};
    network_service_poll();
    assert(default_netif == &cell_marker);
    network_service_release(&lease);
    assert(!atomic_load(&s_wifi_leases) && !atomic_load(&s_cellular_leases));
    network_service_poll();
    assert(default_netif == &wifi_marker);

    // Forced Wi-Fi never silently borrows a connected cellular bearer.
    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    configured = false; cellular.ready = true;
    assert(network_service_acquire(NETWORK_USE_WEATHER, &lease) == ESP_ERR_INVALID_STATE);
    assert(!lease.active && !cell_starts);
    // Active business leases protect the radio against shutdown.
    for (int use = NETWORK_USE_TIME; use <= NETWORK_USE_OTA; ++use) {
        reset(NETWORK_ROUTE_CELLULAR, NETWORK_AUTO_WIFI_FIRST);
        cellular.ready = true;
        assert(network_service_acquire((network_use_t)use, &lease) == ESP_OK);
        assert(network_service_set_enabled(NETWORK_BEARER_CELLULAR, false) == ESP_ERR_INVALID_STATE);
        assert(!cell_stops && !atomic_load(&s_cellular_control));
        network_service_release(&lease);
        assert(network_service_set_enabled(NETWORK_BEARER_CELLULAR, false) == ESP_OK && cell_stops == 1);
        assert(!cellular.enabled && !atomic_load(&s_cellular_leases));
    }
    // The control gate also blocks a new lease while the radio is being changed.
    reset(NETWORK_ROUTE_CELLULAR, NETWORK_AUTO_WIFI_FIRST);
    atomic_store(&s_cellular_control, true);
    assert(network_service_set_enabled(NETWORK_BEARER_CELLULAR, false) == MEMORY_PEAK_BUSY);
    assert(network_service_acquire(NETWORK_USE_OTA, &lease) == MEMORY_PEAK_BUSY);
    assert(!atomic_load(&s_cellular_leases) && !cell_stops);
    // 修改首选承载不抢占正在运行的 OTA，也不悄悄启动另一射频。
    // Route preference changes cannot replace the active OTA bearer or start a second radio.
    reset(NETWORK_ROUTE_CELLULAR, NETWORK_AUTO_CELLULAR_FIRST);
    cellular.ready = true;
    assert(network_service_acquire(NETWORK_USE_OTA, &lease) == ESP_OK);
    assert(network_service_set_route(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST) == ESP_OK);
    assert(network_service_acquire(NETWORK_USE_WEATHER, &borrower) == ESP_ERR_INVALID_STATE);
    assert(!borrower.active && !wifi_starts && !cell_stops && default_netif == &cell_marker);
    assert(!network_service_pause_for_sleep());
    assert(network_service_set_route(NETWORK_ROUTE_AUTO, NETWORK_AUTO_WIFI_FIRST) == ESP_OK);
    assert(network_service_acquire(NETWORK_USE_TIME, &borrower) == ESP_OK);
    assert(borrower.bearer == NETWORK_BEARER_CELLULAR && !wifi_starts);
    network_service_release(&borrower);
    network_service_release(&lease);

    // 后台探测/唤醒重试不得越过正在运行的 Wi-Fi。/ Background retries wait until Wi-Fi stops.
    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_READY, .mode=WIFI_TRANSFER_MODE_STA,
                                     .network_ready=true};
    cell_retry_due = true;
    network_service_poll();
    assert(!cell_starts && cell_retry_due && wifi.network_ready);
    assert(network_service_set_enabled(NETWORK_BEARER_WIFI, false) == ESP_OK);
    network_service_poll();
    assert(cell_starts == 1 && !cell_retry_due);

    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    assert(network_service_pause_for_sleep());
    assert(network_service_acquire(NETWORK_USE_WEATHER, &lease) == MEMORY_PEAK_BUSY && !wifi_starts);
    wifi_wake_result = MEMORY_PEAK_BUSY;
    network_service_resume_after_sleep();
    assert(wifi_wakes == 1 && !cell_wakes);
    network_service_poll();
    assert(wifi_wakes == 1 && s_radios_paused);
    now_us += 1000000;
    wifi_wake_result = ESP_OK;
    network_service_poll();
    assert(wifi_wakes == 2 && cell_wakes == 1 && !s_radios_paused);

    // 离页与拔卡不拆掉正借用的 STA；旧页面编号不能误停替代会话。
    // Exit/media loss leave borrowed STA up; a stale page ID cannot stop a replacement.
    reset(NETWORK_ROUTE_WIFI, NETWORK_AUTO_WIFI_FIRST);
    cellular.enabled = false;
    uint32_t owner;
    assert(network_service_start_local_wifi(&local, &owner) == ESP_OK && owner);
    assert(network_service_stop_owned_wifi(owner + 1, false) && !wifi_stops);
    assert(network_service_acquire(NETWORK_USE_OTA, &lease) == ESP_OK);
    network_service_poll();
    assert(!radio_recoveries);
    assert(network_service_stop_owned_wifi(owner, true) && wifi_blocks == 1 && wifi_detaches == 1);
    assert(wifi.network_ready && !wifi_stops);
    network_service_release(&lease);
    assert(wifi_stops == 1 && !wifi.network_ready);
    puts("PASS: bearer fallback, readiness races, route failures, lease route stability and radio exclusion");
}
'''


OTA_MODEL = r'''
#define ESP_PLATFORM 1
#include <time.h>
#include "ota_online.h"
static network_lease_t s_network_lease;
static bool s_network_lease_active;
static network_use_t s_network_use;
static atomic_bool s_cancel, s_time_active;
static atomic_uint s_cellular_time_pending;
static pico_update_status_t update;
static unsigned snapshots, warnings, sync_calls, time_notes;
static int snapshot_bearer;
static esp_err_t sync_error;
static time_t clock_utc;
static time_t ota_test_time(time_t *out) {
    if (out) *out = clock_utc;
    return clock_utc;
}
#define time ota_test_time
#undef ESP_LOGW
#define ESP_LOGW(tag, format, error) ((void)(tag), (void)(format), (void)(error), ++warnings)
static const char *esp_err_to_name(esp_err_t error) { (void)error; return "test-error"; }
static void state(pico_update_state_t value, const char *message) {
    assert(message && strlen(message) < sizeof(update.message));
    update.state = value;
    strcpy(update.message, message);
}
static void network_diagnostics_snapshot(const char *owner, int bearer) {
    assert(!strcmp(owner, "ota")); ++snapshots; snapshot_bearer = bearer;
}
static esp_err_t sync_time_on_lease(network_lease_t *lease, uint32_t *utc, bool (*cancelled)(void)) {
    if (!network_service_wait_ready_cancelable(lease, 30000, cancelled)) return ESP_ERR_TIMEOUT;
    assert(lease->active); ++sync_calls; *utc = 1704067200;
    return sync_error;
}
static void app_time_service_note_success(uint32_t utc) {
    assert(utc == 1704067200); ++time_notes;
}
static void reset_ota(network_route_t route) {
    reset(route, NETWORK_AUTO_WIFI_FIRST);
    cellular.enabled = false;
    s_network_use = NETWORK_USE_OTA;
    s_network_lease = (network_lease_t){0}; s_network_lease_active = false;
    atomic_store(&s_cancel, false);
    update = (pico_update_status_t){.state=PICO_UPDATE_CHECKING};
    snapshots = warnings = sync_calls = time_notes = 0;
    snapshot_bearer = NETWORK_BEARER_NONE;
    sync_error = ESP_OK; clock_utc = 1704067200;
}
static void release_ota(void) {
    if (s_network_lease_active) network_service_release(&s_network_lease);
    s_network_lease_active = false;
    assert(!atomic_load(&s_wifi_leases) && !atomic_load(&s_cellular_leases));
}
'''

OTA_CASES = r'''
int main(void) {
    bool owned = false;
    // 编译真实 ESP_PLATFORM 分支，连通 OTA 与租约准入、等待、释放流程。
    // Compile the actual ESP_PLATFORM branch to integrate OTA admission, readiness and cleanup.
    for (int automatic = 0; automatic < 2; ++automatic) {
        reset_ota(automatic ? NETWORK_ROUTE_AUTO : NETWORK_ROUTE_WIFI);
        wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_STARTING,
            .mode=WIFI_TRANSFER_MODE_STA, .network_only=true};
        wifi_ready_at = 200000;
        assert(online(&owned));
        assert(now_us == 200000 && !wifi_starts && !wifi_stops && !owned);
        assert(s_network_lease_active && !s_network_lease.owns_wifi);
        assert(snapshots == 1 && snapshot_bearer == NETWORK_BEARER_WIFI && !warnings);
        release_ota(); assert(!wifi_stops && wifi.network_ready);
    }
    reset_ota(NETWORK_ROUTE_WIFI);
    wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_ERROR,
        .mode=WIFI_TRANSFER_MODE_STA, .network_only=true};
    assert(online(&owned) && wifi_starts == 1 && wifi_stops == 1);
    assert(s_network_lease.owns_wifi);
    release_ota(); assert(wifi_stops == 2);

    // 热点、文件服务、正在执行的文件操作和其他租约不会被错误重连打断。
    // Failed-session recovery must not interrupt AP, file-server, active file work or another lease.
    for (int protected_session = 0; protected_session < 4; ++protected_session) {
        reset_ota(NETWORK_ROUTE_WIFI);
        wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_ERROR,
            .mode=WIFI_TRANSFER_MODE_STA, .network_only=true};
        if (protected_session == 0) wifi.mode = WIFI_TRANSFER_MODE_AP;
        if (protected_session == 1) wifi.network_only = false;
        if (protected_session == 2) deny_wifi_stop = true;
        if (protected_session == 3) atomic_store(&s_wifi_leases, 1);
        assert(!online(&owned));
        assert(update.state == PICO_UPDATE_FAILED && !s_network_lease_active);
        assert(!wifi_starts && !wifi_stops && warnings == 1 && snapshots == 1);
        assert(strstr(update.message, protected_session == 0 ? "热点" : "正在使用或切换"));
        assert(atomic_load(&s_wifi_leases) == (protected_session == 3 ? 1u : 0u));
    }
    reset_ota(NETWORK_ROUTE_WIFI); configured = false;
    assert(!online(&owned) && strstr(update.message, "尚未保存Wi-Fi"));
    assert(!wifi_starts && !s_network_lease_active);
    for (int automatic = 0; automatic < 2; ++automatic) {
        reset_ota(automatic ? NETWORK_ROUTE_AUTO : NETWORK_ROUTE_WIFI);
        wifi_start_error = ESP_ERR_NO_MEM;
        assert(!online(&owned) && strstr(update.message, "内存不足"));
        assert(wifi_starts == 1 && !s_network_lease_active && !atomic_load(&s_wifi_leases));
    }

    struct { int state; const char *message; } failures[] = {
        {CELLULAR_SERVICE_NO_SIM, "未检测到SIM卡"},
        {CELLULAR_SERVICE_SIM_NOT_READY, "SIM卡未就绪"},
        {CELLULAR_SERVICE_NO_REGISTRATION, "移动网络注册失败"},
        {CELLULAR_SERVICE_ERROR, "移动网络连接失败"},
    };
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        reset_ota(NETWORK_ROUTE_CELLULAR); cellular.enabled = true;
        cellular.state = failures[i].state;
        assert(!online(&owned) && strstr(update.message, failures[i].message));
        assert(update.state == PICO_UPDATE_FAILED && !s_network_lease_active);
        assert(!wifi_starts && !cell_starts && warnings == 1 && snapshots == 1);
    }
    reset_ota(NETWORK_ROUTE_CELLULAR);
    assert(!online(&owned) && strstr(update.message, "先开启移动网络"));
    assert(!cell_starts);
    reset_ota(NETWORK_ROUTE_CELLULAR);
    s_network_use = NETWORK_USE_OTA_MANUAL; cell_recovers = true; clock_utc = 0;
    assert(online(&owned) && s_network_lease.owns_cellular && cell_starts == 1);
    assert(!cellular.enabled && s_network_lease.use == NETWORK_USE_OTA_MANUAL && sync_calls == 1 && time_notes == 1);
    release_ota(); assert(cell_stops == 1 && !cellular.running && !cellular.enabled);
    reset_ota(NETWORK_ROUTE_CELLULAR);
    s_network_use = NETWORK_USE_OTA_MANUAL; deny_registration = true;
    assert(!online(&owned) && strstr(update.message, "移动网络注册失败") && cell_stops == 1);
    reset_ota(NETWORK_ROUTE_CELLULAR);
    s_network_use = NETWORK_USE_OTA_MANUAL; cell_start_error = ESP_ERR_NO_MEM;
    assert(!online(&owned) && strstr(update.message, "内存不足") && !cell_starts);

    reset_ota(NETWORK_ROUTE_CELLULAR);
    uint32_t utc = 0;
    assert(network_service_sync_time(&utc, true, NULL) == ESP_ERR_NOT_FOUND && !cell_starts);
    now_us = 100000;
    assert(network_service_sync_time(&utc, false, cancel_at_first_wait) == ESP_ERR_INVALID_STATE && !cell_starts);
    cell_recovers = true;
    assert(network_service_sync_time(&utc, false, NULL) == ESP_OK && utc == 1704067200);
    assert(cell_starts == 1 && cell_stops == 1 && !cellular.enabled && !atomic_load(&s_cellular_time_pending));
    reset_ota(NETWORK_ROUTE_CELLULAR); cellular.capable = false;
    assert(!online(&owned) && strstr(update.message, "未检测到4G模块"));
    reset_ota(NETWORK_ROUTE_CELLULAR);
    cellular.capability_known = cellular.capable = false;
    assert(!online(&owned) && strstr(update.message, "正在检测移动网络"));
    assert(!wifi_starts);

    reset_ota(NETWORK_ROUTE_WIFI);
    wifi = (wifi_transfer_status_t){.owns_radio=true,.state=WIFI_TRANSFER_STARTING,
        .mode=WIFI_TRANSFER_MODE_STA, .network_only=true};
    assert(!online(&owned) && now_us == 30000000);
    assert(update.state == PICO_UPDATE_FAILED && strstr(update.message, "连接超时"));
    assert(!s_network_lease_active && !s_network_lease.active && !wifi_stops);
    assert(!atomic_load(&s_wifi_leases));
    reset_ota(NETWORK_ROUTE_CELLULAR); cellular.enabled = true; deny_registration = true;
    assert(!online(&owned) && strstr(update.message, "移动网络注册失败"));
    assert(!s_network_lease_active && !s_network_lease.active && !atomic_load(&s_cellular_leases));
    assert(!wifi_starts);

    reset_ota(NETWORK_ROUTE_WIFI); clock_utc = 0;
    assert(online(&owned) && sync_calls == 1 && time_notes == 1);
    release_ota();
    reset_ota(NETWORK_ROUTE_WIFI); clock_utc = 0; sync_error = ESP_ERR_TIMEOUT;
    assert(!online(&owned) && strstr(update.message, "网络对时失败"));
    assert(sync_calls == 1 && !time_notes);
    release_ota(); assert(wifi_stops == 1);
    reset_ota(NETWORK_ROUTE_WIFI); atomic_store(&s_cancel, true);
    assert(!online(&owned));
    release_ota(); assert(wifi_stops == 1);
    puts("PASS: device OTA admission, Wi-Fi retry, protected sessions, actionable failures and lease cleanup");
}
'''


def main(cases=CASES, extra=""):
    with tempfile.TemporaryDirectory(prefix="network-route-") as folder:
        work = Path(folder)
        env, compiler = toolchain(work)
        (work / "esp_err.h").write_text(
            "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n"
            "#define ESP_ERR_NOT_FOUND -1\n#define ESP_ERR_INVALID_ARG -2\n#define ESP_ERR_INVALID_STATE -3\n"
            "#define ESP_ERR_NO_MEM -4\n#define ESP_ERR_TIMEOUT -5\n#define ESP_ERR_NOT_SUPPORTED -6\n")
        unit = MODEL
        for name in ("resolve_ready_bearer", "saved_wifi", "start_wifi_for_lease",
                     "cellular_can_start_for_lease", "select_bearer_netif",
                     "acquire_bearer", "stop_unused_business_wifi", "stop_unused_business_cellular_under_gate", "poll_switch_rollback",
                     "resume_under_gate", "network_service_resume_after_sleep", "network_service_pause_for_sleep",
                     "stop_owned_wifi_under_gate", "network_service_poll",
                     "network_service_get_status", "network_service_acquire", "network_service_borrow_ready_wifi",
                     "network_service_wait_ready", "network_service_wait_ready_cancelable",
                     "network_service_release", "remember_rollback",
                     "restore_cellular_after_wifi_error", "network_service_set_enabled",
                     "local_wifi", "network_service_start_local_wifi", "network_service_scan_wifi",
                     "network_service_stop_owned_wifi", "network_service_set_route"):
            unit += function(name, SOURCE) + "\n"
        source, binary = work / "test.c", work / "test"
        source.write_text(unit + extra + cases)
        subprocess.run(compiler + ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                                  "-fsanitize=address,undefined", "-I" + str(work),
                                  "-Imain/services", "-Imain/services/cellular", "-Imain/ota", str(source),
                                  "-o", str(binary)], cwd=ROOT, env=env, check=True)
        subprocess.run([str(binary)], env=env, check=True)


if __name__ == "__main__":
    main()
    ota_source = ROOT / "main/ota/ota_online.c"
    ota_extra = OTA_MODEL
    ota_extra += function("network_service_sync_time_on_lease", SOURCE) + "\n"
    for symbol in ("ota_bearer", "network_unavailable_notice", "ota_cancelled", "online"):
        ota_extra += function(symbol, ota_source) + "\n"
    ota_extra += function("network_service_sync_time", SOURCE) + "\n"
    main(OTA_CASES, ota_extra)
