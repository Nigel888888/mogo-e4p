/*
 * 中文：按完整放电区间积分、重复样本和局部端点约束学习电量曲线。
 * English: Learn battery curves from complete discharge integrals, repeated samples and fixed local endpoints.
 * 冻结：不拟合显示百分比、不外推采样空白、不修改内阻、满电或欠压边界。
 * Frozen: Never fit displayed SOC, extrapolate gaps or alter resistance, full-charge or undervoltage boundaries.
 */
#include "battery_calibration.h"

#include <limits.h>
#include <string.h>

#define CAL_VERSION 1u
#define CAL_MAX_UAH 20000000u
#define CAL_MIN_UAH 500u
#define CAL_REBOUND_MV 10

uint32_t batt_cal_checksum(const void *data, size_t size) {
    const uint8_t *bytes = data;
    uint32_t value = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        value ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
    }
    return ~value;
}

static void interrupt(batt_cal_t *s) {
    s->have_previous = false;
    s->active_bin = -1;
    s->integral = s->start_integral = 0;
}

void batt_cal_init(batt_cal_t *s, const int32_t base[BATT_CAL_NODES][2], int64_t now) {
    memset(s, 0, sizeof(*s));
    s->base = base;
    s->slot = -1;
    s->active_bin = -1;
    s->settle_until_us = now + BATT_CAL_SETTLE_US;
    s->last_save_us = now;
    s->record.version = CAL_VERSION;
    s->record.baseline = batt_cal_checksum(base, sizeof(int32_t) * BATT_CAL_NODES * 2);
    for (int i = 0; i < BATT_CAL_NODES; ++i) s->record.soc[i] = (uint32_t)base[i][1];
}

void batt_cal_enable(batt_cal_t *s, bool enabled, int64_t now) {
    if (s->record.enabled == (uint32_t)enabled) return;
    s->record.enabled = enabled;
    s->dirty = true;
    interrupt(s);
    s->settle_until_us = now + BATT_CAL_SETTLE_US;
}

void batt_cal_lock(batt_cal_t *s, bool locked, int64_t now) {
    s->locked = locked;
    interrupt(s);
    s->settle_until_us = now + BATT_CAL_SETTLE_US;
}

// 所有已存样本共同满足20%一致性；不从分歧样本中挑选有利子集。
// All stored samples must agree within 20%; never cherry-pick a favorable subset.
static uint32_t trusted(const batt_cal_record_t *r, int bin) {
    const unsigned count = r->count[bin];
    if (count < 3) return 0;
    uint32_t values[BATT_CAL_HISTORY];
    memcpy(values, r->samples[bin], count * sizeof(*values));
    for (unsigned i = 1; i < count; ++i) {
        uint32_t value = values[i];
        unsigned j = i;
        while (j && values[j - 1] > value) { values[j] = values[j - 1]; --j; }
        values[j] = value;
    }
    const uint32_t median = (values[(count - 1) / 2] + values[count / 2]) / 2;
    return (uint64_t)(values[count - 1] - values[0]) * 5 <= median ? median : 0;
}

static void adapt(batt_cal_t *s, uint32_t changed) {
    uint32_t target[BATT_CAL_NODES];
    memcpy(target, s->record.soc, sizeof(target));
    for (int first = 0; first < BATT_CAL_BINS;) {
        if (!trusted(&s->record, first)) { ++first; continue; }
        int end = first;
        uint32_t mask = 0;
        uint64_t total = 0;
        while (end < BATT_CAL_BINS && trusted(&s->record, end)) {
            total += trusted(&s->record, end);
            mask |= 1u << end++;
        }
        if (end - first >= 2 && s->base[first][0] - s->base[end][0] >= 100 && (changed & mask)) {
            const uint32_t top = s->record.soc[first];
            const uint32_t span = top - s->record.soc[end];
            uint64_t passed = 0;
            for (int i = first + 1; i < end; ++i) {
                passed += trusted(&s->record, i - 1);
                target[i] = top - (uint32_t)((span * passed + total / 2) / total);
            }
        }
        first = end;
    }
    int max_delta = 0;
    for (int i = 0; i < BATT_CAL_NODES; ++i) {
        int delta = (int)target[i] - (int)s->record.soc[i];
        if (delta < 0) delta = -delta;
        if (delta > max_delta) max_delta = delta;
    }
    // 整条曲线共用混合系数；凸组合保留单调性，单批最多2个百分点且不超过候选距离的1/4。
    // One blend factor preserves monotonicity, caps each batch at two percentage points and approaches by at most one quarter.
    const int denominator = max_delta > 80 ? max_delta : 80;
    for (int i = 0; i < BATT_CAL_NODES; ++i) {
        int delta = (int)target[i] - (int)s->record.soc[i];
        s->record.soc[i] = (uint32_t)((int)s->record.soc[i] + delta * 20 / denominator);
    }
}

