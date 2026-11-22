#pragma once
#include "transfer_ui_test_env.h"
#define network_service_scan_wifi wifi_transfer_scan_wifi

static inline esp_err_t network_service_start_local_wifi(const wifi_transfer_cfg_t *cfg, uint32_t *id) {
    static uint32_t generation;
    const esp_err_t result = wifi_transfer_start(cfg);
    *id = result == ESP_OK ? ++generation : 0;
    if (*id) {
        test_status.session_id = *id;
        test_status.network_only = cfg->network_only;
    }
    return result;
}
static inline bool network_service_stop_local_wifi(void) {
    if (!wifi_transfer_try_stop_if_idle()) return false;
    wifi_transfer_stop();
    return true;
}
static inline bool network_service_stop_owned_wifi(uint32_t id, bool media_lost) {
    if (!id || test_status.session_id != id) return true;
    if (!media_lost && !wifi_transfer_try_stop_if_idle()) return false;
    wifi_transfer_stop();
    return true;
}
static inline void network_service_drop_owned_wifi(uint32_t id, bool media_lost) {
    (void)id;
    (void)media_lost;
}
