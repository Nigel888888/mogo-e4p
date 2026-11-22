/*
 * 中文：独立关机图片的后台准备、双槽TF缓存与只读载入；不使用字库或推屏。
 * English: Prepare independent shutdown art in a worker and load its dual-slot SD cache; no fonts or display updates.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct app_shutdown_wallpaper app_shutdown_wallpaper_t;

#define SHUTDOWN_ART_WIDTH 684u
#define SHUTDOWN_ART_HEIGHT 1216u
#define SHUTDOWN_ART_BYTES (SHUTDOWN_ART_WIDTH * SHUTDOWN_ART_HEIGHT / 2u)

/// 重建时写入非活动TF槽；否则只载入缓存小图，调用不做IO。/ Rebuild only the inactive SD slot, or load a saved thumbnail; this call performs no I/O.
void shutdown_wallpaper_request(const app_shutdown_wallpaper_t *value, bool rebuild);
/// 取消过期请求，不强杀持有文件句柄的任务。/ Cancel stale work without killing a task that owns file handles.
void shutdown_wallpaper_cancel(void);
/// 接收一次结果；小图由调用方用mm_free释放，成功后仍需单独提交设置。/ Take one result; release the thumbnail with mm_free, and separately commit successful settings.
bool shutdown_wallpaper_take(app_shutdown_wallpaper_t *value, bool *success, uint8_t **thumbnail);
/// 最近一次接收结果的失败原因；静态字符串，无文件访问。/ Failure reason for the last taken result; static text without file access.
const char *shutdown_wallpaper_error(void);
/// 等待文件句柄交还，超时不继续USB/睡眠交接。/ Wait for file ownership before USB/sleep handoff; timeout blocks the handoff.
bool shutdown_wallpaper_quiesce(unsigned timeout_ms);
/// 从TF缓存读取标准竖屏4bpp画布；调用方在绘图前使用，失败时不得显示部分结果。
/// Read a canonical portrait 4bpp canvas before drawing; never display a partial result on failure.
bool shutdown_wallpaper_read(const app_shutdown_wallpaper_t *value, uint8_t *pixels);