void batt_cal_observe(batt_cal_t *s, int64_t now, int mv, int ma, bool valid, bool power) {
    if (s->reset_state != BATT_CAL_RESET_NONE) return;
    if (valid) {
        if (!s->have_power || power != s->last_power) {
            interrupt(s);
            s->settle_until_us = now + BATT_CAL_SETTLE_US;
        }
        s->have_power = true;
        s->last_power = power;
        if (power && ma > 20 && s->record.seen) {
            s->record.seen = 0;
            s->dirty = true;
        }
    }
    if (!s->record.enabled || s->locked || !valid || power || ma >= 0 || ma < -2000 ||
        mv < 2500 || mv > 5000) {
        interrupt(s);
        return;
    }
    if (s->have_previous && (now <= s->previous_us || now - s->previous_us > BATT_CAL_MAX_GAP_US)) {
        interrupt(s);
        s->settle_until_us = now + BATT_CAL_SETTLE_US;
    }
    if (now < s->settle_until_us) return;
    uint32_t changed = 0;
    if (s->have_previous) {
        const int64_t step = -(int64_t)(s->previous_ma + ma) * (now - s->previous_us) / 2;
        // 极长清醒会话也不允许积分整数溢出；弃掉当前区间，后续重新起步。
        // Discard the current traversal rather than overflow after an unusually long awake session.
        if (s->integral > INT64_MAX - step) { interrupt(s); return; }
        const int64_t before = s->integral;
        s->integral += step;
        if (s->active_bin >= 0) {
            if (mv > s->minimum_mv + CAL_REBOUND_MV) s->active_bin = -1;
            else if (mv < s->minimum_mv) s->minimum_mv = mv;
        }
        if (mv < s->previous_mv) {
            for (int node = 0; node < BATT_CAL_NODES; ++node) {
                const int threshold = s->base[node][0];
                if (s->previous_mv < threshold || mv >= threshold) continue;
                const int64_t at = before + step * (s->previous_mv - threshold) / (s->previous_mv - mv);
                if (s->active_bin == node - 1 && node > 0 && !(s->record.seen & (1u << (node - 1)))) {
                    const int bin = node - 1;
                    const int64_t uah = (at - s->start_integral) / 3600000;
                    if (uah >= CAL_MIN_UAH && uah <= CAL_MAX_UAH) {
                        s->record.samples[bin][s->record.next[bin]] = (uint32_t)uah;
                        s->record.next[bin] = (s->record.next[bin] + 1) % BATT_CAL_HISTORY;
                        if (s->record.count[bin] < BATT_CAL_HISTORY) ++s->record.count[bin];
                        s->record.seen |= 1u << bin;
                        changed |= 1u << bin;
                        s->dirty = true;
                    }
                }
                s->active_bin = node < BATT_CAL_BINS && !(s->record.seen & (1u << node)) ? node : -1;
                s->start_integral = at;
                s->minimum_mv = mv;
            }
        }
    }
    s->previous_us = now;
    s->previous_mv = mv;
    s->previous_ma = ma;
    s->have_previous = true;
    if (changed) adapt(s, changed);
}

int32_t batt_cal_soc(const batt_cal_t *s, int32_t mv) {
    if (mv >= s->base[0][0]) return (int32_t)s->record.soc[0];
    for (int i = 1; i < BATT_CAL_NODES; ++i) {
        if (mv >= s->base[i][0]) {
            const int32_t span = s->base[i - 1][0] - s->base[i][0];
            const int32_t rise = (int32_t)s->record.soc[i - 1] - (int32_t)s->record.soc[i];
            return (int32_t)s->record.soc[i] + (mv - s->base[i][0]) * rise / span;
        }
    }
    return (int32_t)s->record.soc[BATT_CAL_NODES - 1];
}

uint32_t batt_cal_model_id(const batt_cal_t *s) {
    return batt_cal_checksum(s->record.soc, sizeof(s->record.soc)) ^ s->record.baseline;
}

void batt_cal_status(const batt_cal_t *s, batt_cal_status_t *out) {
    *out = (batt_cal_status_t){.enabled = s->record.enabled != 0, .save_error = s->save_error,
        .total_bins = BATT_CAL_BINS};
    if (!s->base) return;
    for (int i = 0; i < BATT_CAL_BINS; ++i) {
        out->sampled_bins += s->record.count[i] > 0;
        out->trusted_bins += trusted(&s->record, i) > 0;
    }
    for (int i = 0; i < BATT_CAL_NODES; ++i)
        out->applied |= s->record.soc[i] != (uint32_t)s->base[i][1];
}

