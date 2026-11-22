#pragma once

#include "epd_highlevel.h"

/// 将有效的竖屏或横屏区域转成原生坐标。/ Convert valid logical bounds to native coordinates.
bool display_static_black_native(EpdRect logical, EpdRect *native);

/// 将区域转成原生坐标，并检测区域内是否有真实变化。
/// Convert valid logical bounds to native coordinates and detect actual changes inside them.
bool display_static_black_changed(const EpdiyHighlevelState *hl, EpdRect logical, EpdRect *native);

/// 尝试单轮扫描加区外纯黑补偿；false 表示未扫描，由原出口处理。
/// Try one scan with outside-black reinforcement; false means no scan and legacy fallback.
bool display_static_black_draw(EpdiyHighlevelState *hl, const EpdWaveform *waveform,
                               EpdRect native, bool stable_only, bool full_body, unsigned black_pushes,
                               enum EpdDrawError *result);

/// 同一扫描提交原生坐标下互不重叠的正文、顶部和页脚；零矩形表示不更新该栏。
/// Publish disjoint native body and chrome regions in one scan; a zero rectangle leaves that bar unchanged.
bool display_static_black_join(EpdiyHighlevelState *hl, const EpdWaveform *waveform, EpdRect body,
                               const EpdRect chrome[2], bool full_body, unsigned black_pushes,
                               enum EpdDrawError *result);
