#!/usr/bin/env python3
"""中文：执行真实电量校准与充电档位设置页的交互。
English: Exercise real battery-calibration and charge-profile settings interactions.
"""
from pathlib import Path
import subprocess
import tempfile

from home_cover_cache_host_test import function
from run_reader_baseline import toolchain

ROOT = Path(__file__).resolve().parents[1]
unit = r'''
#include "device_power.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define EPD_DRAW_ALIGN_LEFT 0
#define UI_GRAY_WHITE 0xff
#define UI_GRAY_BLACK 0
#define UI_GRAY_LIGHT 0x90
#define UI_ICON_ZAP 0
#define UI_ICON_CHECK 1
#define APP_REDRAW_NONE 0
#define APP_REDRAW_PAGE 1
typedef int app_redraw_t;
typedef struct {int x,y,width,height;} EpdRect;
static bool s_batt_calibration_confirm;
static device_power_batt_calibration_status_t s_batt_calibration_status;
static device_power_batt_calibration_status_t status={.total_bins=21};
static char s_notice[96], painted[4096];
static unsigned queries, toggles, resets;
static bool fail, charge_layout;
static void append(const char *s){assert(strlen(painted)+strlen(s)+2<sizeof(painted));strcat(painted,s);strcat(painted,"\n");}
void device_power_get_batt_calibration(device_power_batt_calibration_status_t *out){++queries;*out=status;}
esp_err_t device_power_set_batt_calibration(bool enabled){++toggles;if(fail)return -1;status.enabled=enabled;return 0;}
esp_err_t device_power_reset_batt_calibration(void){++resets;if(fail)return -1;status.applied=false;status.sampled_bins=status.trusted_bins=0;return 0;}
static void back_header(uint8_t *fb,const char *s){(void)fb;append(s);}
static void section(uint8_t *fb,int y,const char *s){(void)fb;assert(y==438);append(s);}
static void setting_toggle(uint8_t *fb,int y,const char *s,const char *detail,bool enabled){(void)fb;assert(y==272&&enabled==status.enabled);append(s);append(detail);}
static void ui_text_box(uint8_t *fb,EpdRect r,int px,const char *s,int align,bool inverted){
    (void)fb;assert(align==0);
    if(charge_layout){
        assert(r.x>=36&&r.x+r.width<=648&&r.y>=212&&r.y+r.height<1112);
        assert(inverted==(r.y<366));
    }else{
        assert(r.x==48&&r.width==588&&r.y>=482&&r.y+r.height<=888&&r.height>=40&&px>=22&&px<=28);
    }
    append(s);
}
static void ui_draw_button(uint8_t *fb,EpdRect r,const char *s,bool enabled){(void)fb;(void)enabled;assert(r.x==36&&r.y==706&&r.width==612&&r.height==78);append(s);}
static void ui_nav_draw(uint8_t *fb,int page){(void)fb;assert(page==3);}
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height;}
static unsigned charge_current=DEVICE_POWER_CHARGE_1A, charge_changes;
static int selected_y, card_bottom;
unsigned device_power_charge_current(void){return charge_current;}
esp_err_t device_power_set_charge_current(device_power_charge_current_t current){
    ++charge_changes;if(fail)return -1;charge_current=current;return ESP_OK;
}
static void settings_card(uint8_t *fb,EpdRect r,int radius,uint8_t fill,uint8_t border){
    (void)fb;(void)radius;(void)border;
    assert(r.x==36&&r.width==612&&fill==UI_GRAY_WHITE);
    card_bottom=r.y+r.height;
}
static void ui_fill_round_rect(uint8_t *fb,EpdRect r,int rad,uint8_t fill){
    (void)fb;(void)rad;assert(r.x==36&&r.y==212&&r.width==612&&fill==UI_GRAY_BLACK);
}
static void ui_draw_icon(uint8_t *fb,int x,int y,int size,int icon,uint8_t ink){
    (void)fb;(void)x;(void)y;(void)size;assert((icon==UI_ICON_ZAP||icon==UI_ICON_CHECK)&&ink==UI_GRAY_WHITE);
}
static void epd_draw_circle(int x,int y,int r,uint8_t color,uint8_t *fb){
    (void)x;(void)y;(void)r;(void)color;(void)fb;
}
static void settings_divider(uint8_t *fb,int y,int x,int width){
    (void)fb;assert(y>card_bottom&&x==36&&width==612);
}
static void epd_fill_circle(int x,int y,int r,uint8_t color,uint8_t *fb){
    (void)x;(void)r;(void)color;(void)fb;selected_y=y;
}
'''
source = ROOT / "main/apps/app_device_settings.c"
text = source.read_text()
marker = "static const struct {\n    device_power_charge_current_t current;"
unit += text[text.index(marker):text.index("static EpdRect charge_choice_rect")]
for name in ("batt_calibration_render", "batt_calibration_touch", "charge_choice_rect", "charge_current_label",
             "charge_current_render", "charge_current_touch"):
    unit += function(name, source) + "\n"
