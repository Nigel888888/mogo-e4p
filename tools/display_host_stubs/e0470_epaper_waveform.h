/* SPDX-License-Identifier: Apache-2.0
 * 波形标识替身。/ Waveform identity shim.
 */
#pragma once
#include "epdiy.h"
extern const EpdWaveform E0470_WAVEFORM, E0470_FOLLOW_WAVEFORM, E0470_GRAY8_WAVEFORM, E0470_FULL_WAVEFORM;
extern const EpdWaveform E0470_CAPSULE_DRAG_WAVEFORM, E0470_CAPSULE_WHITE_WAVEFORM,
    E0470_CAPSULE_GRAY_WAVEFORM, E0470_APPLY_WAVEFORM;

static inline const EpdWaveformPhases* e0470_waveform_phases(const EpdWaveform* w, int mode) {
    for (int i = 0; w && w->mode_data && i < w->num_modes; ++i)
        if (w->mode_data[i]->type == mode) return w->mode_data[i]->range_data[0];
    return NULL;
}

static inline int e0470_phase_action(const EpdWaveformPhases* phases, int phase, int to, int from) {
    if (!phases || !phases->luts || phase < 0 || phase >= phases->phases ||
        (unsigned)to > 15 || (unsigned)from > 15) return 0;
    return (phases->luts[phase * 64 + to * 4 + from / 4] >> (6 - 2 * (from % 4))) & 3;
}
