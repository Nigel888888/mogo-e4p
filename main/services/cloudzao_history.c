/*
 * 中文：固定长度环形记录，每槽独立校验；读取时忽略坏槽。
 * English: Fixed-size ring with independent per-slot checks; skip invalid slots on read.
 */
#include "cloudzao_history.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HISTORY_MAGIC UINT32_C(0x315a4843)

typedef struct {
    uint32_t magic, sequence, utc, size;
    uint8_t state;
    char name[CLOUDZAO_NAME_MAX + 1];
    uint8_t reserved[2];
    uint32_t check;
} slot_t;
_Static_assert(sizeof(slot_t) == 144, "CloudZao history v1 slot size");

static uint32_t checksum(const slot_t *slot) {
    const uint8_t *data = (const uint8_t *)slot;
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < offsetof(slot_t, check); ++i) hash = (hash ^ data[i]) * UINT32_C(16777619);
    return hash;
}

static bool valid_slot(const slot_t *slot, unsigned index) {
    return slot->magic == HISTORY_MAGIC && slot->sequence &&
        slot->sequence % CLOUDZAO_HISTORY_ROWS == index && slot->state >= CZ_ROW_SAVED &&
        slot->state <= CZ_ROW_FAILED && slot->check == checksum(slot) &&
        slot->name[CLOUDZAO_NAME_MAX] == 0 && cloudzao_name_valid(slot->name);
}

static bool history_path(char *path, size_t capacity, const char *root) {
    const int length = snprintf(path, capacity, "%s/.readpico/cloudzao/history.v1", root);
    return length >= 0 && (size_t)length < capacity;
}

static int history_open(const char *root, bool writing) {
    char path[320];
    if (!history_path(path, sizeof(path), root)) {
        errno = ENAMETOOLONG; return -1;
    }
    if (writing) {
        char dir[320];
        snprintf(dir, sizeof(dir), "%s/.readpico", root);
        if (mkdir(dir, 0775) && errno != EEXIST) return -1;
        snprintf(dir, sizeof(dir), "%s/.readpico/cloudzao", root);
        if (mkdir(dir, 0775) && errno != EEXIST) return -1;
    }
    int fd = open(path, writing ? O_RDWR | O_CREAT : O_RDONLY, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > (off_t)(CLOUDZAO_HISTORY_ROWS * sizeof(slot_t))) {
        close(fd); errno = EIO; return -1;
    }
    return fd;
}

static bool read_slot(int fd, unsigned index, slot_t *slot) {
    const off_t offset = (off_t)index * sizeof(*slot);
    struct stat st;
    return !fstat(fd, &st) && st.st_size >= offset + (off_t)sizeof(*slot) &&
           lseek(fd, offset, SEEK_SET) == offset && read(fd, slot, sizeof(*slot)) == sizeof(*slot) &&
           valid_slot(slot, index);
}

cz_error_t cloudzao_history_read(const cz_ops_t *ops, cz_history_item_t *items, unsigned *count) {
    *count = 0;
    cz_error_t error = ops->stop(ops->context);
    if (error) return error;
    int fd = history_open(ops->root, false);
    if (fd < 0) return errno == ENOENT ? CZ_OK : CZ_STORAGE;
    uint32_t order[CLOUDZAO_HISTORY_ROWS];
    for (unsigned i = 0; i < CLOUDZAO_HISTORY_ROWS; ++i) {
        if ((error = ops->stop(ops->context)) != CZ_OK) break;
        slot_t slot;
        if (!read_slot(fd, i, &slot)) continue;
        unsigned place = *count;
        while (place && order[place - 1] < slot.sequence) {
            order[place] = order[place - 1]; items[place] = items[place - 1]; --place;
        }
        order[place] = slot.sequence;
        items[place] = (cz_history_item_t){.utc = slot.utc, .file = {.size = slot.size, .state = slot.state}};
        memcpy(items[place].file.name, slot.name, sizeof(slot.name));
        ++*count;
    }
    close(fd);
    if (error) *count = 0;
    return error;
}

cz_error_t cloudzao_history_clear(const cz_ops_t *ops) {
    cz_error_t error = ops->stop(ops->context);
    if (error != CZ_OK) return error;
    char path[320];
    if (!history_path(path, sizeof(path), ops->root)) return CZ_STORAGE;
    struct stat file;
    if (stat(path, &file)) return errno == ENOENT ? CZ_OK : CZ_STORAGE;
    if (!S_ISREG(file.st_mode)) return CZ_STORAGE;
    error = ops->stop(ops->context);
    if (error != CZ_OK) return error;
    return !unlink(path) || errno == ENOENT ? CZ_OK : CZ_STORAGE;
}

bool cloudzao_history_record(const cz_ops_t *ops, uint32_t *next, const cz_row_t *row, uint32_t utc) {
    if (row->state < CZ_ROW_SAVED || row->state > CZ_ROW_FAILED || !cloudzao_name_valid(row->name) ||
        ops->stop(ops->context) != CZ_OK) return false;
    int fd = history_open(ops->root, true);
    if (fd < 0) return false;
    bool ok = false;
    if (!*next) {
        uint32_t latest = 0;
        for (unsigned i = 0; i < CLOUDZAO_HISTORY_ROWS; ++i) {
            if (ops->stop(ops->context) != CZ_OK) goto done;
            slot_t previous;
            if (read_slot(fd, i, &previous) && previous.sequence > latest) latest = previous.sequence;
        }
        *next = latest + 1;
    }
    if (!*next || *next == UINT32_MAX || ops->stop(ops->context) != CZ_OK) goto done;
    slot_t slot = {.magic = HISTORY_MAGIC, .sequence = *next, .utc = utc, .size = row->size, .state = row->state};
    memcpy(slot.name, row->name, strlen(row->name) + 1);
    slot.check = checksum(&slot);
    const off_t offset = (off_t)(*next % CLOUDZAO_HISTORY_ROWS) * sizeof(slot);
    if (lseek(fd, offset, SEEK_SET) == offset && ops->stop(ops->context) == CZ_OK &&
        write(fd, &slot, sizeof(slot)) == sizeof(slot) && !fsync(fd)) {
        ++*next; ok = true;
    }
done:
    if (close(fd)) ok = false;
    return ok;
}