void batt_cal_reset(batt_cal_t *s, int64_t now) {
    const bool enabled = s->record.enabled != 0, locked = s->locked;
    const uint32_t sequence = s->record.sequence;
    const int slot = s->slot;
    batt_cal_init(s, s->base, now);
    s->record.enabled = enabled;
    s->record.sequence = sequence;
    s->locked = locked;
    s->slot = slot;
    s->dirty = true;
}

static bool valid_record(const batt_cal_t *s, const batt_cal_record_t *r) {
    if (r->version != CAL_VERSION || r->baseline != s->record.baseline || r->enabled > 1 ||
        r->seen >> BATT_CAL_BINS || r->checksum != batt_cal_checksum(r, offsetof(batt_cal_record_t, checksum)) ||
        r->soc[0] != (uint32_t)s->base[0][1] ||
        r->soc[BATT_CAL_NODES - 1] != (uint32_t)s->base[BATT_CAL_NODES - 1][1]) return false;
    for (int i = 0; i < BATT_CAL_NODES; ++i)
        if (r->soc[i] > 1000 || (i && r->soc[i] > r->soc[i - 1])) return false;
    for (int i = 0; i < BATT_CAL_BINS; ++i) {
        if (r->count[i] > BATT_CAL_HISTORY || r->next[i] >= BATT_CAL_HISTORY ||
            (r->count[i] < BATT_CAL_HISTORY && r->next[i] != r->count[i])) return false;
        for (unsigned j = 0; j < BATT_CAL_HISTORY; ++j)
            if (j < r->count[i] ? r->samples[i][j] < CAL_MIN_UAH || r->samples[i][j] > CAL_MAX_UAH : r->samples[i][j] != 0)
                return false;
    }
    return true;
}

bool batt_cal_load(batt_cal_t *s, const batt_cal_storage_t *storage) {
    const batt_cal_reset_state_t reset = storage->read_reset(storage->ctx);
    if (reset == BATT_CAL_RESET_UNREADABLE) {
        s->reset_state = reset;
        s->save_error = true;
        return false;
    }
    batt_cal_record_t a, b;
    const bool va = storage->read(storage->ctx, 0, &a) && valid_record(s, &a);
    const bool vb = storage->read(storage->ctx, 1, &b) && valid_record(s, &b);
    if (va || vb) {
        s->slot = vb && (!va || (int32_t)(b.sequence - a.sequence) > 0) ? 1 : 0;
        s->record = s->slot ? b : a;
    }
    if (reset != BATT_CAL_RESET_NONE) {
        // 槽只用来延续序号；存在重置意图时不能恢复旧模型。
        // Use the slots only to continue the sequence; a reset intent bars restoring the old model.
        batt_cal_reset(s, s->last_save_us);
        s->record.enabled = reset == BATT_CAL_RESET_ON;
        s->reset_state = reset;
        s->save_error = true;
        return true;
    }
    if (!va && !vb) return false;
    s->dirty = false;
    return true;
}

static bool save_slot(batt_cal_t *s, const batt_cal_storage_t *storage) {
    batt_cal_record_t record = s->record;
    ++record.sequence;
    record.checksum = batt_cal_checksum(&record, offsetof(batt_cal_record_t, checksum));
    const int slot = s->slot == 0 ? 1 : 0;
    if (!storage->write(storage->ctx, (unsigned)slot, &record)) return false;
    s->record = record;
    s->slot = slot;
    return true;
}

bool batt_cal_save(batt_cal_t *s, const batt_cal_storage_t *storage, int64_t now, bool force) {
    if (!s->dirty || (!force && now >= s->last_save_us && now - s->last_save_us < BATT_CAL_SAVE_US))
        return !s->save_error;
    s->last_save_us = now;
    if (s->reset_state == BATT_CAL_RESET_NONE) {
        if (!save_slot(s, storage)) goto fail;
    } else {
        if (s->reset_state == BATT_CAL_RESET_UNREADABLE) goto fail;
        for (; s->reset_copies < 2; ++s->reset_copies)
            if (!save_slot(s, storage)) goto fail;
        if (!storage->write_reset(storage->ctx, BATT_CAL_RESET_NONE)) goto fail;
        s->reset_state = BATT_CAL_RESET_NONE;
        s->reset_copies = 0;
    }
    s->dirty = s->save_error = false;
    return true;
fail:
    s->save_error = true;
    return false;
}

bool batt_cal_reset_persist(batt_cal_t *s, const batt_cal_storage_t *storage, int64_t now) {
    if (s->reset_state == BATT_CAL_RESET_NONE || s->reset_state == BATT_CAL_RESET_UNREADABLE) {
        const batt_cal_reset_state_t reset = s->record.enabled ? BATT_CAL_RESET_ON : BATT_CAL_RESET_OFF;
        if (!storage->write_reset(storage->ctx, reset)) {
            s->save_error = true;
            return false;
        }
        batt_cal_reset(s, now);
        s->reset_state = reset;
    }
    return batt_cal_save(s, storage, now, true);
}
