#!/usr/bin/env python3
"""中文：执行实际关机图片任务，覆盖TF双槽、失卡、取消、坏缓存、内存和写入失败。
English: Exercise the real shutdown-art worker: SD dual slots, card loss, cancellation, corrupt caches and allocation/write failures.
"""
from pathlib import Path
import re
import subprocess
import tempfile
from run_reader_baseline import toolchain

ROOT = Path(__file__).resolve().parents[1]
settings = (ROOT / "main/settings/settings.h").read_text()
types = settings[settings.index("typedef enum {\n    APP_OFF_SYSTEM"):settings.index("/// 与浅睡锁屏独立")]
header = re.sub(r"^#(?:include|pragma)[^\n]*\n", "", (ROOT / "main/services/shutdown_wallpaper.h").read_text(), flags=re.M)
source = re.sub(r"^#include[^\n]*\n", "", (ROOT / "main/services/shutdown_wallpaper.c").read_text(), flags=re.M)
unit = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ui_image_dither.h"
#define BOOK_IMAGE_FILE_MAX (50u*1024u*1024u)
#define MM_OWNER_FILE_JOB 0
#define MM_OWNER_UI_CACHE 1
#define MM_POOL_PSRAM 1
#define pdPASS 1
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static bool locked;
#define portENTER_CRITICAL(x) do{(void)(x);assert(!locked);locked=true;}while(0)
#define portEXIT_CRITICAL(x) do{(void)(x);assert(locked);locked=false;}while(0)
static bool card_present=true, card_mounted=true;
typedef struct {bool present,mounted;} board_sd_info_t;
static int board_sd_get_info(board_sd_info_t *info){info->present=card_present;info->mounted=card_mounted;return 0;}
#define ESP_LOGW(...) ((void)0)
static unsigned allocations, fail_allocation, live, decodes, opens, writes, directories;
static bool space_fail, task_fail, write_fail, sync_fail, close_fail, readonly, directory_fail, decode_fail, mutate, cancel_decode, remove_in_decode;
static unsigned source_w=1200,source_h=600;
static int64_t now_us;
static void (*scheduled)(void *);
static void *mm_alloc(int owner,int pool,size_t bytes,size_t align,size_t reserve){
 (void)owner;(void)pool;(void)align;(void)reserve;assert(!locked);
 if(++allocations==fail_allocation)return NULL;
 void *p=malloc(bytes);if(p)++live;return p;
}
static bool mm_free(void *p){assert(!locked);if(p){assert(live);--live;free(p);}return true;}
static int xTaskCreate(void (*fn)(void *),const char *name,int stack,void *arg,int pri,void *handle){
 (void)arg;(void)handle;assert(!locked&&!scheduled&&stack==12288&&pri==1&&!strcmp(name,"off_art"));
 if(task_fail)return 0;scheduled=fn;return pdPASS;
}
static void vTaskDelete(void *arg){assert(!arg&&!locked);}
static void vTaskDelay(int ticks){assert(ticks==1&&!locked);now_us+=1000;}
static int64_t esp_timer_get_time(void){return now_us;}
static const char *mapped(const char *path,char out[512]){if(!strncmp(path,"/sdcard/",8)){snprintf(out,512,"card/%s",path+8);return out;}return path;}
static int stat_art(const char *path,struct stat *st){char out[512];assert(!locked&&!strncmp(path,"/sdcard/",8));return stat(mapped(path,out),st);}
static int mkdir_art(const char *path,mode_t mode){char out[512];++directories;assert(!locked&&!strncmp(path,"/sdcard/",8));if(directory_fail){errno=EROFS;return -1;}return mkdir(mapped(path,out),mode);}
static FILE *open_art(const char *path,const char *mode){char out[512];assert(!locked&&!strncmp(path,"/sdcard/",8));++opens;if(!card_present||!card_mounted){errno=ENODEV;return NULL;}if(readonly&&strchr(mode,'w')){errno=EROFS;return NULL;}return fopen(mapped(path,out),mode);}
static size_t write_art(const void *p,size_t s,size_t n,FILE *f){assert(!locked);++writes;if(space_fail){errno=ENOSPC;return 0;}return write_fail?0:fwrite(p,s,n,f);}
static int sync_art(int fd){return sync_fail?-1:fsync(fd);}
static int close_art(FILE *file){int result=fclose(file);return close_fail?-1:result;}
static bool flat_highlight;
typedef bool (*image_cancel_fn)(void *);
static bool book_image_file_dimensions(const char *p,bool png,unsigned *w,unsigned *h){(void)p;(void)png;*w=source_w;*h=source_h;return true;}
static bool book_image_file_grayscale_cancel(const char *p,bool png,unsigned w,unsigned h,uint8_t *out,image_cancel_fn cancel,void *ctx){
 (void)png;++decodes;assert(!locked);
 if(cancel_decode)shutdown_wallpaper_cancel();
 if(remove_in_decode){card_present=false;card_mounted=false;}
 if(mutate){char out_path[512];FILE *f=fopen(mapped(p,out_path),"ab");assert(f);fputc(1,f);fclose(f);}
 if(decode_fail||cancel(ctx))return false;
 for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)out[y*w+x]=flat_highlight?248:(uint8_t)(x*255u/w);
 return true;
}
static void run_worker(void){assert(scheduled);void (*fn)(void *)=scheduled;scheduled=NULL;fn(NULL);assert(!locked);}
'''
crop = re.sub(r"^#(?:include|pragma)[^\n]*\n", "", (ROOT / "main/ui/ui_wallpaper_crop.h").read_text(), flags=re.M)
unit = unit.replace("static bool book_image_file_grayscale_cancel(", "void shutdown_wallpaper_cancel(void);\nstatic bool book_image_file_grayscale_cancel(")
unit += types + header + crop + r'''
typedef ui_wallpaper_crop_t book_crop_t;
static book_crop_t book_cover_crop(unsigned w,unsigned h,unsigned dw,unsigned dh){return ui_wallpaper_center_crop(w,h,dw,dh);}
bool app_settings_shutdown_wallpaper_valid(const app_shutdown_wallpaper_t *v){return v&&v->slot<2&&v->fit<2&&v->style<=APP_OFF_CALENDAR&&v->style!=2&&v->image_id&&v->path[0];}
#define stat stat_art
#define fopen open_art
#define fwrite write_art
#define fsync sync_art
#define mkdir(...) mkdir_art(__VA_ARGS__)
#define fclose close_art
'''
# 宏不能改写struct stat类型名。/ Do not let a function macro rename the struct stat tag.
unit = unit.replace("#define stat stat_art", "#define stat(...) stat_art(__VA_ARGS__)")
unit += source + r'''
#undef stat
#undef fopen
#undef fwrite
#undef fsync
#undef mkdir
#undef fclose
static uint8_t pixels[SHUTDOWN_ART_BYTES],original[SHUTDOWN_ART_BYTES];
static app_shutdown_wallpaper_t take(bool expected){app_shutdown_wallpaper_t result;bool ok=false;uint8_t *thumb=NULL;
 assert(shutdown_wallpaper_take(&result,&ok,&thumb)&&ok==expected);if(ok)assert(thumb||fail_allocation);mm_free(thumb);return result;}
