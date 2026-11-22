/*
 * 中文：真实学习模块的独立曲线回放、生命周期与故障存储回归。
 * English: Independent curve replays, lifecycle and fault-injected storage tests of the real learner.
 */
#include "battery_calibration.h"
#include "baseline.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t now;
static batt_cal_t cal;

static void observe(int mv, int ma, bool valid, bool power) {
    uint32_t before[BATT_CAL_NODES];
    memcpy(before, cal.record.soc, sizeof(before));
    batt_cal_observe(&cal, now, mv, ma, valid, power);
    for (int i = 0; i < BATT_CAL_NODES; ++i) {
        assert(abs((int)cal.record.soc[i] - (int)before[i]) <= 20);
        assert(cal.record.soc[i] <= 1000);
        if (i) assert(cal.record.soc[i] <= cal.record.soc[i - 1]);
    }
    assert(cal.record.soc[0] == 1000 && cal.record.soc[21] == 0);
}

static void start(void) {
    now = 0;
    batt_cal_init(&cal, s_soc_curve, now);
    batt_cal_enable(&cal, true, now);
}

static void discharge_start(void) {
    now += 1000000;
    observe(4150, 100, true, true);
    now += 1000000;
    observe(4101, -100, true, false);
    now += BATT_CAL_SETTLE_US;
    observe(4101, -100, true, false);
}

// 物理夹具以独立给定的各段微安时生成时间/电压，不使用学习输出生成输入。
// The physical fixture generates time/voltage from independent interval charge, never learner output.
static void cycle(const int charge[4]) {
    discharge_start();
    now += 1000000;
    observe(4100, -100, true, false);
    for (int bin = 0; bin < 4; ++bin) {
        const int seconds = charge[bin] * 36 / 1000;
        const int top = s_soc_curve[3 + bin][0], low = s_soc_curve[4 + bin][0];
        for (int t = 1; t <= seconds; ++t) {
            now += 1000000;
            observe(top - (top - low) * t / seconds, -100, true, false);
        }
    }
    now += 1000000;
    observe(3899, -100, true, false);
}

static double error(const int charge[4], const batt_cal_t *model) {
    const double total = charge[0] + charge[1] + charge[2] + charge[3];
    double result = 0, consumed = 0;
    for (int bin = 0; bin < 4; ++bin) {
        for (int mv = s_soc_curve[3 + bin][0] - 3; mv > s_soc_curve[4 + bin][0]; mv -= 7) {
            const double part = (double)(s_soc_curve[3 + bin][0] - mv) /
                                (s_soc_curve[3 + bin][0] - s_soc_curve[4 + bin][0]);
            const double truth = 790 - 222 * (consumed + charge[bin] * part) / total;
            double delta = batt_cal_soc(model, mv) - truth;
            result += delta < 0 ? -delta : delta;
        }
        consumed += charge[bin];
    }
    return result;
}

static void independent_curves(void) {
    const int a[4] = {10000, 20000, 40000, 41000};
    const int b[4] = {40000, 30000, 20000, 21000};
    batt_cal_t baseline, model_a;
    batt_cal_init(&baseline, s_soc_curve, 0);
    start();
    for (int i = 0; i < 10; ++i) cycle(a);
    model_a = cal;
    batt_cal_status_t status;
    batt_cal_status(&cal, &status);
    assert(status.applied && status.sampled_bins == 4 && status.trusted_bins == 4);
    assert(error(a, &cal) < error(a, &baseline) * .5);
    for (int i = 0; i < BATT_CAL_NODES; ++i)
        if (i <= 3 || i >= 7) assert(cal.record.soc[i] == (uint32_t)s_soc_curve[i][1]);
    const uint32_t identity = batt_cal_model_id(&cal);
    batt_cal_enable(&cal, false, now);
    batt_cal_record_t frozen = cal.record;
    cycle(b);
    assert(!memcmp(frozen.soc, cal.record.soc, sizeof(frozen.soc)));
    assert(!memcmp(frozen.samples, cal.record.samples, sizeof(frozen.samples)));
    assert(!memcmp(frozen.count, cal.record.count, sizeof(frozen.count)));
    assert(batt_cal_model_id(&cal) == identity);
    start();
    for (int i = 0; i < 10; ++i) cycle(b);
    assert(error(b, &cal) < error(b, &baseline) * .5);
    assert(cal.record.soc[5] != model_a.record.soc[5]);
    printf("PASS: independent battery A/B held-out curve error %.2f/%.2f -> %.2f/%.2f (aggregate permille)\n",
           error(a, &baseline), error(b, &baseline), error(a, &model_a), error(b, &cal));
}