unit += r'''
int main(void){
    uint8_t fb=0;
    batt_calibration_render(&fb);assert(queries==1&&!toggles&&!resets);
    assert(strstr(painted,"自动电量校准(beta)")&&strstr(painted,"学习已停止")&&strstr(painted,"0 / 21"));
    assert(batt_calibration_touch(48,300)==APP_REDRAW_PAGE&&status.enabled&&toggles==1);
    assert(strstr(s_notice,"已开启"));
    status.sampled_bins=4;status.trusted_bins=3;painted[0]=0;
    batt_calibration_render(&fb);assert(strstr(painted,"正在学习")&&strstr(painted,"4 / 21"));
    assert(batt_calibration_touch(48,300)==APP_REDRAW_PAGE&&!status.enabled&&!status.applied);
    painted[0]=0;batt_calibration_render(&fb);
    assert(strstr(painted,"学习已停止")&&!strstr(painted,"正在学习")&&strstr(painted,"4 / 21"));
    assert(batt_calibration_touch(48,300)==APP_REDRAW_PAGE&&status.enabled);
    status.applied=true;
    assert(batt_calibration_touch(48,300)==APP_REDRAW_PAGE&&!status.enabled&&status.applied);
    assert(strstr(s_notice,"已有校准继续生效"));painted[0]=0;
    batt_calibration_render(&fb);assert(strstr(painted,"已应用部分校准"));
    assert(batt_calibration_touch(48,720)==APP_REDRAW_PAGE&&s_batt_calibration_confirm&&!resets);
    painted[0]=0;batt_calibration_render(&fb);assert(strstr(painted,"确认重置校准"));
    assert(batt_calibration_touch(20,720)==APP_REDRAW_PAGE&&!s_batt_calibration_confirm&&!resets);
    assert(batt_calibration_touch(48,720)==APP_REDRAW_PAGE);
    assert(batt_calibration_touch(48,720)==APP_REDRAW_PAGE&&resets==1&&!status.applied&&!status.enabled);
    assert(strstr(s_notice,"学习开关保持不变"));
    fail=true;
    assert(batt_calibration_touch(48,300)==APP_REDRAW_PAGE&&!status.enabled&&strstr(s_notice,"保存失败"));
    assert(batt_calibration_touch(48,720)==APP_REDRAW_PAGE);
    assert(batt_calibration_touch(48,720)==APP_REDRAW_PAGE&&!s_batt_calibration_confirm&&strstr(s_notice,"未完整保存"));
    status.save_error=true;strcpy(s_notice,"旧的成功提示");painted[0]=0;
    batt_calibration_render(&fb);assert(strstr(painted,"校准保存失败")&&!strstr(painted,"旧的成功提示"));
    assert(batt_calibration_touch(48,800)==APP_REDRAW_NONE);
    charge_layout=true;fail=false;painted[0]=s_notice[0]=0;
    const EpdRect daily=charge_choice_rect(0), fast=charge_choice_rect(1);
    charge_current_render(&fb);
    assert(!charge_changes&&selected_y>daily.y&&selected_y<daily.y+daily.height);
    assert(strstr(painted,"当前档位\n日常充电\n")&&strstr(painted,"1 A · 默认")&&strstr(painted,"1.5 A"));
    assert(strstr(painted,"日常充电\n1 A · 默认\n")&&strstr(painted,"快速充电\n1.5 A\n"));
    assert(strstr(painted,"重启后保留"));
    assert(charge_current_touch(daily.x,daily.y)==APP_REDRAW_NONE&&!charge_changes);
    assert(charge_current_touch(daily.x,daily.y+daily.height)==APP_REDRAW_NONE&&!charge_changes);
    assert(charge_current_touch(fast.x-1,fast.y)==APP_REDRAW_NONE&&!charge_changes);
    assert(charge_current_touch(fast.x,fast.y)==APP_REDRAW_PAGE&&charge_changes==1&&charge_current==DEVICE_POWER_CHARGE_1_5A);
    painted[0]=0;charge_current_render(&fb);
    assert(selected_y>fast.y&&selected_y<fast.y+fast.height&&strstr(painted,"当前档位\n快速充电\n"));
    assert(charge_current_touch(fast.x,fast.y)==APP_REDRAW_NONE&&charge_changes==1);
    fail=true;
    assert(charge_current_touch(daily.x,daily.y)==APP_REDRAW_PAGE&&charge_current==DEVICE_POWER_CHARGE_1_5A&&charge_changes==2);
    painted[0]=0;charge_current_render(&fb);
    assert(selected_y>fast.y&&selected_y<fast.y+fast.height&&strstr(painted,"未能切换"));
    fail=false;
    assert(charge_current_touch(daily.x,daily.y)==APP_REDRAW_PAGE&&charge_current==DEVICE_POWER_CHARGE_1A&&charge_changes==3);
    assert(!s_notice[0]);
    charge_current=0;selected_y=0;painted[0]=0;
    charge_current_render(&fb);assert(!selected_y&&strstr(painted,"暂不可用")&&strstr(painted,"重启设备后再试"));
    assert(charge_current_touch(daily.x,daily.y)==APP_REDRAW_NONE&&charge_changes==3);
    puts("PASS: charge profile rendering, hit boundaries, application, failure and unavailable state");
    puts("PASS: pure calibration painting, coverage/states, retained model on disable, reset confirmation/cancel and persistence error feedback");
}
'''
with tempfile.TemporaryDirectory(prefix="battery-calibration-ui-") as directory:
    folder = Path(directory)
    env, _ = toolchain(folder)
    (folder / "driver").mkdir()
    (folder / "driver/i2c_master.h").write_text("#pragma once\ntypedef void *i2c_master_bus_handle_t;\n")
    (folder / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
    c, binary = folder / "test.c", folder / "test"
    c.write_text(unit)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I" + str(folder), "-Icomponents/device_power/include",
                    str(c), "-o", str(binary)], cwd=ROOT, env=env, check=True)
    subprocess.run([str(binary)], cwd=ROOT, env=env, check=True)
