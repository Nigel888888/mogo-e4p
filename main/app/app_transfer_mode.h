#pragma once

// The file manager uses this before opening Transfer to enter USB storage directly.
void app_transfer_request_usb_start(void);

// File manager opens the selected transfer mode directly, without the method picker.
void app_transfer_request_wifi_upload(void);
void app_transfer_request_hotspot_start(void);

// Settings uses this before opening Transfer to enter WiFi provisioning directly.
void app_transfer_request_wifi_setup(void);

// 联调构建从文件管理直达 CloudZao；不自动接收。
// Test builds open CloudZao directly from Files without starting a receive job.
void app_transfer_request_cloudzao(void);