static void breaks_and_rebounds(void) {
    start();
    discharge_start();
    now += 1000000; observe(4100, -100, true, false);
    now += 1000000; observe(4099, -100, true, false);
    now += 1000000; observe(4080, -100, true, false);
    batt_cal_lock(&cal, true, now);
    for (int i = 0; i < 200; ++i) { now += 1000000; observe(4040, -100, true, false); }
    assert(!cal.record.count[3]);
    batt_cal_lock(&cal, false, now);
    now += 1000000; observe(4055, -100, true, false);
    assert(!cal.have_previous);
    now += BATT_CAL_SETTLE_US; observe(4045, -100, true, false);
    now += 1000000; observe(3990, -100, true, false);
    assert(!cal.record.count[3] && !cal.record.count[4]);

    for (int failure = 0; failure < 4; ++failure) {
        start(); discharge_start();
        now += 1000000; observe(4100, -100, true, false);
        now += 1000000; observe(4070, -100, true, false);
        if (failure == 0) { now += 6000000; observe(4060, -100, true, false); }
        if (failure == 1) { now += 1000000; observe(4060, -100, false, false); }
        if (failure == 2) { now += 1000000; observe(4060, 100, true, true); }
        if (failure == 3) { now -= 500000; observe(4060, -100, true, false); }
        now += 1000000; observe(4040, -100, true, false);
        assert(!cal.record.count[3]);
    }
    start(); discharge_start();
    now += 1000000; observe(4100, -100, true, false);
    for (int mv = 4099; mv >= 4049; --mv) { now += 1000000; observe(mv, -100, true, false); }
    assert(cal.record.count[3] == 1);
    now += 1000000; observe(4110, -100, true, false);
    for (int mv = 4109; mv >= 4049; --mv) { now += 1000000; observe(mv, -100, true, false); }
    assert(cal.record.count[3] == 1);
    now += 1000000; observe(4061, -100, true, false);
    now += 1000000; observe(3999, -100, true, false);
    assert(cal.record.count[4] == 0);
    puts("PASS: settling, lock/wake, charger, invalid samples, gaps, clock rollback and rebound deduplication");
}

static batt_cal_record_t slots[2];
static bool present[2], fail_write;
static unsigned writes;
static bool read_slot(void *ctx, unsigned slot, batt_cal_record_t *record) {
    (void)ctx;
    *record = slots[slot];
    return present[slot];
}
static bool write_slot(void *ctx, unsigned slot, const batt_cal_record_t *record) {
    (void)ctx;
    ++writes;
    if (fail_write) return false;
    slots[slot] = *record;
    present[slot] = true;
    return true;
}

static batt_cal_reset_state_t read_no_reset(void *ctx) {
    (void)ctx;
    return BATT_CAL_RESET_NONE;
}

