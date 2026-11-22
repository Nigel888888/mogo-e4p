/* SPDX-License-Identifier: Apache-2.0
 * 中文：检查实际短表每对灰阶只向目标驱动，不能把它当作实屏灰阶或残影验证。
 * English: Check that every actual short-table gray pair drives only toward the target; this does not validate optical grays or ghosting.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "e0470_epaper_waveform.h"
#include "display_main_waveform.h"
#include "display_refresh_config.h"
#include "ui/ui_image_dither.h"

int main(void) {
    e0470_waveform_init();
    const EpdWaveformPhases* complete = e0470_waveform_phases(&E0470_FULL_WAVEFORM, MODE_GC16);
    const EpdWaveformPhases* trimmed = e0470_waveform_phases(&E0470_WAVEFORM, MODE_GC16);
    assert(complete && complete->phases == 48 && complete->phases == E0470_FULL_GC16_FRAMES);
    assert(trimmed && trimmed->phases == E0470_GC16_FRAMES && trimmed->phases < complete->phases);
    const EpdWaveformPhases* short_table = e0470_waveform_phases(&E0470_FOLLOW_WAVEFORM, 1);
    const EpdWaveformPhases* gray_table = e0470_waveform_phases(&E0470_WAVEFORM, 5);
    assert(short_table && gray_table && short_table->phases == 8);
    const EpdWaveformPhases* original_bw = e0470_waveform_phases(&E0470_WAVEFORM, MODE_DU);
    uint8_t saved_bw[E0470_FULL_DU_FRAMES * 16 * 4], saved_gray[E0470_GL16_FRAMES * 16 * 4];
    assert(original_bw && original_bw->phases == E0470_FULL_DU_FRAMES);
    memcpy(saved_bw, original_bw->luts, sizeof(saved_bw));
    memcpy(saved_gray, gray_table->luts, sizeof(saved_gray));
    int gray_reverse_dark = 0;
    for (int from = 0; from < 16; ++from) {
        for (int to = 0; to < 16; ++to) {
            int pushes = 0;
            for (int f = 0; f < short_table->phases; ++f) {
                const int action = e0470_phase_action(short_table, f, to, from);
                if (from == to) assert(action == 0);
                else if (action) {
                    assert(action == (to > from ? 2 : 1));
                    ++pushes;
                }
            }
            if (from != to) assert(pushes > 0 && pushes <= 8);
            if (from == 0 && to == 15) assert(pushes == 8);
            if (from == 15 && to == 0) assert(pushes == 7);
            // GL16 中部分变浅迁移含反向压黑；短表所有对应迁移均已检查无反向动作。
            // Some GL16 brightening pairs include reverse darkening; every corresponding short-table pair above has no reverse action.
            if (to > from) {
                for (int f = 0; f < gray_table->phases; ++f)
                    gray_reverse_dark += e0470_phase_action(gray_table, f, to, from) == 1;
            }
        }
    }
    assert(gray_reverse_dark > 0);

    // 局部灰阶不裁短任何变色迁移，只有等灰动作清零；原始校准表不得被原地改写。
    // Local grays do not truncate changed transitions: only equal-gray actions are cleared; the original calibration table must not be modified in place.
    const EpdWaveform* local = display_main_gray_waveform();
    const EpdWaveformPhases* bands = e0470_waveform_phases(local, MODE_GL16);
    const EpdWaveformPhases* bw = e0470_waveform_phases(&E0470_WAVEFORM, MODE_DU);
    assert(local != &E0470_WAVEFORM && local->temp_intervals == E0470_WAVEFORM.temp_intervals);
    assert(bands && bands->phases == gray_table->phases && bands->phase_times == gray_table->phase_times);
    assert(bw && bw->phases == 20);
    for (int f = 0; f < bw->phases; ++f) {
        assert(e0470_phase_action(bw, f, 0, 0) == 0);
        assert(e0470_phase_action(bw, f, 15, 15) == 0);
    }
    int original_diagonal = 0;
    for (int f = 0; f < bands->phases; ++f)
        for (int from = 0; from < 16; ++from)
            for (int to = 0; to < 16; ++to) {
                int action = e0470_phase_action(bands, f, to, from);
                int original = e0470_phase_action(gray_table, f, to, from);
                if (from == to) {assert(action == 0); original_diagonal += original != 0;}
                else assert(action == original);
            }
    assert(original_diagonal > 0 && display_main_gray_waveform() == local);
    // 书架退出完整复用DU并给变白像素补四相，禁止中灰到白反向压黑。
    // Shelf exit retains full DU plus four whitening phases, forbidding reverse darkening on gray-to-white.
    const EpdWaveform* exit_wave=display_main_shelf_exit_waveform();
    const EpdWaveformPhases* exit_table=e0470_waveform_phases(exit_wave,MODE_DU);
    assert(exit_wave!=&E0470_WAVEFORM && exit_wave->temp_intervals==E0470_WAVEFORM.temp_intervals);
    assert(exit_table && exit_table->phases==24 && !exit_table->phase_times);
    int white_pushes=0;
    for(int f=0;f<exit_table->phases;++f)for(int from=0;from<16;++from)for(int to=0;to<16;++to){
        int expected=from==to?0:f<bw->phases?e0470_phase_action(bw,f,to,from):to==15?2:0;
        int action=e0470_phase_action(exit_table,f,to,from);
        assert(action==expected);
        if(to==15)assert(action==0 || action==2);
        if(to==0)assert(action==0 || action==1);
        if(from==0 && to==15)white_pushes+=action==2;
    }
    assert(white_pushes==22 && display_main_shelf_exit_waveform()==exit_wave);
    // 柔和水波纹只改黑白端点和等灰，中间灰完整保留37相，防止丢失灰阶细节。
    // Soft water changes only monochrome endpoints and equal grays, retaining all 37 midgray phases to preserve detail.
    const EpdWaveform* water_wave=display_main_water_waveform();
    const EpdWaveformPhases* water_table=e0470_waveform_phases(water_wave,MODE_GL16);
    assert(water_wave!=&E0470_WAVEFORM && water_wave->temp_intervals==E0470_WAVEFORM.temp_intervals);
    assert(water_table && water_table->phases==37 && !water_table->phase_times);
    // 变白端点在 DU 段后补六相；变黑端点 2026-10-24 起用原厂擦除序列并补回 18 次连续推黑。
    // The white endpoint adds six pushes after the DU segment; since 2026-10-24 the black
    // endpoint takes the vendor erase sequence with 18 consecutive restored pushes.
    for(int f=0;f<water_table->phases;++f)for(int from=0;from<16;++from)for(int to=0;to<16;++to){
        int expected;
        if(from==to)expected=0;
        else if(to==15)expected=f<bw->phases?e0470_phase_action(bw,f,15,from):(f<bw->phases+6?2:0);
        else if(to==0){
            // 原厂行擦白保留，推黑补到 18 次并右对齐到保持相之前；与组件 boost 同一规则。
            // Vendor erase stays, pushes restore to 18 right-aligned before the holds; the component boost follows the same rule.
            expected=e0470_phase_action(gray_table,f,0,from);
            int erase=0,black=0,first=-1,ordered=1;
            for(int g=0;g<water_table->phases;++g){
                int a=e0470_phase_action(gray_table,g,0,from);
                if(!a)continue;
                if(first<0)first=g;
                if(a==1)++black;
                else if(a==2&&!black)++erase;
                else ordered=0;
            }
            if(ordered&&black&&black<E0470_BOOST_BLACK_PUSHES){
                int start=first;
                if(start+erase+E0470_BOOST_BLACK_PUSHES-1>water_table->phases-1-E0470_TRIM_HOLD)
                    start=water_table->phases-1-E0470_TRIM_HOLD-erase-E0470_BOOST_BLACK_PUSHES+1;
                expected=f>=start&&f<start+erase?2:f>=start+erase&&f<start+erase+E0470_BOOST_BLACK_PUSHES?1:0;
            }
        }
        else expected=e0470_phase_action(gray_table,f,to,from);
        int action=e0470_phase_action(water_table,f,to,from);
        assert(action==expected);
        if(to==15)assert(action==0 || action==2);
        if(to==0)assert(action==0 || action==1 || action==2);
    }
    assert(display_main_water_waveform()==water_wave);
    const EpdWaveformPhases* first_table=e0470_waveform_phases(display_main_bw_waveform(),MODE_DU);
    assert(first_table && first_table->phases == 20);
    for (int from=0;from<16;++from) for (int to=0;to<16;++to) for (int f=0;f<20;++f)
        assert(e0470_phase_action(first_table,f,to,from)==(from==to?0:e0470_phase_action(bw,f,to,from)));
    assert(!memcmp(saved_bw,bw->luts,sizeof(saved_bw)) && !memcmp(saved_gray,gray_table->luts,sizeof(saved_gray)));
    const EpdWaveform *normal_wave=display_main_normal_waveform();
    const EpdWaveformPhases *normal_table=e0470_waveform_phases(normal_wave,MODE_GL16);
    assert(normal_table&&normal_table->phases==water_table->phases&&display_main_normal_waveform()==normal_wave);
    // 三条有效局刷表都必须有足够的白→黑相，不能在真机静默退回原出口。
    // The three effective partial-update tables need enough white-to-black pushes to avoid silent fallback on device.
    const EpdWaveform *eligible[] = {&E0470_WAVEFORM, local, normal_wave};
    for (size_t i = 0; i < sizeof(eligible) / sizeof(*eligible); ++i) {
        assert(eligible[i]->num_temp_ranges == 1);
        const EpdWaveformPhases *p = e0470_waveform_phases(eligible[i], MODE_GL16);
        assert(p && p->phases <= 64);
        unsigned black_pushes = 0;
        for (int f = 0; f < p->phases; ++f) black_pushes += e0470_phase_action(p, f, 0, 15) == 1;
        assert(black_pushes >= DISPLAY_STATIC_BLACK_PUSHES);
    }
    int softened=0;
    for(int from=0;from<16;++from)for(int to=0;to<16;++to){
        int old_counts[4]={0},new_counts[4]={0},differences=0,first=-1,last=-1;
        for(int phase=0;phase<water_table->phases;++phase){
            int a=e0470_phase_action(water_table,phase,to,from),b=e0470_phase_action(normal_table,phase,to,from);
            ++old_counts[a];++new_counts[b];differences+=a!=b;
            if(a){if(first<0)first=phase;last=phase;}
            if(to==from||to==0||to==15||a==0||a==3)assert(a==b);
        }
        assert(!memcmp(old_counts,new_counts,sizeof(old_counts))&&differences<=4);
        if(first>=0){assert(e0470_phase_action(water_table,first,to,from)==e0470_phase_action(normal_table,first,to,from));
            assert(e0470_phase_action(water_table,last,to,from)==e0470_phase_action(normal_table,last,to,from));}
        softened+=differences>0;
    }
    assert(softened>0&&!memcmp(saved_gray,gray_table->luts,sizeof(saved_gray)));
    printf("PASS: ordinary main %d softened gray-edge paths retain all37 phases, counts, holds, endpoints; ripple and vendor outputs preserved\n",softened);
    // 普通书架留白从各级灰只向白；其余十五级完整保留原GL16/GC16与同时输出相数。
    // Ordinary shelf whitespace drives only white from every gray; all other targets retain original GL16/GC16 actions and simultaneous phase counts.
    for(int index=0;index<2;++index){
        enum EpdDrawMode mode=index?MODE_GC16:MODE_GL16;
        const EpdWaveform *shelf=display_main_shelf_waveform(mode);
        const EpdWaveformPhases *source=e0470_waveform_phases(&E0470_WAVEFORM,mode);
        const EpdWaveformPhases *table=e0470_waveform_phases(shelf,mode);
        assert(shelf!=&E0470_WAVEFORM&&table&&table->phases==source->phases);
        assert(display_main_shelf_waveform(mode)==shelf);
        for(int f=0;f<table->phases;++f)for(int from=0;from<16;++from)for(int to=0;to<16;++to){
            int action=e0470_phase_action(table,f,to,from);
            if(to!=15)assert(action==e0470_phase_action(source,f,to,from));
            else{assert(action==0||action==2);assert(action==e0470_phase_action(normal_table,f,to,from));}
        }
    }
    assert(!memcmp(saved_gray,gray_table->luts,sizeof(saved_gray)));
    puts("PASS: shelf white pixels never reverse-darken; all 15 art targets, GL16/GC16 counts and vendor LUTs preserved");
    // 封面映射单调、保留端点，黑白点阵相位稳定；通用灰阶抖动保持原样。
    // Cover tones are monotone and endpoint-preserving; BW dots stay phase-stable and generic gray dithering is unchanged.
    for (unsigned contrast = 100; contrast <= 200; contrast += 10) {
        uint8_t previous = 0;
        for (int gray = 0; gray <= 255; ++gray) {
            uint8_t tone = ui_image_cover_tone((uint8_t)gray, contrast);
            assert(tone >= previous);
            previous = tone;
            if (gray == 0 || gray == 255) assert(tone == gray);
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                int v = ui_image_dither_bw(tone, x, y);
                assert(v == 0 || v == 255);
                assert(v == ui_image_dither_bw(tone, x + 4, y + 4));
                assert(v == ui_image_dither_bw(tone, x - 4, y - 4));
                if (gray == 0 || gray == 255) assert(v == gray);
            }
        }
    }
    for (int gray = 32; gray <= 224; gray += 32) {
        uint8_t tone = ui_image_cover_tone((uint8_t)gray, 100);
        unsigned old_total = 0, new_total = 0;
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
            old_total += ui_image_dither_gray((uint8_t)gray, x, y);
            new_total += ui_image_dither_gray(tone, x, y);
        }
        assert(new_total < old_total && old_total - new_total <= 21 * 16);
    }
    assert(ui_image_cover_tone(248, 100) >= 245 && ui_image_dither_gray(119, 0, 0) == 119);
    puts("navigation waveform: calibrated GL16, unchanged grays, shelf whitespace erase, original water midgrays, vendor tables and darker cover tones passed; optical behavior requires hardware");
    return 0;
}
