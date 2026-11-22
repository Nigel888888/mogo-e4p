#!/usr/bin/env python3
"""中文：执行 Metalio 电池采样，验证读数有效性、充电来源和公共快照。
English: Exercise Metalio battery polling, sample validity, charging sources and public snapshots."""
from pathlib import Path
import re
import subprocess
import tempfile
from home_cover_cache_host_test import function

root = Path(__file__).resolve().parents[1]
backend = root / 'components/device_power/device_power_metalio.c'
source = backend.read_text()
# 提取真实滤波参数与类型；采样流程直接执行产品实现。/ Extract real filter parameters/types and execute the product polling implementation.
defines = '\n'.join(line for line in source.splitlines()
                    if re.match(r'#define (?:BATT_|BQ_REG_|CX_REG_STATUS1\b|CX_CHG_STAT_TOPOFF\b)', line))
curve = re.search(r'^static const int32_t s_soc_curve\[.*?\};', source, re.M | re.S).group(0)
types = '\n'.join(re.search(r'typedef struct \{[^}]*\} ' + name + ';', source, re.S).group(0)
                  for name in ('batt_filter_t',))
code = r'''
#include "device_power.h"
#include "battery_log.h"
#include "battery_calibration.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#define ESP_FAIL -1
#define ESP_OK 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
'''
code += defines + '\n' + curve + '\n' + types + '\n'
code += r'''
static void *s_bq, *s_cx;
static pmu_snapshot_t s_snap;
static batt_filter_t s_batt_filter;
static batt_log_state_t s_batt_log;
static batt_cal_t s_batt_cal;
static const batt_cal_storage_t s_batt_cal_storage = {0};
static bool s_charger_uv_shutdown;
static int s_batt_log_mux;
static int32_t s_batt_saved_permille;
static uint8_t s_batt_restore;
static int32_t s_batt_persisted_permille;
static uint32_t s_batt_display_model;
static bool s_batt_charging_last;
static int64_t s_batt_last_save_us, s_batt_ic_us, s_batt_full_since_us;
static int64_t s_batt_poll_us;
static bool s_batt_poll_reusable;
static int8_t s_batt_ic_soc;
static int64_t now_us;
static int64_t batt_log_wall(void) { return 1793415600 + now_us / 1000000; }
static void batt_log_notify(void) {}
static uint16_t raw_mv;
static int16_t raw_ma;
static uint8_t charger_state;
static bool voltage_fail, current_fail, charger_fail;
static unsigned saves, soc_reads;
static void cx_charger_poll(int64_t now, bool have_v, uint16_t mv, bool have_i, int16_t ma,
                            bool have_status, uint8_t status) {
    (void)now; (void)have_v; (void)mv; (void)have_i; (void)ma; (void)have_status; (void)status;
}
static int64_t esp_timer_get_time(void) { return now_us; }
static void batt_save(int32_t value) { assert(value >= 0 && value <= 1000); ++saves; s_batt_persisted_permille=value; }
static int bq_read_soc(void) { ++soc_reads; return 63; }
static esp_err_t dev_read(void *dev, uint8_t reg, uint8_t *out, size_t len) {
    if (dev == s_cx) {
        assert(reg == CX_REG_STATUS1 && len == 1);
        if (charger_fail) return ESP_FAIL;
        *out = charger_state << 3; return ESP_OK;
    }
    assert(dev == s_bq && len == 2);
    uint16_t value;
    if (reg == BQ_REG_VOLTAGE) {
        if (voltage_fail) return ESP_FAIL;
        value = raw_mv;
    } else {
        assert(reg == BQ_REG_CURRENT);
        if (current_fail) return ESP_FAIL;
        value = (uint16_t)raw_ma;
    }
    out[0] = value & 0xff; out[1] = value >> 8; return ESP_OK;
}
'''
for name in ('bq_read_voltage', 'bq_read_current', 'batt_compensated_mv', 'batt_curve_soc', 'batt_effective_soc', 'batt_filter_step', 'batt_full_now', 'batt_log_enabled', 'poll_battery'):
    code += function(name, backend) + '\n'
