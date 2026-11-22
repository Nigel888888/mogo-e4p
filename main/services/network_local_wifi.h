#pragma once
#include "wifi_transfer.h"

/// 前台本地 Wi-Fi 操作：切换失败恢复原承载，扫描成功也恢复。/ Local Wi-Fi: restore the old bearer on failure and after a scan.
esp_err_t network_service_start_local_wifi(const wifi_transfer_cfg_t *cfg, uint32_t *session_id);
esp_err_t network_service_scan_wifi(wifi_transfer_network_t out[WIFI_TRANSFER_SCAN_MAX], size_t *count);

/// 仅在没有业务借用时停服。/ Stop only when no business lease borrows the radio.
bool network_service_stop_local_wifi(void);

/// 只关闭指定页面拥有的会话，保留其他业务借用的 STA。/ Close only the page-owned session, preserving a borrowed STA.
bool network_service_stop_owned_wifi(uint32_t session_id, bool media_lost);
/// 退出无法同步停服时，按原会话编号延后重试。/ Retry a deferred page exit by exact session ID.
void network_service_drop_owned_wifi(uint32_t session_id, bool media_lost);
