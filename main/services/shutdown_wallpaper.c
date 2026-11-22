/*
 * 中文：关机图片采用独立TF双槽；未完成、取消和保存失败不替换当前选择。
 * English: Independent dual-slot SD shutdown art; incomplete, cancelled or failed saves never replace the selected image.
 * 用户2026-11-17修订：图片仅缓存在TF卡，不再写入或读取机内缓存。
 * User revision on 2026-11-17: cache images only on SD, without internal-cache reads or writes.
 * 冻结：后台不绘屏、不调用字库、不提交设置；无卡或缓存失效时由调用方回退系统图。
 * Frozen: Workers never paint, use fonts or commit settings; callers use built-in art without usable card caches.
 */
#include "shutdown_wallpaper.h"
#include "settings.h"
#include "image_decode.h"
#include "ui_image_dither.h"
#include "memory_manager.h"
#include "board_sd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SHUTDOWN_ART_ROOT
#define SHUTDOWN_ART_ROOT "/sdcard/.readpico"
#endif
#define ART_CACHE_DIR SHUTDOWN_ART_ROOT "/shutdown"
#define ART_MAGIC_LEGACY UINT32_C(0x4f464631)
#define ART_MAGIC_BASE_TONE UINT32_C(0x4f464632)
#define ART_MAGIC UINT32_C(0x4f464633)
#define THUMB_WIDTH 160u
#define THUMB_HEIGHT 284u
#define THUMB_BYTES (THUMB_WIDTH * THUMB_HEIGHT / 2u)
typedef struct {
    uint32_t magic, width, height, fit, image_id, checksum;
} off_header_t;
static portMUX_TYPE s_art_lock = portMUX_INITIALIZER_UNLOCKED;
static app_shutdown_wallpaper_t s_request, s_result;
static unsigned s_generation;
static bool s_pending, s_rebuild, s_running, s_done, s_success;
static uint8_t *s_thumbnail;
static const char *s_result_error, *s_taken_error;