code += r'''
static void reset(void) {
    memset(&s_snap, 0, sizeof(s_snap)); s_snap.present = true;
    memset(&s_batt_filter, 0, sizeof(s_batt_filter));
    batt_cal_init(&s_batt_cal,s_soc_curve,0);
    s_bq = (void *)1; s_cx = (void *)2;
    s_batt_saved_permille = s_batt_persisted_permille = -1; s_batt_full_since_us = -1;
    s_batt_poll_us = 0; s_batt_poll_reusable = false; s_charger_uv_shutdown=false;
    s_batt_last_save_us = now_us = 0; s_batt_ic_us = -1; s_batt_ic_soc = -1;
    memset(&s_batt_log, 0, sizeof(s_batt_log));
    s_batt_charging_last = false;
    voltage_fail = current_fail = charger_fail = false;
    raw_mv = 4000; raw_ma = 0; charger_state = saves = soc_reads = 0;
}
int main(void) {
    reset(); assert(pmu_battery_percent(&s_snap) == -1);
    poll_battery();
    assert(s_snap.status_ok && !s_snap.quick_ok && pmu_battery_percent(&s_snap) == 70);
    assert(s_snap.battery_raw == 4000 && !pmu_battery_charging(&s_snap));
    // 曲线重置不能更换仍在显示的旧值身份。/ Reset must not relabel a held display value.
    reset();s_batt_cal.record.soc[5]-=20;poll_battery();
    const uint32_t old_model=s_batt_display_model;
    assert(s_snap.soc_permille==675 && old_model==batt_cal_model_id(&s_batt_cal));
    batt_cal_reset(&s_batt_cal,now_us);
    s_batt_cal.dirty=false;
    now_us+=1000000;poll_battery();
    assert(s_snap.soc_permille==675 && s_batt_display_model==old_model);
    assert(batt_cal_model_id(&s_batt_cal)!=old_model);
    raw_mv=3900;now_us+=60000000;poll_battery();
    assert(s_snap.soc_permille<675 && s_batt_display_model==batt_cal_model_id(&s_batt_cal));
    // 无效电压和 I2C 失败清有效位，不能继续显示上次电量。
    // Invalid voltage or failed I2C clears validity instead of presenting the last percent.
    const uint16_t invalid[] = {0, 2499, 5001, 65535};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reset(); poll_battery(); raw_mv = invalid[i]; poll_battery();
        assert(pmu_battery_percent(&s_snap) == -1);
    }
    reset(); poll_battery(); voltage_fail = true; poll_battery(); assert(pmu_battery_percent(&s_snap) == -1);
    reset(); s_bq = NULL; poll_battery(); assert(pmu_battery_percent(&s_snap) == -1);
    // CX 状态优先；读取失败或缺芯片时才用有符号电流判断充电。
    // CX status wins; signed current is the charging fallback when CX is absent or unreadable.
    reset(); raw_ma = 200; poll_battery(); assert(!pmu_battery_charging(&s_snap));
    reset(); charger_state = 1; raw_ma = -100; poll_battery(); assert(pmu_battery_charging(&s_snap));
    assert(s_snap.battery_mv == 4018);
    reset(); raw_mv = 4174; raw_ma = 868; poll_battery();
    assert(!pmu_battery_charging(&s_snap) && s_snap.battery_mv == 4024);
    reset(); charger_fail = true; raw_ma = 21; poll_battery(); assert(pmu_battery_charging(&s_snap));
    reset(); s_cx = NULL; raw_ma = -200; poll_battery(); assert(!pmu_battery_charging(&s_snap));
    assert(s_snap.battery_mv == 4036);
    reset(); charger_fail = current_fail = true; poll_battery();
    assert(!pmu_battery_charging(&s_snap) && pmu_battery_percent(&s_snap) == 70);
    // 充满对齐、恢复值只消费一次，日志关闭时不增加库仑计读数。
    // Full-charge alignment, one-time saved value consumption and no coulomb reads while logging is off.
    reset(); charger_state = CX_CHG_STAT_TOPOFF; raw_mv = 4400; poll_battery();
    assert(pmu_battery_percent(&s_snap) == 100 && saves == 1);
    reset(); raw_mv = 3900; s_batt_saved_permille = 540; poll_battery();
    assert(pmu_battery_percent(&s_snap) == 54 && s_batt_saved_permille == -1 && !soc_reads);
    batt_log_set_enabled(&s_batt_log, true, now_us, batt_log_wall());
    now_us = (int64_t)BATT_LOG_INTERVAL_MS * 1000; poll_battery();
    assert(soc_reads == 1 && s_batt_log.latest.ic_soc == 63);
    assert(s_batt_log.latest.up_us == now_us && s_batt_log.latest.ic_us == now_us);
    // 失败的采样也必须替换日志快照；不能保留旧电压或把无效电流当零电流。
    // Failed attempts must replace the log sample rather than retaining old voltage or treating invalid current as zero.
    now_us += 1000000; voltage_fail = true; current_fail = true; poll_battery();
    assert(!s_batt_log.latest.have_v && !s_batt_log.latest.have_i);
    assert(s_batt_log.latest.up_us == now_us && s_batt_log.latest.ocv_mv == -1);
    voltage_fail = current_fail = false; raw_ma = -200; now_us += 1000000; poll_battery();
    assert(s_batt_log.latest.have_v && s_batt_log.latest.have_i && s_batt_log.latest.ma == -200);
    assert(s_batt_log.latest.comp_mv == 3936 && s_batt_log.latest.curve_permille >= 0);
    assert(soc_reads == 1 && s_batt_log.latest.ic_us == now_us - 2000000);
    batt_log_set_enabled(&s_batt_log, false, now_us, batt_log_wall());
    now_us += BATT_LOG_PERIOD_US; poll_battery(); assert(soc_reads == 1);
    // 判满必须有连续有效采样；丢读数、长空白和持续放电均重新确认。
    // Full confirmation requires continuous valid samples; failures, gaps and discharge restart it.
    reset(); raw_mv=4370; raw_ma=10; charger_state=2; poll_battery();
    for (int i=1;i<=300;i++) {now_us=(int64_t)i*1000000;poll_battery();}
    assert(pmu_battery_percent(&s_snap)==100);
    reset(); raw_mv=4370; raw_ma=10; charger_state=2; poll_battery();
    now_us=300000000;poll_battery();assert(pmu_battery_percent(&s_snap)<100);
    for(int i=1;i<=299;i++){now_us+=1000000;poll_battery();}
    assert(pmu_battery_percent(&s_snap)<100);
    now_us+=1000000;poll_battery();assert(pmu_battery_percent(&s_snap)==100);
    reset(); raw_mv=4370; raw_ma=10; charger_state=2; poll_battery();
    now_us=1000000;current_fail=true;poll_battery();assert(s_batt_full_since_us==-1);
    current_fail=false;raw_ma=-100;now_us+=1000000;poll_battery();assert(s_batt_full_since_us==-1);
    // 日志使用真正的确认窗口，短暂高电流和长空白均保留中断原因。
    reset();raw_mv=4370;raw_ma=10;charger_state=2;
    batt_log_set_enabled(&s_batt_log,true,0,batt_log_wall());poll_battery();
    now_us=1000000;poll_battery();assert(s_batt_log.latest.full_hold_ms==1000);
    raw_ma=100;now_us+=1000000;poll_battery();
    assert(s_batt_log.latest.full_reset && (s_batt_log.latest.full_gate&BATT_LOG_FULL_HIGH_I));
    assert(s_batt_log.latest.full_hold_ms==-1);
    raw_ma=10;now_us+=1000000;poll_battery();
    now_us+=6000000;poll_battery();
    assert(s_batt_log.latest.full_reset && (s_batt_log.latest.full_gate&BATT_LOG_FULL_GAP));
    assert(s_batt_log.latest.full_hold_ms==0);
    reset();s_batt_saved_permille=700;poll_battery();assert(s_batt_restore==1);
    reset();s_batt_saved_permille=100;poll_battery();assert(s_batt_restore==5);
    // 累计变化提前保存，但不突破一分钟写入门限。
    // Save accumulated changes without crossing the one-minute write gate.
    reset();s_batt_persisted_permille=650;poll_battery();assert(!saves);
    now_us=59999999;poll_battery();assert(!saves);
    now_us=60000000;poll_battery();assert(saves==1 && s_batt_persisted_permille==695);
    now_us+=200000;poll_battery();assert(saves==1);
    // 关闭日志仍直接学习补偿/EMA电压；锁屏与同轮读数失败立即打断。
    // Logging-off polls still learn compensated/EMA voltage; locks and same-poll failures interrupt immediately.
    reset();raw_ma=-100;raw_mv=4110;batt_cal_enable(&s_batt_cal,true,0);poll_battery();
    for(int t=1;t<=180;++t){now_us=(int64_t)t*1000000;raw_mv=t<=60?4110:4110-(t-60);poll_battery();}
    assert(s_batt_cal.record.count[3]==1&&!soc_reads&&!batt_log_enabled());
    assert(s_batt_cal.previous_mv==s_snap.battery_mv&&s_batt_cal.previous_ma==raw_ma);
    current_fail=true;now_us+=1000000;poll_battery();assert(!s_batt_cal.have_previous);
    current_fail=false;batt_cal_lock(&s_batt_cal,true,now_us);now_us+=1000000;poll_battery();
    assert(!s_batt_cal.have_previous&&s_batt_cal.record.count[3]==1);
    now_us=400000000;poll_battery();assert(s_batt_cal.dirty);
    reset();batt_cal_enable(&s_batt_cal,true,0);s_charger_uv_shutdown=true;
    now_us=400000000;poll_battery();assert(s_batt_cal.dirty&&!s_batt_cal.have_previous);

    // 公共快照的兼容有效位与边界仍保留，不再依赖旧 CW32 解析器。
    // Keep public snapshot validity/boundary compatibility without the old CW32 parser.
    s_snap.quick_ok = true; s_snap.qb_flags = 3; s_snap.qb_soc = 0; assert(pmu_battery_percent(&s_snap) == 0);
    s_snap.qb_soc = 1000; assert(pmu_battery_percent(&s_snap) == 100);
    s_snap.qb_soc = 1001; assert(pmu_battery_percent(&s_snap) == -1);
    s_snap.qb_soc = 500; s_snap.qb_flags = 1; assert(pmu_battery_percent(&s_snap) == -1);
    s_snap.qb_flags = 7; assert(pmu_battery_charging(&s_snap));
    assert(pmu_battery_percent(NULL) == -1 && !pmu_battery_charging(NULL));
    puts("PASS: actual Metalio battery polling, invalid samples, signed BQ current, CX precedence/fallback, full charge, saved state and snapshot compatibility");
}
'''
persist = '#include "battery_calibration.h"\n' + re.search(r'typedef struct \{[^}]*\} batt_soc_checkpoint_t;', source, re.S).group(0) + r'''
#include "device_power.h"
#include "battery_calibration.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_SUPPORTED -2
#define ESP_ERR_NVS_NOT_FOUND -4
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef int nvs_handle_t;
static pmu_snapshot_t s_snap;
static int32_t s_batt_persisted_permille=-1, stored_soc=600;
static uint32_t stored_ver, s_batt_restore_version;
static uint32_t s_batt_persisted_model, s_batt_display_model;
static batt_soc_checkpoint_t stored_checkpoint;
static bool have_checkpoint;
static batt_cal_t s_batt_cal;
static batt_cal_record_t saved_slots[2];
static bool have_slots[2];
static unsigned cal_writes, fail_cal_write;
static uint8_t stored_reset;
static bool reset_present, fail_reset_clear;
static uint8_t s_batt_restore;
static int32_t s_batt_restore_value;
static unsigned opens, closes, commits, writes, restarts;
static int fault;
static int nvs_open(const char *name,int mode,nvs_handle_t *out) {
    assert((!strcmp(name,"pmu") || !strcmp(name,"battcal")) && (mode==0 || mode==1));++opens;*out=1;return fault==1?ESP_FAIL:ESP_OK;
}
static int nvs_get_i32(nvs_handle_t h,const char *key,int32_t *v) {
    assert(h==1 && !strcmp(key,"soc"));*v=stored_soc;return fault==5?ESP_FAIL:ESP_OK;
}
static int nvs_get_u32(nvs_handle_t h,const char *key,uint32_t *v) {
    assert(h==1&&!strcmp(key,"soc_ver"));if(fault==6)return ESP_FAIL;*v=stored_ver;
    return ESP_OK;
}
static int nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *v) {
    assert(h==1&&!strcmp(key,"reset"));
    if(fault==9)return ESP_FAIL;
    if(!reset_present)return ESP_ERR_NVS_NOT_FOUND;
    *v=stored_reset;return ESP_OK;
}
static int nvs_set_u8(nvs_handle_t h,const char *key,uint8_t v) {
    assert(h==1&&!strcmp(key,"reset"));
    if(fault==2||(!v&&fail_reset_clear))return ESP_FAIL;
    stored_reset=v;reset_present=true;return ESP_OK;
}
static int nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size) {
    assert(h==1);
    if(!strcmp(key,"soc_state")){
        assert(*size==sizeof(stored_checkpoint));if(fault==7)return ESP_FAIL;
        if(!have_checkpoint)return ESP_ERR_NVS_NOT_FOUND;memcpy(out,&stored_checkpoint,*size);
        if(fault==8)--*size;return ESP_OK;
    }
    assert(*size==sizeof(batt_cal_record_t));unsigned slot=!strcmp(key,"slot1");
    if(!have_slots[slot])return ESP_ERR_NVS_NOT_FOUND;memcpy(out,&saved_slots[slot],*size);return ESP_OK;
}
static int nvs_set_blob(nvs_handle_t h,const char *key,const void *in,size_t size) {
    assert(h==1);if(fault==2||fault==3)return ESP_FAIL;
    if(!strcmp(key,"soc_state")){assert(size==sizeof(stored_checkpoint));memcpy(&stored_checkpoint,in,size);have_checkpoint=true;++writes;return ESP_OK;}
    assert(size==sizeof(batt_cal_record_t));
    if(fail_cal_write && cal_writes+1==fail_cal_write)return ESP_FAIL;
    unsigned slot=!strcmp(key,"slot1");memcpy(&saved_slots[slot],in,size);have_slots[slot]=true;++cal_writes;return ESP_OK;
}
static int64_t esp_timer_get_time(void) {return 0;}
static int nvs_commit(nvs_handle_t h) {assert(h==1);++commits;return fault==4?ESP_FAIL:ESP_OK;}
static void nvs_close(nvs_handle_t h) {assert(h==1);++closes;}
static void device_power_batt_log_event_stub(device_power_batt_log_event_t e) {assert(e==DEVICE_POWER_BATT_LOG_RESTART);}
static bool device_power_batt_log_flush_stub(uint32_t ms) {assert(ms==250);return true;}
#define device_power_batt_log_event device_power_batt_log_event_stub
#define device_power_batt_log_flush device_power_batt_log_flush_stub
esp_err_t device_power_power_off(void) {return ESP_OK;}
static void esp_restart(void) {++restarts;}
'''
persist += defines + '\n' + curve + '\n#define ESP_ERR_INVALID_STATE -3\n'
for name in ('batt_cal_read_slot', 'batt_cal_write_slot', 'batt_cal_read_reset', 'batt_cal_write_reset'):
    persist += function(name, backend) + '\n'
