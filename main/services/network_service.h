/* SPDX-License-Identifier: Apache-2.0
 * External-network route and lease service.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "network_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool cellular_capability_known;
    bool cellular_capable;
    bool cellular_enabled;
    bool cellular_ready;
    int cellular_signal;
    uint8_t cellular_state;
    char cellular_detail[96];
    bool wifi_enabled;
    bool wifi_ready;
    network_route_t route;
    network_auto_priority_t auto_priority;
    network_bearer_t active_bearer;
} network_status_t;

typedef struct {
    network_bearer_t bearer;
    network_use_t use;
    bool owns_wifi;
    bool owns_cellular;
    bool active;
} network_lease_t;

void network_service_init(void);
void network_service_poll(void);
/// 承载静默/恢复与业务准入共用控制门。/ Sleep handoff shares the bearer gate with business admission.
bool network_service_pause_for_sleep(void);
void network_service_resume_after_sleep(void);
void network_service_get_status(network_status_t *out);

/// Toggle one radio and select it as the business route when enabling it.
esp_err_t network_service_set_enabled(network_bearer_t bearer, bool enabled);
esp_err_t network_service_set_route(network_route_t route, network_auto_priority_t priority);
network_route_t network_service_route(void);
network_auto_priority_t network_service_auto_priority(void);

/// Acquire the user-selected bearer. Forced routes fail closed; AUTO follows priority.
esp_err_t network_service_acquire(network_use_t use, network_lease_t *lease);
/// 只借用已经就绪的纯联网 STA；不启动无线、不回退。/ Borrow only a ready network-only STA; no radio start or fallback.
esp_err_t network_service_borrow_ready_wifi(network_lease_t *lease);
bool network_service_wait_ready(network_lease_t *lease, uint32_t timeout_ms);
/// 取消不消费租约；调用者照常释放。/ Cancellation leaves lease ownership with the caller.
bool network_service_wait_ready_cancelable(network_lease_t *lease, uint32_t timeout_ms,
                                           bool (*cancelled)(void));
void network_service_release(network_lease_t *lease);

/// 通过选定承载对时；只有手动请求可临时开启关闭的 4G。/ Sync over the selected bearer; only manual requests may start switched-off cellular.
esp_err_t network_service_sync_time(uint32_t *utc_seconds, bool automatic, bool (*cancelled)(void));
esp_err_t network_service_sync_time_on_lease(network_lease_t *lease, uint32_t *utc_seconds, bool (*cancelled)(void));
/// 调用者先发取消信号，再请求 Wi-Fi 中断并有界等待租约交还。/ Signal cancellation first, then interrupt Wi-Fi sync and wait boundedly for handoff.
void network_service_cancel_time_sync(void);
/// Consume a successful cellular-side sync on the main loop. Wi-Fi keeps its
/// existing pending-event queue for compatibility.
bool network_service_take_time_sync(uint32_t *utc_seconds);

#ifdef __cplusplus
}
#endif