static bool art_storage_ready(void) {
    board_sd_info_t info = {0};
    board_sd_get_info(&info);
    return info.present && info.mounted;
}
static bool art_ensure_dir(const char *path) {
    if (mkdir(path, 0777) == 0) return true;
    struct stat st;
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static uint32_t art_hash(uint32_t hash, const void *bytes, size_t count) {
    const uint8_t *p = bytes;
    while (count--) hash = (hash ^ *p++) * 16777619u;
    return hash;
}
static bool art_cancelled(void *context) {
    portENTER_CRITICAL(&s_art_lock);
    bool cancelled = *(unsigned *)context != s_generation;
    portEXIT_CRITICAL(&s_art_lock);
    return cancelled;
}
static void art_path(char path[128], unsigned slot) {
    snprintf(path, 128, ART_CACHE_DIR "/%u.bin", slot);
}
// 旧缓存可能是唯一副本；校验后只调整内存画布，不重写活动槽。
// A legacy cache may be the only copy; tone the validated canvas in memory without rewriting the active slot.
static void art_tone_legacy(uint8_t *pixels, bool base_toned) {
    uint8_t tones[16];
    unsigned source = 0;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned level = i * 17u;
        if (base_toned) {
            // 用单调的初版曲线估计原亮度，只补偿新增的浅灰调整，不重复压暗中间调。
            // Estimate original luminance from the monotone first curve; apply only the new highlight correction.
            while (source < UINT8_MAX && ui_image_wallpaper_base_tone(source) < level) ++source;
            tones[i] = (uint8_t)(level - ui_image_wallpaper_highlight_delta(source));
        } else {
            tones[i] = ui_image_wallpaper_tone(level);
        }
    }
    for (unsigned y = 0; y < SHUTDOWN_ART_HEIGHT; ++y) {
        for (unsigned x = 0; x < SHUTDOWN_ART_WIDTH; x += 2) {
            size_t at = ((size_t)y * SHUTDOWN_ART_WIDTH + x) / 2u;
            unsigned pair = pixels[at];
            unsigned lo = ui_image_dither_gray(tones[pair & 15u], x, y) / 17u;
            unsigned hi = ui_image_dither_gray(tones[pair >> 4], x + 1, y) / 17u;
            pixels[at] = (uint8_t)(lo | (hi << 4));
        }
    }
}
bool shutdown_wallpaper_read(const app_shutdown_wallpaper_t *value, uint8_t *pixels) {
    if (!pixels || !app_settings_shutdown_wallpaper_valid(value) || !value->image_id) return false;
    if (!art_storage_ready()) return false;
    char path[128]; art_path(path, value->slot);
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    off_header_t header;
    bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) &&
        (header.magic == ART_MAGIC || header.magic == ART_MAGIC_BASE_TONE || header.magic == ART_MAGIC_LEGACY) &&
        header.width == SHUTDOWN_ART_WIDTH && header.height == SHUTDOWN_ART_HEIGHT &&
        header.fit == value->fit && header.image_id == value->image_id &&
        fread(pixels, 1, SHUTDOWN_ART_BYTES, file) == SHUTDOWN_ART_BYTES && fgetc(file) == EOF &&
        !ferror(file) && header.checksum == art_hash(2166136261u, pixels, SHUTDOWN_ART_BYTES);
    fclose(file);
    if (ok && header.magic != ART_MAGIC) art_tone_legacy(pixels, header.magic == ART_MAGIC_BASE_TONE);
    return ok;
}
static bool art_write(app_shutdown_wallpaper_t *value, const uint8_t *pixels, unsigned generation, const char **error) {
    off_header_t header = {.magic = ART_MAGIC, .width = SHUTDOWN_ART_WIDTH,
        .height = SHUTDOWN_ART_HEIGHT, .fit = value->fit,
        .checksum = art_hash(2166136261u, pixels, SHUTDOWN_ART_BYTES)};
    header.image_id = art_hash(header.checksum, value->path, strlen(value->path) + 1);
    header.image_id = art_hash(header.image_id, &value->fit, sizeof(value->fit));
    if (!header.image_id) header.image_id = 1;
    char path[128]; art_path(path, value->slot);
    *error = "TF卡未就绪";
    if (art_cancelled(&generation) || !art_storage_ready()) return false;
    if (!art_ensure_dir(SHUTDOWN_ART_ROOT) || !art_ensure_dir(ART_CACHE_DIR)) {
        *error = errno == ENOSPC ? "TF卡空间不足" : "TF壁纸目录不可用";
        return false;
    }
    // 仅覆盖非活动槽，关闭并同步完整载荷后才把身份交给UI提交。
    // Overwrite only the inactive slot; close and sync the entire payload before offering its identity to the UI.
    FILE *file = fopen(path, "wb");
    bool ok = file && fwrite(&header, 1, sizeof(header), file) == sizeof(header);
    for (size_t offset = 0; ok && offset < SHUTDOWN_ART_BYTES; offset += 4096u) {
        size_t count = SHUTDOWN_ART_BYTES - offset;
        if (count > 4096u) count = 4096u;
        ok = !art_cancelled(&generation) && fwrite(pixels + offset, 1, count, file) == count;
    }
    if (!ok) *error = errno == ENOSPC ? "TF卡空间不足" : "TF图片写入失败";
    if (ok) {
        ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
        if (!ok) *error = errno == ENOSPC ? "TF卡空间不足" : "TF图片同步失败";
    }
    if (ok) *error = "TF图片关闭失败";
    if (file && fclose(file)) ok = false;
    if (ok && !art_cancelled(&generation)) value->image_id = header.image_id;
    else ok = false;
    return ok;
}
static bool art_decode(app_shutdown_wallpaper_t *value, uint8_t *pixels, unsigned generation) {
    const char *ext = strrchr(value->path, '.');
    if (strncmp(value->path, "/sdcard/", 8) || !ext ||
        (strcasecmp(ext, ".png") && strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg"))) return false;
    struct stat before, after;
    if (stat(value->path, &before) || !S_ISREG(before.st_mode) || before.st_size <= 0 ||
        (uint64_t)before.st_size > BOOK_IMAGE_FILE_MAX || art_cancelled(&generation)) return false;
    unsigned width = 0, height = 0;
    bool png = !strcasecmp(ext, ".png");
    if (!book_image_file_dimensions(value->path, png, &width, &height) || !width || !height) return false;
    unsigned source_w = width, source_h = height;
    if (width >= height) { width = 1216; height = (unsigned)((uint64_t)source_h * width / source_w); }
    else { height = 1216; width = (unsigned)((uint64_t)source_w * height / source_h); }
    if (!width) width = 1;
    if (!height) height = 1;
    while ((uint64_t)width * height > SHUTDOWN_ART_WIDTH * SHUTDOWN_ART_HEIGHT) {
        width = (width * 3u + 3u) / 4u; height = (height * 3u + 3u) / 4u;
    }
    uint8_t *gray = mm_alloc(MM_OWNER_FILE_JOB, MM_POOL_PSRAM, (size_t)width * height, 0, 0);
    if (!gray) return false;
    bool ok = book_image_file_grayscale_cancel(value->path, png, width, height, gray, art_cancelled, &generation);
    book_crop_t crop = book_cover_crop(width, height, SHUTDOWN_ART_WIDTH, SHUTDOWN_ART_HEIGHT);
    unsigned draw_w = SHUTDOWN_ART_WIDTH, draw_h = SHUTDOWN_ART_HEIGHT;
    if (value->fit) {
        crop = (book_crop_t){0, 0, width, height};
        if ((uint64_t)width * draw_h > (uint64_t)height * draw_w) draw_h = (unsigned)((uint64_t)height * draw_w / width);
        else draw_w = (unsigned)((uint64_t)width * draw_h / height);
        if (!draw_w) draw_w = 1;
        if (!draw_h) draw_h = 1;
    }
    memset(pixels, 255, SHUTDOWN_ART_BYTES);
    unsigned dx = (SHUTDOWN_ART_WIDTH - draw_w) / 2u, dy = (SHUTDOWN_ART_HEIGHT - draw_h) / 2u;
    for (unsigned y = 0; ok && y < draw_h; ++y) {
        if (art_cancelled(&generation)) { ok = false; break; }
        unsigned sy = crop.y + (unsigned)((uint64_t)y * crop.height / draw_h);
        for (unsigned x = 0; x < draw_w; ++x) {
            unsigned sx = crop.x + (unsigned)((uint64_t)x * crop.width / draw_w);
            uint8_t tone = ui_image_wallpaper_tone(gray[(size_t)sy * width + sx]);
            unsigned gray4 = ui_image_dither_gray(tone, dx + x, dy + y) / 17u;
            size_t at = (size_t)(dy + y) * SHUTDOWN_ART_WIDTH + dx + x;
            unsigned shift = (at & 1u) * 4u;
            pixels[at / 2] = (uint8_t)((pixels[at / 2] & ~(15u << shift)) | (gray4 << shift));
        }
        if (y % 32u == 0) vTaskDelay(1);
    }
    mm_free(gray);
    return ok && !art_cancelled(&generation) && stat(value->path, &after) == 0 &&
        before.st_size == after.st_size && before.st_mtime == after.st_mtime;
}
static uint8_t *art_thumbnail(const uint8_t *pixels) {
    uint8_t *thumb = mm_alloc(MM_OWNER_UI_CACHE, MM_POOL_PSRAM, THUMB_BYTES, 0, 0);
    if (!thumb) return NULL;
    // 先平均源图覆盖范围，再抖动到缩图灰阶，避免隔点取样放大抖动条纹。
    // Average each covered source box before thumbnail dithering to avoid aliasing the full-size dither.
    for (unsigned y = 0; y < THUMB_HEIGHT; ++y) {
        unsigned top = y * SHUTDOWN_ART_HEIGHT / THUMB_HEIGHT;
        unsigned bottom = (y + 1) * SHUTDOWN_ART_HEIGHT / THUMB_HEIGHT;
        for (unsigned x = 0; x < THUMB_WIDTH; ++x) {
            unsigned left = x * SHUTDOWN_ART_WIDTH / THUMB_WIDTH;
            unsigned right = (x + 1) * SHUTDOWN_ART_WIDTH / THUMB_WIDTH;
            unsigned sum = 0, count = (right - left) * (bottom - top);
            for (unsigned sy = top; sy < bottom; ++sy) {
                for (unsigned sx = left; sx < right; ++sx) {
                    unsigned at = sy * SHUTDOWN_ART_WIDTH + sx;
                    sum += (pixels[at / 2u] >> ((at & 1u) * 4u)) & 15u;
                }
            }
            unsigned gray4 = ui_image_dither_gray((sum * 17u + count / 2u) / count, x, y) / 17u;
            size_t at = y * THUMB_WIDTH + x;
            if (x & 1u) thumb[at / 2u] |= (uint8_t)(gray4 << 4);
            else thumb[at / 2u] = (uint8_t)gray4;
        }
    }
    return thumb;
}
static void art_worker(void *unused) {
    (void)unused;
    for (;;) {
        portENTER_CRITICAL(&s_art_lock);
        if (!s_pending) { s_running = false; portEXIT_CRITICAL(&s_art_lock); break; }
        unsigned generation = s_generation;
        app_shutdown_wallpaper_t value = s_request;
        bool rebuild = s_rebuild; s_pending = false;
        portEXIT_CRITICAL(&s_art_lock);
        const char *error = "TF卡未就绪";
        bool ok = art_storage_ready();
        uint8_t *pixels = ok ? mm_alloc(MM_OWNER_FILE_JOB, MM_POOL_PSRAM, SHUTDOWN_ART_BYTES, 0, 0) : NULL;
        if (ok) {
            error = "可用内存不足";
            ok = pixels != NULL;
        }
        if (ok) {
            error = rebuild ? "图片读取或解码失败" : "TF壁纸不可用";
            ok = rebuild ? art_decode(&value, pixels, generation) : shutdown_wallpaper_read(&value, pixels);
        }
        if (ok && rebuild) ok = art_write(&value, pixels, generation, &error);
        uint8_t *thumbnail = ok ? art_thumbnail(pixels) : NULL;
        mm_free(pixels);
        if (!ok && !art_cancelled(&generation)) ESP_LOGW("off_art", "%s (errno=%d)", error, errno);
        portENTER_CRITICAL(&s_art_lock);
        if (generation == s_generation) {
            s_result = value; s_success = ok; s_done = true;
            s_result_error = ok ? NULL : error;
            s_thumbnail = thumbnail; thumbnail = NULL;
        }
        portEXIT_CRITICAL(&s_art_lock);
        mm_free(thumbnail);
    }
    vTaskDelete(NULL);
}
void shutdown_wallpaper_cancel(void) {
    portENTER_CRITICAL(&s_art_lock);
    ++s_generation; s_pending = false; s_done = false;
    uint8_t *old = s_thumbnail; s_thumbnail = NULL;
    portEXIT_CRITICAL(&s_art_lock);
    mm_free(old);
}
void shutdown_wallpaper_request(const app_shutdown_wallpaper_t *value, bool rebuild) {
    shutdown_wallpaper_cancel();
    portENTER_CRITICAL(&s_art_lock);
    s_request = *value; s_rebuild = rebuild; s_pending = true;
    bool start = !s_running;
    if (start) s_running = true;
    portEXIT_CRITICAL(&s_art_lock);
    if (start && xTaskCreate(art_worker, "off_art", 12288, NULL, 1, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_art_lock);
        s_running = false; s_pending = false; s_result = *value; s_done = true; s_success = false;
        s_result_error = "后台任务创建失败";
        portEXIT_CRITICAL(&s_art_lock);
    }
}
bool shutdown_wallpaper_take(app_shutdown_wallpaper_t *value, bool *success, uint8_t **thumbnail) {
    portENTER_CRITICAL(&s_art_lock);
    bool done = s_done;
    if (done) { *value = s_result; *success = s_success; *thumbnail = s_thumbnail; s_thumbnail = NULL; s_done = false; s_taken_error = s_result_error; }
    portEXIT_CRITICAL(&s_art_lock);
    return done;
}
const char *shutdown_wallpaper_error(void) {
    portENTER_CRITICAL(&s_art_lock);
    const char *error = s_taken_error;
    portEXIT_CRITICAL(&s_art_lock);
    return error ? error : "壁纸保存失败";
}
bool shutdown_wallpaper_quiesce(unsigned timeout_ms) {
    shutdown_wallpaper_cancel();
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        portENTER_CRITICAL(&s_art_lock); bool running = s_running; portEXIT_CRITICAL(&s_art_lock);
        if (!running) return true;
        if (esp_timer_get_time() >= deadline) return false;
        vTaskDelay(1);
    }
}
