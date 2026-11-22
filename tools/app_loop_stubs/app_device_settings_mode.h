/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

void app_device_settings_request_bluetooth(bool return_to_origin);
void app_device_settings_request_haptics(bool return_to_origin);
void app_device_settings_request_memory(bool return_to_origin);
bool app_device_settings_navigation_busy(void);