static unsigned gray_at(unsigned x,unsigned y){unsigned at=y*684+x;return (pixels[at/2]>>((at&1)*4))&15;}
int main(void){
 assert(!mkdir("card",0700)&&!mkdir("card/pictures",0700));
 FILE *f=fopen("card/pictures/one.png","wb");assert(f);fputs("fixture",f);fclose(f);
 app_shutdown_wallpaper_t value={.style=APP_OFF_CUSTOM,.slot=1,.show_hint=1};strcpy(value.path,"/sdcard/pictures/one.png");
 unsigned before=opens;shutdown_wallpaper_request(&value,true);assert(opens==before&&scheduled);run_worker();value=take(true);
 assert(value.image_id&&shutdown_wallpaper_read(&value,pixels));memcpy(original,pixels,sizeof(pixels));
 assert(gray_at(0,0)>3&&gray_at(683,1215)<12);
 // 重启后从同一张卡恢复缓存，不依赖机内卷；换图只写非活动槽。
 // Restore from the same card after reboot without an internal volume; replacements write only the inactive slot.
 unsigned before_decode=decodes;
 shutdown_wallpaper_request(&value,false);run_worker();take(true);assert(decodes==before_decode);
 app_shutdown_wallpaper_t next=value;next.slot=0;next.fit=1;
 shutdown_wallpaper_request(&next,true);run_worker();next=take(true);assert(shutdown_wallpaper_read(&next,pixels));
 assert(gray_at(0,0)==15&&gray_at(683,1215)==15&&gray_at(0,607)==0&&gray_at(683,607)>13);
 assert(shutdown_wallpaper_read(&value,pixels)&&!memcmp(pixels,original,sizeof(pixels)));
 // 无卡或未挂载时不创建目录、解码或读写文件；重新就绪后可重试。
 // Absent/unmounted cards cause no directory creation, decoding or file I/O; readiness permits retries.
 for(unsigned state=0;state<2;++state){
  card_present=state==1;card_mounted=state==0;
  unsigned opened=opens,written=writes,created=directories;before_decode=decodes;
  assert(!shutdown_wallpaper_read(&value,pixels));
  shutdown_wallpaper_request(&next,true);run_worker();take(false);
  assert(!strcmp(shutdown_wallpaper_error(),"TF卡未就绪")&&decodes==before_decode&&opens==opened&&writes==written&&directories==created);
 }
 card_present=card_mounted=true;
 assert(shutdown_wallpaper_read(&value,pixels)&&!memcmp(pixels,original,sizeof(pixels)));
 unsigned decoded=decodes;shutdown_wallpaper_request(&next,false);run_worker();take(true);assert(decodes==decoded);
 assert(!remove("card/pictures/one.png"));assert(shutdown_wallpaper_read(&value,pixels));
 assert(!memcmp(pixels,original,sizeof(pixels)));f=fopen("card/pictures/one.png","wb");assert(f);fputs("fixture",f);fclose(f);
 space_fail=true;shutdown_wallpaper_request(&next,true);run_worker();take(false);
 assert(!strcmp(shutdown_wallpaper_error(),"TF卡空间不足"));space_fail=false;
 assert(shutdown_wallpaper_read(&value,pixels)&&!memcmp(pixels,original,sizeof(pixels)));
 shutdown_wallpaper_request(&next,true);run_worker();next=take(true);
 for(unsigned fault=0;fault<10;++fault){
  write_fail=fault==0;sync_fail=fault==1;close_fail=fault==2;decode_fail=fault==3;mutate=fault==4;
  readonly=fault==7;directory_fail=fault==8;remove_in_decode=fault==9;
  allocations=0;fail_allocation=fault==5?1:fault==6?2:0;
  shutdown_wallpaper_request(&next,true);run_worker();take(false);
  card_present=card_mounted=true;
  assert(shutdown_wallpaper_read(&value,pixels)&&!memcmp(pixels,original,sizeof(pixels)));
 }
 write_fail=sync_fail=close_fail=decode_fail=mutate=readonly=directory_fail=remove_in_decode=false;fail_allocation=0;
 shutdown_wallpaper_request(&next,true);shutdown_wallpaper_cancel();run_worker();assert(!s_done);
 cancel_decode=true;shutdown_wallpaper_request(&next,true);run_worker();assert(!s_done);cancel_decode=false;
 shutdown_wallpaper_request(&next,true);assert(!shutdown_wallpaper_quiesce(1));run_worker();assert(shutdown_wallpaper_quiesce(1));
 task_fail=true;shutdown_wallpaper_request(&next,true);take(false);task_fail=false;
 assert(!rename("card/.readpico","card/saved-cache"));
 assert(!shutdown_wallpaper_read(&value,pixels));
 shutdown_wallpaper_request(&value,false);run_worker();take(false);assert(!strcmp(shutdown_wallpaper_error(),"TF壁纸不可用"));
 assert(!rename("card/saved-cache","card/.readpico"));
 assert(shutdown_wallpaper_read(&value,pixels)&&!memcmp(pixels,original,sizeof(pixels)));
 app_shutdown_wallpaper_t bad=value;bad.image_id^=1;assert(!shutdown_wallpaper_read(&bad,pixels));
 char path[128],disk[512];art_path(path,value.slot);f=fopen(mapped(path,disk),"r+b");assert(f);assert(!fseek(f,sizeof(off_header_t)+123,SEEK_SET));int c=fgetc(f);assert(!fseek(f,-1,SEEK_CUR));fputc(c^1,f);fclose(f);
 assert(!shutdown_wallpaper_read(&value,pixels));
 shutdown_wallpaper_request(&value,true);run_worker();value=take(true);
 art_path(path,value.slot);f=fopen(mapped(path,disk),"ab");assert(f);fputc(1,f);fclose(f);assert(!shutdown_wallpaper_read(&value,pixels));
 // 新图从8位亮度调色再抖动；读回不能重复调色。 / Tone new art before 4bpp dithering; do not retone it on read.
 flat_highlight=true;value.fit=0;shutdown_wallpaper_request(&value,true);run_worker();value=take(true);
 assert(shutdown_wallpaper_read(&value,pixels));
 for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)
  assert(gray_at(x,y)==ui_image_dither_gray(ui_image_wallpaper_tone(248),x,y)/17u);
 uint8_t *thumb=art_thumbnail(pixels);assert(thumb);
 unsigned sum=0;
 for(unsigned i=0;i<THUMB_BYTES;++i)sum+=(thumb[i]&15u)+(thumb[i]>>4);
 assert(sum*17u>=239u*THUMB_WIDTH*THUMB_HEIGHT&&sum*17u<=244u*THUMB_WIDTH*THUMB_HEIGHT);
 mm_free(thumb);
 // 原图删除且TF只读时，旧双槽缓存仍可验证并只在内存中调色。
 // Legacy dual-slot art works without its source on a read-only card and is toned only in memory.
 assert(!remove("card/pictures/one.png"));
 memset(original,0xee,sizeof(original));original[0]=0xf0;original[1]=0x76;
 const uint32_t legacy_versions[]={ART_MAGIC_LEGACY,ART_MAGIC_BASE_TONE};
 for(unsigned v=0;v<sizeof(legacy_versions)/sizeof(legacy_versions[0]);++v){
  off_header_t legacy={.magic=legacy_versions[v],.width=SHUTDOWN_ART_WIDTH,.height=SHUTDOWN_ART_HEIGHT,
   .fit=value.fit,.image_id=value.image_id,.checksum=art_hash(2166136261u,original,sizeof(original))};
  art_path(path,value.slot);f=fopen(mapped(path,disk),"wb");assert(f);
  assert(fwrite(&legacy,1,sizeof(legacy),f)==sizeof(legacy)&&fwrite(original,1,sizeof(original),f)==sizeof(original)&&!fclose(f));
  readonly=true;unsigned written=writes;before_decode=decodes;
  assert(shutdown_wallpaper_read(&value,pixels)&&writes==written&&decodes==before_decode);
  assert(gray_at(0,0)==0&&gray_at(1,0)==15);
  if(v==1)assert(pixels[1]==0x76);
  unsigned toned=0;
  for(unsigned y=4;y<8;++y)for(unsigned x=0;x<4;++x)toned+=gray_at(x,y);
  assert(toned<14u*16u&&toned>=12u*16u);
  assert(shutdown_wallpaper_read(&value,pixels)&&writes==written&&decodes==before_decode);
  f=fopen(mapped(path,disk),"rb");assert(f&&fread(&legacy,1,sizeof(legacy),f)==sizeof(legacy));
  assert(legacy.magic==legacy_versions[v]&&fread(pixels,1,sizeof(pixels),f)==sizeof(pixels)&&!fclose(f));
  assert(!memcmp(pixels,original,sizeof(pixels)));
  readonly=false;
 }
 shutdown_wallpaper_cancel();assert(!live&&!locked);
 puts("PASS: highlight tone/dither and source-free legacy tone compatibility; independent shutdown cache, portrait crop/fit, inactive-slot failures, reboot/card readiness retries, full/read-only SD, card removal during decode, source-free cached reads, corrupt/trailing caches, cancellation, OOM and task failures");
}
'''
with tempfile.TemporaryDirectory(prefix="shutdown-art-") as directory:
    folder = Path(directory)
    env, _ = toolchain(folder)
    source_path = folder / "test.c"
    source_path.write_text(unit)
    binary = folder / "test"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I" + str(ROOT / "main/ui"), str(source_path),
                    str(ROOT / "main/ui/ui_image_dither.c"), "-o", str(binary)],
                   env=env, check=True)
    subprocess.run([str(binary)], cwd=folder, env=env, check=True)