persist += 'static const batt_cal_storage_t s_batt_cal_storage={.read=batt_cal_read_slot,.write=batt_cal_write_slot,.read_reset=batt_cal_read_reset,.write_reset=batt_cal_write_reset};\n'
for name in ('batt_save', 'batt_load', 'batt_save_current', 'device_power_get_batt_calibration',
             'device_power_set_batt_calibration', 'device_power_reset_batt_calibration', 'device_power_action'):
    persist += function(name, backend) + '\n'
persist += r'''
int main(void) {
    batt_cal_init(&s_batt_cal,s_soc_curve,0);
    s_batt_display_model=batt_cal_model_id(&s_batt_cal);
    assert(batt_load()==-1 && s_batt_restore==3); // 不恢复旧曲线 / do not restore the old curve
    stored_ver=1;assert(batt_load()==-1); // 中低段重标后拒绝上一版百分比 / reject percentages from the prior curve
    stored_ver=2;assert(batt_load()==-1); // 本轮曲线不恢复版本2 / this curve rejects revision 2
    stored_ver=BATT_STATE_VERSION+1;assert(batt_load()==-1);
    stored_ver=BATT_STATE_VERSION;assert(batt_load()==600 && s_batt_restore==0 && s_batt_restore_value==600);
    stored_soc=-1;assert(batt_load()==-1);stored_soc=1001;assert(batt_load()==-1);
    stored_soc=600;
    for(int f=1;f<=6;f++){if(f!=1 && f!=5 && f!=6)continue;fault=f;assert(batt_load()==-1);}
    fault=0;batt_save(650);assert(s_batt_persisted_permille==650 && batt_load()==650);
    unsigned before=opens;batt_save(650);batt_save(-1);batt_save(1001);assert(opens==before);
    for(int f=1;f<=4;f++) {
        fault=f;s_batt_persisted_permille=-1;batt_save(700);
        assert(s_batt_persisted_permille==-1);
        fault=0;batt_save(700);assert(s_batt_persisted_permille==700 && batt_load()==700);
    }
    s_snap.soc_permille=720;before=opens;batt_save_current();assert(opens==before);
    s_snap.flags=PMU_STATUS_BATTERY_VALID;batt_save_current();assert(batt_load()==720);
    s_snap.soc_permille=730;
    assert(device_power_action(PMU_ACTION_HOST_NORMAL_RESTART,0,0)==ESP_OK);
    assert(restarts==1 && batt_load()==730);
    s_snap.soc_permille=740;
    assert(device_power_action(PMU_ACTION_HOST_HARD_RESET,0,0)==ESP_OK);
    assert(restarts==2 && batt_load()==740);
    assert(device_power_action(255,0,0)==ESP_ERR_NOT_SUPPORTED && restarts==2);
    stored_checkpoint.checksum^=1;assert(batt_load()==-1 && s_batt_restore==4);stored_checkpoint.checksum^=1;
    fault=7;assert(batt_load()==-1);fault=8;assert(batt_load()==-1);fault=0;
    stored_checkpoint.model^=1;
    stored_checkpoint.checksum=batt_cal_checksum(&stored_checkpoint,offsetof(batt_soc_checkpoint_t,checksum));
    assert(batt_load()==-1 && s_batt_restore==3);
    stored_checkpoint.model^=1;
    stored_checkpoint.checksum=batt_cal_checksum(&stored_checkpoint,offsetof(batt_soc_checkpoint_t,checksum));
    have_checkpoint=false;assert(batt_load()==600);
    s_batt_cal.record.soc[5]-=20;assert(batt_load()==-1);
    s_batt_display_model=batt_cal_model_id(&s_batt_cal);
    batt_save(740);assert(batt_load()==740);
    device_power_batt_calibration_status_t status;
    assert(device_power_set_batt_calibration(true)==ESP_OK);
    device_power_get_batt_calibration(&status);assert(status.enabled&&status.applied);
    assert(device_power_set_batt_calibration(false)==ESP_OK);
    device_power_get_batt_calibration(&status);assert(!status.enabled&&status.applied);
    for(int f=1;f<=4;++f){if(f==3)continue;fault=f;assert(device_power_set_batt_calibration(true)!=ESP_OK);assert(!s_batt_cal.record.enabled);}
    fault=0;assert(device_power_set_batt_calibration(true)==ESP_OK);
    fault=2;assert(device_power_reset_batt_calibration()!=ESP_OK);assert(s_batt_cal.record.soc[5]!=s_soc_curve[5][1]);
    fault=0;fail_cal_write=cal_writes+2;
    assert(device_power_reset_batt_calibration()!=ESP_OK);
    device_power_get_batt_calibration(&status);assert(status.enabled&&!status.applied&&status.save_error&&s_batt_cal.dirty);
    assert(stored_reset==BATT_CAL_RESET_ON);
    // 失败槽保留旧模型，且新槽在启动前损坏；重置意图仍须阻止回退。
    // The failed slot retains the old model; even if the new slot corrupts before boot, the intent bars fallback.
    saved_slots[s_batt_cal.slot].checksum^=1;
    fail_cal_write=0;
    batt_cal_init(&s_batt_cal,s_soc_curve,0);assert(batt_cal_load(&s_batt_cal,&s_batt_cal_storage));
    device_power_get_batt_calibration(&status);assert(status.enabled&&!status.applied&&status.save_error&&s_batt_cal.dirty);
    unsigned saved=cal_writes;
    assert(!batt_cal_save(&s_batt_cal,&s_batt_cal_storage,1,false)&&cal_writes==saved);
    fail_reset_clear=true;
    assert(!batt_cal_save(&s_batt_cal,&s_batt_cal_storage,BATT_CAL_SAVE_US,false));
    assert(cal_writes==saved+2&&s_batt_cal.dirty&&s_batt_cal.save_error&&stored_reset==BATT_CAL_RESET_ON);
    fail_reset_clear=false;
    assert(batt_cal_save(&s_batt_cal,&s_batt_cal_storage,BATT_CAL_SAVE_US,true));
    assert(cal_writes==saved+2&&!s_batt_cal.dirty&&!s_batt_cal.save_error&&!stored_reset);
    saved_slots[s_batt_cal.slot].checksum^=1;
    batt_cal_init(&s_batt_cal,s_soc_curve,0);assert(batt_cal_load(&s_batt_cal,&s_batt_cal_storage));
    device_power_get_batt_calibration(&status);assert(status.enabled&&!status.applied&&!status.sampled_bins);
    // 保留旧曲线的显示快照；在重置与下一次采样之间重启，不得按基线恢复旧值。
    // Restart between a reset and the next sample; do not restore the old display under the baseline.
    s_batt_cal.record.soc[5]-=20;
    s_batt_display_model=batt_cal_model_id(&s_batt_cal);
    s_snap.soc_permille=batt_cal_soc(&s_batt_cal,4000);
    assert(s_snap.soc_permille==675);
    saved=cal_writes;assert(device_power_reset_batt_calibration()==ESP_OK && cal_writes==saved+2);
    assert(device_power_action(PMU_ACTION_HOST_NORMAL_RESTART,0,0)==ESP_OK);
    batt_cal_init(&s_batt_cal,s_soc_curve,0);assert(batt_cal_load(&s_batt_cal,&s_batt_cal_storage));
    assert(stored_checkpoint.model==s_batt_display_model && batt_load()==-1 && s_batt_restore==3);
    fault=9;
    batt_cal_init(&s_batt_cal,s_soc_curve,0);assert(!batt_cal_load(&s_batt_cal,&s_batt_cal_storage));
    assert(s_batt_cal.save_error&&s_batt_cal.reset_state==BATT_CAL_RESET_UNREADABLE);
    assert(device_power_set_batt_calibration(true)==ESP_FAIL);
    fault=0;

    assert(writes && commits && closes<opens);
    puts("PASS: battery persistence, curve revision, invalid data, write failures/retries, write deduplication and restart checkpoint");
}
'''
with tempfile.TemporaryDirectory() as folder:
    out = Path(folder)
    (out / 'driver').mkdir()
    (out / 'driver/i2c_master.h').write_text('#pragma once\ntypedef void *i2c_master_bus_handle_t;\n')
    (out / 'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n')
    c, binary = out / 'test.c', out / 'test'
    c.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + folder, '-I' + str(root / 'components/device_power/include'),
                    '-I' + str(root / 'components/device_power'),
                    str(c), str(root / 'components/device_power/battery_log.c'),
                    str(root / 'components/device_power/battery_calibration.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    c.write_text(persist)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + folder, '-I' + str(root / 'components/device_power/include'),
                    '-I' + str(root / 'components/device_power'),
                    str(c), str(root / 'components/device_power/battery_calibration.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
