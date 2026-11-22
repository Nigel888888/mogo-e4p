#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
# 中文：执行启动清白、开机图和旧关机恢复记录回归。
# English: Exercise the boot white baseline, splash and legacy shutdown resume records.
from pathlib import Path
import subprocess,tempfile
from home_cover_cache_host_test import function
r=Path(__file__).resolve().parents[1]
s=(r/'main/app_main.c').read_text()
assert 'const bool resuming = pico_boot_has_resume();\n    present_boot_frame(&hl, framebuffer, resuming);' in s
assert 'if(!resuming || lock_pin_enabled())app_font_activate_system();' in s
assert 'if(first==app_at(1) && resume.reader)app_font_activate_reading();' in s
u=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef struct{uint8_t *front_fb,*back_fb;} EpdiyHighlevelState;
enum {UI_BUILTIN_IMAGE_LOADING};
static unsigned power,census,white,splash,output;
static void epd_poweron(void){assert(!power&&!white);++power;}
static void board_i2c_census_take(void){assert(power==1&&!white);++census;}
static void epd_hl_set_all_white(EpdiyHighlevelState *hl){assert(power==1&&!splash);*hl->front_fb=0xff;++white;}
static bool ui_builtin_image_draw(uint8_t *fb,int image){assert(white==1&&*fb==0xff&&image==UI_BUILTIN_IMAGE_LOADING);*fb=0x02;++splash;return true;}
static void update_display_boot_frame(EpdiyHighlevelState *hl){assert(splash==1&&*hl->front_fb==0x02&&*hl->back_fb==0xa5);++output;}
'''+function('present_boot_frame',r/'main/app_main.c')+r'''
int main(void){
 for(int resuming=0;resuming<2;++resuming){
  uint8_t front=0x37,back=0xa5;EpdiyHighlevelState hl={&front,&back};
  power=census=white=splash=output=0;
  present_boot_frame(&hl,&front,resuming);
  assert(power==1&&census==!resuming&&white==1&&splash==1&&output==1);
 }
 puts("PASS: regular and timed-shutdown resume boot rebuild the optical baseline, without seeding back from lock art");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(u);out=p.with_suffix('')
 cc=['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined']
 subprocess.run([*cc,str(p),'-o',str(out)],cwd=r,check=True)
 subprocess.run([str(out)],check=True)
 subprocess.run([*cc,'-D_DEFAULT_SOURCE','-Itools/settings_backup_stubs','-Imain','-Imain/boot',
                 'tools/boot_state_host_test.c','-o',str(out)],cwd=r,check=True)
 subprocess.run([str(out)],check=True)
