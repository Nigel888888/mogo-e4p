/* SPDX-License-Identifier: Apache-2.0
 * Runtime NT26 capability and cellular bearer service.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "memory_peak.h"

/// NT26 UART 帧的最大缓冲需求（包括封装）。/ Maximum NT26 UART frame buffer, including framing.
#define CELLULAR_UART_FRAME_MAX_BYTES 1600u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CELLULAR_SERVICE_DISABLED = 0,
    CELLULAR_SERVICE_STOPPED,
    CELLULAR_SERVICE_PROBING,
    CELLULAR_SERVICE_STARTING,
    CELLULAR_SERVICE_READY,
    CELLULAR_SERVICE_NO_SIM,
    CELLULAR_SERVICE_NO_REGISTRATION,
    CELLULAR_SERVICE_ERROR,
    CELLULAR_SERVICE_SIM_NOT_READY,
} cellular_service_state_t;

typedef struct {
    /// A probe has completed; false means the unified image is still probing.
    bool capability_known;
    /// The NT26 hardware answered its AT probe. SIM/registration are runtime state.
    bool capable;
    bool enabled;
    bool ready;
    bool running; ///< 驱动仍拥有资源（包括探测或清理）。/ Driver still owns resources, including while probing or cleaning up.
    bool startup_failed; ///< 当前启动的终止失败，独立于能力/显示状态。/ Current startup failed, independent of capability/UI state.
    cellular_service_state_t state;
    int signal_strength;
    char detail[96];
} cellular_service_status_t;

void cellular_service_init(void);
/// 由承载控制门调用；allow_start 只影响后台启动，不阻止清理。/ Called under the bearer gate; allow_start gates background startup, not cleanup.
void cellular_service_poll(bool allow_start);
/// 仅为替代承载交还无数据链路的终止会话，保留用户设置。/ Yield only a terminal data-less session, retaining preferences.
bool cellular_service_yield_idle(void);
esp_err_t cellular_service_set_enabled(bool enabled);
/// 仅供持有承载控制门的临时业务会话，不改用户开关。/ Temporary bearer starts/stops under the bearer gate without changing user settings.
esp_err_t cellular_service_start_temporary(void);
bool cellular_service_stop_temporary(void);
/// 成功异步启动会接管外层代次。/ Successful asynchronous startup takes the outer token.
esp_err_t cellular_service_enable_admitted(memory_peak_token_t *admitted);
/// Stop the modem before board_ioe_sleep without changing the user's setting.
bool cellular_service_pause_for_sleep(void);
/// Restore the modem/probe after board_ioe_wake; keeps the user's setting.
void cellular_service_resume_after_sleep(void);
void cellular_service_get_status(cellular_service_status_t *out);
bool cellular_service_capable(void);
bool cellular_service_enabled(void);
bool cellular_service_ready(void);

/// Return the modem-created Ethernet netif while the service is running.
/// The pointer is owned by the service and becomes invalid after it is stopped.
void *cellular_service_netif(void);

#ifdef __cplusplus
}
#endif
