/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 从完整灰阶表裁擦除/饱和余量。自行调整屏幕波形会使设备失去保修。
 *
 * Trim erase/sat slack from a full gray table. Changing panel waveforms
 * voids the warranty.
 */

#include "e0470_waveform_trim.h"

#include <stddef.h>

#define MAX_PHASES 64

// 表的字节布局：data[frame][to][from/4]，每字节高位起 4 个 2bit 动作。
// / Table layout: data[frame][to][from/4], four 2-bit actions MSB-first.
static void unpack_sequence(
    const uint8_t (*data)[16][4], int phases, int to, int group, int shift, uint8_t *seq
) {
    for (int f = 0; f < phases; f++) seq[f] = (uint8_t)((data[f][to][group] >> shift) & 3);
}

typedef struct {
    int keep_e, keep_s, tail_start, len;
} trimmed_span_t;

static int run_length(const uint8_t* seq, int start, int end, int value) {
    int n = 0;
    while (start + n < end && seq[start + n] == value) n++;
    return n;
}

// to=15 的饱和极性是白（0b10），其余是黑（0b01）；擦除极性总是反面。
// Saturation is white (0b10) for to=15, otherwise black (0b01); erase is the opposite.
static trimmed_span_t trim_one(const uint8_t* seq, int phases, int to, const e0470_trim_t* trim) {
    const int sat = to == 15 ? 2 : 1;
    const int erase = to == 15 ? 1 : 2;

    int a = 0;
    while (a < phases && seq[a] == 0) a++;
    int z = phases;
    while (z > a && seq[z - 1] == 0) z--;
    if (a == z) return (trimmed_span_t){0};  // 全保持 / All-hold

    const int e = run_length(seq, a, z, erase);
    const int s = run_length(seq, a + e, z, sat);
    const int keep_e = e < trim->erase_max ? e : trim->erase_max;
    const int want_cut = to == 15 ? trim->white_sat_cut : trim->sat_cut;
    int cut_s = want_cut < s - 1 ? want_cut : s - 1;
    if (cut_s < 0) cut_s = 0;
    const int keep_s = s - cut_s;
    const int tail = a + e + s;
    return (trimmed_span_t){.keep_e = keep_e, .keep_s = keep_s, .tail_start = tail,
                            .len = keep_e + keep_s + z - tail};
}

int e0470_waveform_trim(
    const EpdWaveformPhases* src, const e0470_trim_t* trim, uint8_t (*dst_data)[16][4]
) {
    if (src == NULL || src->luts == NULL || trim == NULL || dst_data == NULL) return 0;
    const int src_phases = src->phases;
    if (src_phases <= 0 || src_phases > MAX_PHASES) return 0;
    if (trim->erase_max < 0 || trim->sat_cut < 0 || trim->white_sat_cut < 0 || trim->hold < 1) {
        return 0;
    }

    const uint8_t(*data)[16][4] = (const uint8_t(*)[16][4])src->luts;
    uint8_t seq[MAX_PHASES];
    int longest = 0;

    // 先量长度并校验输出，再逐组重算；不常驻保存全部 256 条轨迹。
    // Measure lengths and validate the output first, then recompute by group without retaining all 256 tracks.
    for (int to = 0; to < 16; to++) {
        for (int group = 0; group < 4; group++) {
            for (int shift = 6; shift >= 0; shift -= 2) {
                unpack_sequence(data, src_phases, to, group, shift, seq);
                const int len = trim_one(seq, src_phases, to, trim).len;
                if (len > longest) longest = len;
            }
        }
    }

    if (trim->hold > src_phases - longest) return 0;
    const int phases = longest + trim->hold;
    for (int to = 0; to < 16; to++) {
        const int sat = to == 15 ? 2 : 1;
        const int erase = to == 15 ? 1 : 2;
        for (int group = 0; group < 4; group++) {
            // 每组四条轨迹写成一个字节；读完该组再写，允许源和目标为同一张表。
            // Pack four tracks into one byte; finish reading the group before writing, allowing in-place use.
            uint8_t packed[MAX_PHASES] = {0};
            for (int shift = 6; shift >= 0; shift -= 2) {
                unpack_sequence(data, src_phases, to, group, shift, seq);
                const trimmed_span_t t = trim_one(seq, src_phases, to, trim);
                int f = longest - t.len;
                for (int i = 0; i < t.keep_e; i++) packed[f++] |= (uint8_t)(erase << shift);
                for (int i = 0; i < t.keep_s; i++) packed[f++] |= (uint8_t)(sat << shift);
                for (int i = t.tail_start; f < longest; i++) packed[f++] |= (uint8_t)(seq[i] << shift);
            }
            for (int f = 0; f < phases; f++) dst_data[f][to][group] = packed[f];
        }
    }
    return phases;
}
