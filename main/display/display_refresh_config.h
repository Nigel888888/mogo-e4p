/* 中文：集中既有清理周期；计数状态和触发边界仍归原呈现模块。
 * English: Centralize existing cleanup cadences; original presenters retain counters and trigger boundaries.
 */
#pragma once
#define DISPLAY_REFRESH_SYSTEM_CLEANUP_EVERY 40
#define DISPLAY_REFRESH_MAIN_CLEANUP_EVERY 12
#define DISPLAY_REFRESH_SUBPAGE_SETTLE_EVERY 3
#define DISPLAY_REFRESH_READER_CLEANUP_DEFAULT 15

// 通用局刷补偿间隔及推黑预算。/ Generic partial-update reinforcement cadence and black pulse budget.
#define DISPLAY_STATIC_BLACK_EVERY 4
#define DISPLAY_STATIC_BLACK_PUSHES 4

// 阅读翻页补黑预算；补白另外复用一相。/ Reader turn black pulse budget; whitening separately reuses one phase.
#define DISPLAY_READER_CHROME_PUSHES 4
