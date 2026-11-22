/*
 * 中文：CloudZao 官方登录页使用的 App 下载链接二维码。
 * English: QR code for the app download URL used on the official CloudZao login page.
 * 载荷 / Payload: https://yun.cloudzao.com/download
 * 由锁定的 qrcodegen 生成（纠错 M、版本 1～3、自动掩码、不提高纠错级别）。
 * Generated with the locked qrcodegen (ECC M, versions 1–3, auto mask, no ECC boost).
 * 每行最低位在左，1 为黑；留白在绘制时保留。/ LSB is leftmost, 1 is black; drawing supplies the quiet zone.
 */
#pragma once
#include <stdint.h>

#define CLOUDZAO_DOWNLOAD_QR_SIDE 29
static const uint32_t cloudzao_download_qr_rows[CLOUDZAO_DOWNLOAD_QR_SIDE] = {
    0x1fc4ef7f, 0x10569041, 0x17522b5d, 0x1747925d, 0x1745945d,
    0x10457941, 0x1fd5557f, 0x001c3400, 0x1495a0c5, 0x18cb042e,
    0x16e979dd, 0x03fdc383, 0x10d16cd1, 0x185873b2, 0x108b95cf,
    0x0181348e, 0x10d9b1f5, 0x1cdb949a, 0x1318e377, 0x00ec4114,
    0x0bf1f273, 0x171de700, 0x1150977f, 0x091dbc41, 0x1bf9365d,
    0x17a3865d, 0x1911cf5d, 0x02b11241, 0x10d1457f,
};
