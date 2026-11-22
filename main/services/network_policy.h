/* SPDX-License-Identifier: Apache-2.0
 * Shared bearer and business-route policy vocabulary.
 */
#pragma once

#include <stdbool.h>

typedef enum {
    NETWORK_BEARER_NONE = 0,
    NETWORK_BEARER_WIFI,
    NETWORK_BEARER_CELLULAR,
} network_bearer_t;

typedef enum {
    NETWORK_ROUTE_WIFI = 0,
    NETWORK_ROUTE_CELLULAR,
    NETWORK_ROUTE_AUTO,
} network_route_t;

typedef enum {
    NETWORK_AUTO_WIFI_FIRST = 0,
    NETWORK_AUTO_CELLULAR_FIRST,
} network_auto_priority_t;

typedef enum {
    NETWORK_USE_TIME = 0,
    NETWORK_USE_WEATHER,
    NETWORK_USE_OTA,
    NETWORK_USE_WEATHER_MANUAL,
    NETWORK_USE_TIME_MANUAL,
    NETWORK_USE_OTA_MANUAL,
    NETWORK_USE_CLOUDZAO,
    NETWORK_USE_WEATHER_BACKGROUND,
} network_use_t;

/// 仅明确发起的操作可临时启用关闭的 4G。/ Only explicit actions may temporarily start switched-off cellular.
static inline bool network_use_allows_temporary_cellular(network_use_t use) {
    return use == NETWORK_USE_WEATHER_MANUAL || use == NETWORK_USE_TIME_MANUAL || use == NETWORK_USE_OTA_MANUAL;
}

static inline bool network_route_valid(network_route_t route) {
    return route >= NETWORK_ROUTE_WIFI && route <= NETWORK_ROUTE_AUTO;
}

static inline bool network_auto_priority_valid(network_auto_priority_t priority) {
    return priority == NETWORK_AUTO_WIFI_FIRST || priority == NETWORK_AUTO_CELLULAR_FIRST;
}

const char *network_route_name(network_route_t route);
const char *network_auto_priority_name(network_auto_priority_t priority);
