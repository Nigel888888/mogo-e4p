/*
 * 中文：CloudZao 已结束文件结果的限量 TF 记录，不保存可重放的任务。
 * English: Bounded TF results for ended CloudZao files; no replayable tasks.
 */
#pragma once
#include "cloudzao_receive.h"

cz_error_t cloudzao_history_read(const cz_ops_t *ops, cz_history_item_t *items, unsigned *count);
/// 只移除当前 TF 卡的历史文件；不存在即已清空。/ Remove only the current card’s history file; a missing file is already clear.
cz_error_t cloudzao_history_clear(const cz_ops_t *ops);
bool cloudzao_history_record(const cz_ops_t *ops, uint32_t *next, const cz_row_t *row, uint32_t utc);
