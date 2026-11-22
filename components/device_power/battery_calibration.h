/*
 * 中文：本机放电曲线学习与双槽记录；不访问硬件、不控制充电、不依赖日志。
 * English: Device-local discharge learning and dual-slot records; no hardware, charging control or logging dependency.
 * 冻结：仅完整有效的清醒区间可学习；未观测区间和曲线端点保持不变。
 * Frozen: Learn only complete eligible awake intervals; retain unobserved ranges and curve endpoints.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BATT_CAL_NODES 22
#define BATT_CAL_BINS (BATT_CAL_NODES - 1)
#define BATT_CAL_HISTORY 5
#define BATT_CAL_SETTLE_US (60LL * 1000000)
#define BATT_CAL_MAX_GAP_US (5LL * 1000000)
#define BATT_CAL_SAVE_US (300LL * 1000000)

typedef struct {
    uint32_t version;                         ///< 记录版本 / Record revision
    uint32_t baseline;                        ///< 基线指纹 / Baseline fingerprint
    uint32_t sequence;                        ///< 保存序号 / Save sequence
    uint32_t enabled;                         ///< 仅本机学习开关 / Local-only learning preference
    uint32_t seen;                            ///< 本轮已完成区间 / Completed intervals in this discharge
    uint32_t soc[BATT_CAL_NODES];              ///< 当前曲线千分比 / Active curve in permille
    uint32_t samples[BATT_CAL_BINS][BATT_CAL_HISTORY]; ///< 各次完整经过的微安时 / Microamp-hours per complete traversal
    uint32_t count[BATT_CAL_BINS];             ///< 已存样本数 / Stored sample counts
    uint32_t next[BATT_CAL_BINS];              ///< 环形写入位置 / Ring write cursors
    uint32_t checksum;                        ///< 完整记录 CRC32 / Whole-record CRC32
} batt_cal_record_t;

typedef enum {
    BATT_CAL_RESET_NONE = 0,     ///< 无待完成重置 / No pending reset
    BATT_CAL_RESET_OFF = 1,      ///< 待恢复基线，开关保持关闭 / Pending baseline reset, keep learning off
    BATT_CAL_RESET_ON = 2,       ///< 待恢复基线，开关保持开启 / Pending baseline reset, keep learning on
    BATT_CAL_RESET_UNREADABLE,   ///< 重置意图读取失败，不回退旧槽 / Cannot read intent; never fall back to old slots
} batt_cal_reset_state_t;

typedef struct {
    const int32_t (*base)[2]; ///< 固定电压节点与基线 / Fixed voltage nodes and baseline
    batt_cal_record_t record; ///< 可持久化状态 / Persistent state
    bool dirty, locked, have_previous, have_power, last_power; ///< 生命周期状态 / Lifecycle state
    int active_bin;           ///< 正在完整经过的区间，负值为空 / Active traversal, negative when absent
    int previous_mv, previous_ma; ///< 上次有效读数 / Previous eligible readings
    int minimum_mv;           ///< 当前区间最低电压，限制回弹 / Traversal voltage minimum for rebound rejection
    int64_t previous_us, settle_until_us; ///< 单调采样和稳定期限 / Monotonic sample and settling deadline
    int64_t integral, start_integral; ///< 电流积分，mA·µs / Current integrals in mA microseconds
    int64_t last_save_us;     ///< 上次存储尝试 / Last persistence attempt
    int slot;                ///< 最近有效槽，负值为空 / Latest valid slot, negative when absent
    batt_cal_reset_state_t reset_state; ///< 当前重置意图 / Current reset intent
    unsigned reset_copies;    ///< 此次运行已确认的基线副本数 / Baseline copies confirmed in this boot
    bool save_error;         ///< 最近存储失败 / Last persistence failed
} batt_cal_t;

typedef struct {
    bool enabled, applied, save_error; ///< 开关、个性曲线和保存状态 / Learning, custom curve and storage state
    uint8_t sampled_bins, trusted_bins, total_bins; ///< 有样本、可信及总区间数 / Sampled, trusted and total intervals
} batt_cal_status_t;

typedef struct {
    bool (*read)(void *ctx, unsigned slot, batt_cal_record_t *record); ///< 读取完整槽 / Read a complete slot
    bool (*write)(void *ctx, unsigned slot, const batt_cal_record_t *record); ///< 提交完整槽 / Commit a complete slot
    batt_cal_reset_state_t (*read_reset)(void *ctx); ///< 区分无意图与读取失败 / Distinguish absent intent from a failed read
    bool (*write_reset)(void *ctx, batt_cal_reset_state_t state); ///< 提交重置意图或标记完成 / Commit reset intent or completion
    void *ctx; ///< 存储上下文 / Storage context
} batt_cal_storage_t;

/// 本机完整记录校验，也供原子电量检查点复用。/ Local record checksum, also shared by atomic SOC checkpoints.
uint32_t batt_cal_checksum(const void *data, size_t size);

/// 初始化为系统基线；启动后先稳定一分钟。/ Initialize the baseline with a one-minute startup settling window.
void batt_cal_init(batt_cal_t *s, const int32_t base[BATT_CAL_NODES][2], int64_t now);
/// 改变学习开关但保留曲线和已完成样本。/ Change learning while retaining the curve and complete samples.
void batt_cal_enable(batt_cal_t *s, bool enabled, int64_t now);
/// 锁屏与唤醒都中断当前区间；唤醒后等待一分钟。/ Lock and wake break traversal; wake settles for one minute.
void batt_cal_lock(batt_cal_t *s, bool locked, int64_t now);
/// 有效标记必须包含电压、电流和充电芯片状态；power 是实际外部供电。/ Validity includes voltage, current and charger status; power means external input.
void batt_cal_observe(batt_cal_t *s, int64_t now, int mv, int ma, bool valid, bool power);
/// 用当前个性曲线插值；不读取显示百分比作为学习输入。/ Interpolate the custom curve without learning from displayed percentages.
int32_t batt_cal_soc(const batt_cal_t *s, int32_t mv);
/// 当前曲线身份，与开关、样本数和保存序号无关。/ Curve identity independent of learning, samples and save sequence.
uint32_t batt_cal_model_id(const batt_cal_t *s);
/// 只读查询学习覆盖；不读硬件、不保存。/ Query coverage without hardware or storage access.
void batt_cal_status(const batt_cal_t *s, batt_cal_status_t *out);
/// 清除模型和样本，保留开关及锁屏状态；调用方负责保存。/ Reset samples and curve, retaining learning and lock state; caller persists it.
void batt_cal_reset(batt_cal_t *s, int64_t now);
/// 先遵循重置意图，否则选较新有效槽。/ Honor a reset intent first, otherwise choose the newer valid slot.
bool batt_cal_load(batt_cal_t *s, const batt_cal_storage_t *storage);
/// 交替提交完整快照；失败不清 dirty，不推进槽与序号。/ Alternate complete snapshots; failures retain dirty state, slot and sequence.
bool batt_cal_save(batt_cal_t *s, const batt_cal_storage_t *storage, int64_t now, bool force);

/// 先持久化重置意图，再覆盖两槽；跨启动续做，不恢复过时的曲线。/ Journal intent before overwriting both slots; resume across boots without restoring obsolete curves.
bool batt_cal_reset_persist(batt_cal_t *s, const batt_cal_storage_t *storage, int64_t now);