static void storage_and_reset(void) {
    batt_cal_storage_t io = {.read = read_slot, .write = write_slot, .read_reset = read_no_reset};
    const int charge[4] = {10000, 20000, 40000, 41000};
    start();
    assert(!batt_cal_load(&cal, &io));
    assert(batt_cal_save(&cal, &io, now, false) && !writes);
    assert(batt_cal_save(&cal, &io, now, true) && writes == 1 && cal.slot == 0);
    cycle(charge);
    assert(batt_cal_save(&cal, &io, now, true) && cal.slot == 1);
    batt_cal_t restored;
    batt_cal_init(&restored, s_soc_curve, now);
    assert(batt_cal_load(&restored, &io) && restored.record.count[3] == 1 && !restored.have_previous);
    slots[1].samples[3][0] ^= 1;
    batt_cal_init(&restored, s_soc_curve, now);
    assert(batt_cal_load(&restored, &io) && !restored.record.count[3] && restored.record.enabled);
    slots[0].version += 1;
    batt_cal_init(&restored, s_soc_curve, now);
    assert(!batt_cal_load(&restored, &io) && !restored.record.enabled);
    for (int i = 0; i < 4; ++i) cycle(charge);
    uint32_t model = batt_cal_model_id(&cal);
    const unsigned before = writes;
    cal.last_save_us = now;
    assert(batt_cal_save(&cal, &io, now + 1, false) && writes == before);
    fail_write = true;
    const uint32_t sequence = cal.record.sequence;
    const int slot = cal.slot;
    assert(!batt_cal_save(&cal, &io, now, true));
    assert(cal.dirty && cal.save_error && cal.record.sequence == sequence && cal.slot == slot);
    fail_write = false;
    assert(batt_cal_save(&cal, &io, now + BATT_CAL_SAVE_US, false));
    assert(!cal.dirty && !cal.save_error && batt_cal_model_id(&cal) == model);
    batt_cal_reset(&cal, now);
    batt_cal_status_t status;
    batt_cal_status(&cal, &status);
    assert(status.enabled && !status.applied && !status.sampled_bins && !status.trusted_bins);
    assert(batt_cal_save(&cal, &io, now, true));
    cal.dirty = true;
    assert(batt_cal_save(&cal, &io, now, true));
    slots[cal.slot].checksum ^= 1;
    batt_cal_init(&restored, s_soc_curve, now);
    assert(batt_cal_load(&restored, &io));
    batt_cal_status(&restored, &status);
    assert(status.enabled && !status.applied && !status.sampled_bins);
    puts("PASS: dual-slot recovery, checksum/version rejection, throttled saves, failed commits and reset preservation");
}

static void outliers_and_baseline(void) {
    batt_cal_t baseline;
    batt_cal_init(&baseline, s_soc_curve, 0);
    for (int mv = 2500; mv <= 5000; ++mv) {
        int expected = 0;
        if (mv >= 4380) expected = 1000;
        else for (int i = 1; i < BATT_CAL_NODES; ++i) {
            if (mv >= s_soc_curve[i][0]) {
                expected = s_soc_curve[i][1] + (mv - s_soc_curve[i][0]) *
                    (s_soc_curve[i - 1][1] - s_soc_curve[i][1]) /
                    (s_soc_curve[i - 1][0] - s_soc_curve[i][0]);
                break;
            }
        }
        assert(batt_cal_soc(&baseline, mv) == expected);
    }
    const int normal[4] = {10000, 20000, 40000, 41000}, abnormal[4] = {30000, 2000, 90000, 41000};
    start(); cycle(normal); cycle(normal); cycle(abnormal);
    batt_cal_status_t status;
    batt_cal_status(&cal, &status);
    assert(status.sampled_bins == 4 && status.trusted_bins == 1 && !status.applied);
    for (int i = 0; i < 5; ++i) cycle(normal);
    batt_cal_status(&cal, &status);
    assert(status.trusted_bins == 4 && status.applied);
    puts("PASS: baseline equivalence at every millivolt, outlier rejection and recovery through the five-sample window");
}

int main(void) {
    independent_curves();
    breaks_and_rebounds();
    storage_and_reset();
    outliers_and_baseline();
    return 0;
}
