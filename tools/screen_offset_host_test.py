#!/usr/bin/env python3
"""中文：扫描行、四向触点与校正页的真实实现回归。
English: Regress actual scan rows, four-way contacts and calibration UI.
"""
from pathlib import Path
import re
import subprocess
import tempfile
from home_cover_cache_host_test import function

ROOT = Path(__file__).resolve().parents[1]

scan = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "epd_output_offset.h"
#include "display_orientation.h"
#include "epdiy.h"
#define IRAM_ATTR
#define CONFIG_IDF_TARGET_ESP32S31 1
#define NUM_RENDER_THREADS 2
#define EPD_DRAW_EMPTY_LINE_QUEUE 1
#define MODE_PACKING_1PPB_DIFFERENCE 1
#define MODE_PACKING_2PPB 2
#define MODE_PACKING_8PPB 4
#define EPD_DRAW_INVALID_PACKING_MODE 8
#define int_min(a,b) ((a)<(b)?(a):(b))
#define Cache_Start_L1_DCache_Preload(a,b,c) ((void)0)
typedef struct {int size,element_size;} LineQueue_t;
typedef struct {
 EpdRect area,crop_to;int mode,display_width,display_height,offset_x,offset_y;
 const uint8_t *data_ptr;const bool *drawn_lines;int error;bool frame_ended;
 atomic_int lines_prepared;int lines_total;int line_threads[48];
 uint8_t *feed_line_buffers[2],*line_mask;LineQueue_t line_queues[2];
 bool lut_msb_first;uint8_t *static_line_buffer;
 const uint8_t *conversion_lut;const uint8_t *const *phase_luts;const int8_t *line_phase;
 const uint8_t *line_outer_lut;int line_inner_x0,line_inner_x1;
 int col_band_n;const int *col_band_x0,*col_band_x1;const int8_t *col_band_phase;
 void (*lut_lookup_func)(const uint32_t *,uint8_t *,const uint8_t *,uint32_t);
} RenderContext_t;
static RenderContext_t *active;
_Alignas(16) static uint8_t output[48][16];static int commits,starts;
static int s_prefill_lines=4;
static void lq_reset(LineQueue_t *q){(void)q;}
static uint8_t *lq_current(LineQueue_t *q){(void)q;return output[active->lines_prepared-1];}
static void lq_commit(LineQueue_t *q){(void)q;++commits;}
static bool retrieve_line_isr(RenderContext_t *c,uint8_t *b){(void)c;(void)b;return false;}
typedef bool (*line_cb_func_t)(RenderContext_t *,uint8_t *);
static void epd_lcd_line_source_cb(line_cb_func_t cb,RenderContext_t *c){(void)cb;(void)c;}
static void epd_lcd_start_frame(void){++starts;}
void epd_apply_line_mask_VE(uint8_t *row,const uint8_t *mask,int bytes);
static void epd_apply_line_mask(uint8_t *row,const uint8_t *mask,int bytes){epd_apply_line_mask_VE(row,mask,bytes);}
static int min(int a,int b){return a<b?a:b;}
#define DARK_BYTE 0x55
#define CLEAR_BYTE 0xaa
static void lookup(const uint32_t *p,uint8_t *out,const uint8_t *lut,uint32_t width){
 const uint8_t *in=(const uint8_t *)p;
 for(unsigned x=0;x<width;x+=4){out[x/4]=0;for(unsigned k=0;k<4;++k)out[x/4]|=((in[x+k]+*lut)&3)<<(6-2*k);}
}
'''
render_context = ROOT / "components/epdiy/src/output_common/render_context.c"
for name in ("get_buffer_params", "epd_populate_line_mask"):
    scan += function(name, render_context) + "\n"
scan += (ROOT / "components/epdiy/src/epdiy_riscv_ve_stub.c").read_text().replace(
    "#include <esp_attr.h>", "").replace('__attribute__((optimize("O3"))) ', "")
lut = ROOT / "components/epdiy/src/output_common/lut.c"
for signature in ("static inline uint8_t lookup_pixels_in_VE_LUT(",
                  "void IRAM_ATTR calc_epd_input_1ppB_1k_S3_VE(",
                  "void IRAM_ATTR calc_epd_input_1ppB_64k("):
    text = lut.read_text()
    start = text.index(signature)
    scan += text[start:text.index("\n}", start) + 2] + "\n"
scan += function("push_pixels_populate_line", ROOT / "components/epdiy/src/output_lcd/render_lcd.c") + "\n"
feeder = (ROOT / "components/epdiy/src/output_lcd/render_lcd.c").read_text()
scan += "static void " + feeder[feeder.index("lookup_line_with_outer("):feeder.rindex("\n#endif")].replace('__attribute__((optimize("O3"))) ', "")
scan += r'''
static enum EpdRotation rotation=EPD_ROT_INVERTED_PORTRAIT;
int epd_width(void){return 1216;}int epd_height(void){return 684;}
enum EpdRotation epd_get_rotation(void){return rotation;}
void epd_set_rotation(enum EpdRotation r){rotation=r;}
static unsigned pixel(const uint8_t *p,int x,bool msb){return (p[x/4]>>(msb?6-2*(x%4):2*(x%4)))&3;}
static void shift_test(void){
 for(int msb=0;msb<2;++msb)for(int dx=-100;dx<=100;++dx){
  uint8_t row[16],original[16];for(int i=0;i<16;++i)row[i]=original[i]=(uint8_t)(i*73+19);
  epd_output_shift_line(row,16,dx,msb);
  for(int x=0;x<64;++x)assert(pixel(row,x,msb)==(x-dx>=0&&x-dx<64?pixel(original,x-dx,msb):0));
 }
}
static void scan_test(void){
 uint8_t data[40][64],input[64],base=0,one=1,two=2;
 _Alignas(16) uint8_t mask[16];
 bool dirty[40];int8_t phases[40];const uint8_t *luts[]={&one,&two};
 const int x0[]={0,32},x1[]={32,64};const int8_t band_phase[]={0,-1};
 for(int y=0;y<40;++y){dirty[y]=y%3!=1;phases[y]=y%2;for(int x=0;x<64;++x)data[y][x]=(uint8_t)(x+y*3);}
 for(int i=0;i<16;++i)mask[i]=(uint8_t)(i%2?0xf3:0xcf);
 for(int mode=0;mode<4;++mode)for(int dx=-32;dx<=32;++dx)for(int dy=-32;dy<=32;++dy){
  RenderContext_t ctx={.area={0,0,64,40},.crop_to={0,3,64,34},.mode=1,.display_width=64,.display_height=40,
   .offset_x=dx,.offset_y=dy,.data_ptr=&data[0][0],.drawn_lines=dirty,.lines_total=44,
   .feed_line_buffers={input,input},.line_queues={{80,16},{80,16}},.line_mask=mask,
   .lut_lookup_func=lookup,.lut_msb_first=true,.conversion_lut=&base};
  if(mode==1||mode==3){ctx.phase_luts=luts;ctx.line_phase=phases;}
  for(int y=0;y<40;++y)phases[y]=mode==3&&y%4==0?-1:y%2;
  if(mode==3){ctx.line_outer_lut=&base;ctx.line_inner_x0=13;ctx.line_inner_x1=41;}
  if(mode==2){ctx.phase_luts=luts;ctx.col_band_n=2;ctx.col_band_x0=x0;ctx.col_band_x1=x1;ctx.col_band_phase=band_phase;}
  active=&ctx;commits=starts=0;memset(output,0xa5,sizeof(output));lcd_calculate_frame(&ctx,0);
  assert(commits==44&&starts==1&&!ctx.error);
  for(int y=0;y<44;++y)for(int x=0;x<64;++x){
   int sy=y-dy,sx=x-dx;unsigned want=0;
   if(y<40&&sy>=3&&sy<37&&dirty[sy]&&sx>=0&&sx<64){
    if(mode!=2||sx<32){int phase=(mode==1||(mode==3&&sx>=13&&sx<41))?1+phases[sy]:mode==2?1:0;
     want=mode==3&&sx>=13&&sx<41&&phases[sy]<0?0:(data[sy][sx]+phase)&3;
     want&=(mask[sx/4]>>(6-2*(sx%4)))&3;
    }
   }
   if(pixel(output[y],x,true)!=want){fprintf(stderr,"mode=%d dx=%d dy=%d x=%d y=%d got=%u want=%u\n",mode,dx,dy,x,y,pixel(output[y],x,true),want);assert(0);}
  }
 }
}

static void physical_difference_test(void){
 uint8_t front[32*20/2],back[sizeof(front)],diff[32*20];
 for(unsigned i=0;i<sizeof(front);++i){front[i]=(uint8_t)(i*31+13);back[i]=(uint8_t)(i*7+5);}
 const int values[]={-32,-1,0,1,32};
 for(int a=0;a<5;++a)for(int b=0;b<5;++b)for(int c=0;c<5;++c)for(int d=0;d<5;++d){
  int ox=values[a],oy=values[b],nx=values[c],ny=values[d];bool changed=false;
  bool result=epd_output_offset_difference(front,back,diff,32,20,ox,oy,nx,ny);
  for(int y=0;y<20;++y)for(int x=0;x<32;++x){
   int ax=x-ox,ay=y-oy,bx=x-nx,by=y-ny;unsigned from=15,to=15;
   if(ax>=0&&ax<32&&ay>=0&&ay<20)from=(back[ay*16+ax/2]>>((ax%2)*4))&15;
   if(bx>=0&&bx<32&&by>=0&&by<20)to=(front[by*16+bx/2]>>((bx%2)*4))&15;
   assert(diff[y*32+x]==((to<<4)|from));changed|=to!=from;
  }
  assert(result==changed);
 }
 memset(front,255,sizeof(front));memset(back,255,sizeof(back));
 assert(!epd_output_offset_difference(front,back,diff,32,20,-1,1,1,-1));
 for(unsigned i=0;i<sizeof(diff);++i)assert(diff[i]==255);
}
static void mask_test(void){
 uint8_t from[32],to[32],dirty_columns[32],input[64];
 _Alignas(16) uint8_t mask[16];
 _Alignas(16) uint32_t diff[16];
 static uint8_t lut_64k[1 << 16];
 uint8_t lut_1k[1024]={0};lut_1k[0x0f*4]=1;
 lut_64k[0xff0f]=1;lut_64k[0x0fff]=4;
 memset(from,255,sizeof(from));
 const bool dirty_lines[]={true};
 for(int pack=0;pack<2;++pack)for(int dx=-1;dx<=1;++dx)for(int x=0;x<8;++x){
  memset(to,255,sizeof(to));memset(dirty_columns,0,sizeof(dirty_columns));
  to[x/2]&=(uint8_t)~(0x0f << (4*(x%2)));
  assert(epd_interlace_4bpp_line_VE(to,from,(uint8_t*)diff,dirty_columns,64));
  epd_populate_line_mask(mask,dirty_columns,16,pack==0);
  RenderContext_t ctx={.area={0,0,64,1},.crop_to={0,0,64,1},.mode=MODE_PACKING_1PPB_DIFFERENCE,
   .display_width=64,.display_height=1,.offset_x=dx,.data_ptr=(const uint8_t*)diff,
   .drawn_lines=dirty_lines,.lines_total=1,.feed_line_buffers={input,input},
   .line_queues={{80,16},{80,16}},.line_mask=mask,.lut_msb_first=pack==0,
   .conversion_lut=pack==0?lut_1k:lut_64k,
   .lut_lookup_func=pack==0?calc_epd_input_1ppB_1k_S3_VE:calc_epd_input_1ppB_64k};
  active=&ctx;commits=starts=0;lcd_calculate_frame(&ctx,0);
  assert(commits==1&&!ctx.error);
  for(int px=0;px<64;++px)assert(pixel(output[0],px,pack==0)==(px==x+dx));
 }
 for(int x=0;x<8;++x){
  _Alignas(16) uint8_t line[16];
  RenderContext_t ctx={.area={x,0,1,1},.display_width=64,.line_mask=mask,.static_line_buffer=line};
  push_pixels_populate_line(&ctx,1);
  for(int px=0;px<64;++px)assert(pixel(line,px,true)==(px==x?2:0));
 }
}
static void touch_test(void){
 const int values[]={-32,-1,0,1,32};
 for(int direction=0;direction<4;++direction)for(int a=0;a<5;++a)for(int b=0;b<5;++b){
  int dx=values[a],dy=values[b];epd_set_output_offset(dy,-dx);display_orientation_set(direction);
  for(int y=0;y<1216;y+=13)for(int x=0;x<684;x+=11){
   uint16_t xx=x,yy=y;bool mapped=display_orientation_touch(&xx,&yy);
   int sx=x-dx,sy=y-dy;
   if(sx<0||sx>=684||sy<0||sy>=1216){assert(!mapped&&xx==x&&yy==y);continue;}
   assert(mapped);
   if(direction==0)assert(xx==sx&&yy==sy);
   if(direction==1)assert(xx==sy&&yy==683-sx);
   if(direction==2)assert(xx==683-sx&&yy==1215-sy);
   if(direction==3)assert(xx==1215-sy&&yy==sx);
  }
  uint16_t x=220,y=1500;assert(display_orientation_touch(&x,&y)&&x==220&&y==1500);
 }
}
int main(void){shift_test();scan_test();mask_test();physical_difference_test();touch_test();puts("PASS: packed drive shifts, actual interlace/LUT/dirty masks and feeder crops/phases/bands/padding and four-way touch alignment");}
'''

ui = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "app_input.h"
#include "display_orientation.h"
#include "epd_output_offset.h"
enum {SETTINGS_MAIN,SETTINGS_SCREEN_OFFSET,DISPLAY_REFRESH_FEEDBACK,DISPLAY_REFRESH_DEFAULT};
#define APP_SCREEN_OFFSET_LIMIT 32
#define UI_GRAY_BLACK 0
#define UI_GRAY_WHITE 255
static int s_page=SETTINGS_SCREEN_OFFSET,px,py,prepared,blocks,buttons,feedback,inverted,ruler_labels;
static int s_screen_offset_pressed=-1;
static unsigned s_screen_offset_retries;
static int64_t now_ms,s_screen_offset_retry_ms;
static int saves,scans;
static bool fail,guides,fail_scan;static char s_notice[96];
static char painted_notice[96];
static void ui_clear_page(uint8_t *fb){(void)fb;}
int64_t esp_timer_get_time(void){return now_ms*1000;}
#define ESP_LOGW(tag, ...) ((void)0)
enum EpdRotation { EPD_ROT_INVERTED_PORTRAIT=3 };
static enum EpdRotation rotation=EPD_ROT_INVERTED_PORTRAIT;
int epd_width(void){return 1216;}int epd_height(void){return 684;}
enum EpdRotation epd_get_rotation(void){return rotation;}
void epd_set_rotation(enum EpdRotation r){rotation=r;}
static void display_get_screen_offset(int *x,int *y){int nx,ny;epd_get_output_offset(&nx,&ny);*x=-ny;*y=nx;}
static enum EpdDrawError update_display_screen_offset(display_state_t *hl,int x,int y){
 (void)hl;++scans;if(fail_scan)return EPD_DRAW_INVALID_CROP;epd_set_output_offset(y,-x);return EPD_DRAW_SUCCESS;
}
int ui_key_hit_test(uint16_t x,uint16_t y){return x<684&&y>=1216?x/228:-1;}

static void back_header(uint8_t *fb,const char *t){(void)fb;(void)t;}
void epd_fill_rect(EpdRect r,uint8_t c,uint8_t *f){(void)f;if(guides){assert(c==0&&r.x>=0&&r.y>=0&&r.x+r.width<=684&&r.y+r.height<=1216);int n=blocks/4+1,k=blocks%4;assert(n<=32);
 if(k==0)assert(r.x==70+(n-1)*17&&r.y==0&&r.width==16&&r.height==n);
 if(k==1)assert(r.x==70+(n-1)*17&&r.y==1216-n&&r.width==16&&r.height==n);
 if(k==2)assert(r.x==0&&r.y==210+(n-1)*24&&r.width==n&&r.height==23);
 if(k==3)assert(r.x==684-n&&r.y==210+(n-1)*24&&r.width==n&&r.height==23);
 ++blocks;}}
static void epd_draw_hline(int x,int y,int w,int c,uint8_t *f){(void)x;(void)y;(void)w;(void)c;(void)f;}
static void epd_draw_vline(int x,int y,int w,int c,uint8_t *f){(void)x;(void)y;(void)w;(void)c;(void)f;}
static void ui_text_fixed_vc(uint8_t *fb,int x,int y,int size,const char *text,int align,bool invert){(void)fb;(void)x;(void)y;(void)size;(void)text;(void)align;if(y==1012)snprintf(painted_notice,sizeof(painted_notice),"%s",text);if(guides){int n=0;assert(sscanf(text,"%d",&n)==1&&n==ruler_labels/4+1);++ruler_labels;}inverted+=invert;}
static void ui_fill_round_rect(uint8_t *fb,EpdRect r,int radius,int gray){(void)fb;(void)r;(void)radius;assert(gray==0||gray==255);}
static void ui_draw_control_frame(uint8_t *fb,EpdRect r,int radius,int gray){(void)fb;(void)r;(void)radius;assert(gray==0);++buttons;}
static int display_refresh_area(void *hl,int intent,EpdRect area,int options){(void)hl;assert(intent==DISPLAY_REFRESH_FEEDBACK&&options==DISPLAY_REFRESH_DEFAULT&&area.width>=200&&area.height>=78);++feedback;return 0;}
void guard_draw_result(display_state_t *hl,enum EpdDrawError result){(void)hl;assert(result==0);}
static void app_settings_screen_offset(int *x,int *y){*x=px;*y=py;}
static bool app_settings_set_screen_offset(int x,int y){++saves;if(fail)return false;px=x;py=y;return true;}
static void settings_prepare_profile(void){++prepared;}
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height;}
'''
ui += (ROOT / "main/display/display_orientation.c").read_text().replace('#include "epdiy.h"', "")
settings = ROOT / "main/apps/app_device_settings.c"
ui += re.search(r"^enum \{ SCREEN_OFFSET_RETRIES[^\n]+", settings.read_text(), re.M).group(0) + "\n"
for name in ("screen_offset_button", "screen_offset_guides", "screen_offset_draw_button",
             "screen_offset_feedback", "screen_offset_render", "screen_offset_present",
             "screen_offset_retry_if_due", "settings_gesture_contact_allowed", "screen_offset_gesture"):
    ui += function(name, settings) + "\n"
ui += r'''
static int events[UI_GESTURE_RELEASE+1];
static app_redraw_t capture(app_ctx_t *ctx,const ui_gesture_event_t *e){++events[e->type];return screen_offset_gesture(ctx,e);}
void app_present(app_ctx_t *ctx,const app_desc_t *page,app_redraw_t redraw){(void)ctx;(void)page;(void)redraw;}
static void present_event(app_ctx_t *ctx,const app_desc_t *page,ui_gesture_t *g,app_redraw_t redraw){(void)g;app_present(ctx,page,redraw);}
static void contact_test(bool replay,bool blank){
 const app_desc_t page={.on_gesture=capture,.gesture_contact_allowed=settings_gesture_contact_allowed};
 app_input_touch_filter_t filter={0};app_touch_t latest={0};app_ctx_t ctx={.touch=&latest};ui_gesture_t gesture={0};
 bool was_down=false,pressed,released;int64_t last_input=0;
 const int x[]={blank?96:100,blank?15:100,blank?96:100,0,100,0};
 const int y[]={blank?400:550,blank?400:549,blank?400:550,0,550,0};
 epd_set_output_offset(0,blank?-16:0);
 memset(events,0,sizeof(events));int before=saves;
 for(int i=0;i<6;++i){
  bool down=i!=3&&i!=5;
  app_touch_t t={.x=x[i],.y=y[i],.touched=down,.count=down};
  if(replay){touch_frame_t f={.x=x[i],.y=y[i],.touched=down,.count=down};app_input_frame_to_touch(&f,&t);}
  bool blocked=app_input_filter_screen_touch(&filter,&t,ESP_OK,false,&ctx,10+10*i);
  assert(blocked==(blank&&(i==1||i==2)));
  if(blocked){app_input_cancel_gesture(&ctx,&page,&gesture);app_input_drop_contact(&latest,&was_down);}
  app_input_ingest_touch_frame(&ctx,&latest,&t,ESP_OK,&was_down,10+10*i,&last_input,&pressed,&released);
  if(blocked||(blank&&i==3))assert(!pressed&&!released&&!latest.touched);
  app_input_dispatch_gesture_frame(&ctx,&page,&gesture,ESP_OK,false,present_event);
  was_down=t.touched;
  if(i==3){assert(events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_CANCEL]==1);
   assert(!events[UI_GESTURE_TAP]&&!events[UI_GESTURE_SWIPE_R]&&!events[UI_GESTURE_SWIPE_L]&&saves==before);}
 }
 assert(events[UI_GESTURE_PRESS]==2&&events[UI_GESTURE_TAP]==1&&saves==before+1);
 epd_set_output_offset(0,0);px=py=0;
}
static void retry_test(void){
 app_touch_t touch={0};app_ctx_t ctx={.touch=&touch};app_input_touch_filter_t filter={0};
 epd_set_output_offset(0,-32);px=1;py=-1;fail_scan=true;scans=0;int before=saves;
 now_ms=100;s_screen_offset_retries=SCREEN_OFFSET_RETRIES;
 screen_offset_present(&ctx);
 assert(scans==1&&!ctx.screen_touch_cutoff_ms&&strstr(painted_notice,"尚未生效"));
 ctx.now_ms=now_ms=s_screen_offset_retry_ms-1;assert(!screen_offset_retry_if_due(&ctx));
 ctx.now_ms=now_ms=s_screen_offset_retry_ms;fail_scan=false;
 touch=(app_touch_t){.x=15,.y=500,.touched=true,.count=1};
 assert(app_input_filter_screen_touch(&filter,&touch,ESP_OK,false,&ctx,now_ms));
 assert(!touch.touched&&ctx.physical_touch_active&&!screen_offset_retry_if_due(&ctx));
 assert(app_input_filter_screen_touch(&filter,&touch,ESP_ERR_INVALID_STATE,false,&ctx,++now_ms));
 ctx.now_ms=now_ms;assert(ctx.physical_touch_active&&!screen_offset_retry_if_due(&ctx)&&scans==1);
 assert(!app_input_filter_screen_touch(&filter,&touch,ESP_OK,false,&ctx,++now_ms));
 ctx.now_ms=now_ms;
 s_page=SETTINGS_MAIN;assert(!screen_offset_retry_if_due(&ctx));s_page=SETTINGS_SCREEN_OFFSET;
 assert(screen_offset_retry_if_due(&ctx)&&scans==2&&saves==before&&!s_screen_offset_retries);
 int x,y;display_get_screen_offset(&x,&y);assert(x==1&&y==-1);
 assert(ctx.screen_touch_cutoff_ms==now_ms&&!strstr(painted_notice,"尚未生效"));
 const int64_t cutoff=now_ms;
 fail_scan=true;px=2;s_screen_offset_retries=SCREEN_OFFSET_RETRIES;screen_offset_present(&ctx);
 for(unsigned i=0;i<SCREEN_OFFSET_RETRIES;++i){ctx.now_ms=now_ms+=1000;assert(screen_offset_retry_if_due(&ctx));}
 ctx.now_ms=now_ms+=1000;assert(!screen_offset_retry_if_due(&ctx)&&scans==3+SCREEN_OFFSET_RETRIES&&saves==before);
 display_get_screen_offset(&x,&y);assert(x==1&&y==-1&&ctx.screen_touch_cutoff_ms==cutoff&&strstr(painted_notice,"尚未生效"));
 epd_set_output_offset(0,0);px=py=0;fail_scan=false;
}
static ui_gesture_event_t event(int type,int index){EpdRect r=screen_offset_button(index);return (ui_gesture_event_t){.type=type,.x0=r.x+10,.y0=r.y+10,.x=r.x+10,.y=r.y+10};}
static void present_replay(app_ctx_t *ctx,const app_desc_t *page,ui_gesture_t *g,app_redraw_t redraw){
 (void)page;(void)g;if(redraw==APP_REDRAW_PAGE)screen_offset_present(ctx);
}
static void replay_barrier_test(void){
 const app_desc_t page={.on_gesture=capture,.gesture_contact_allowed=settings_gesture_contact_allowed};
 const EpdRect center=screen_offset_button(4);
 app_input_touch_filter_t filter={0};app_touch_t latest={0};app_ctx_t ctx={.touch=&latest};ui_gesture_t gesture={0};
 bool was_down=false,pressed,released;int64_t last_input=0;
 epd_set_output_offset(0,-32);px=32;py=0;now_ms=1000;scans=0;int before=saves;
 // 居中按键在回放中成功落屏；旧画面 x=70 不是左移键，但新画面是。
 // Center commits mid-replay; x=70 is outside the old Left button but inside the new one.
 const touch_frame_t frames[]={
  {.t_ms=800,.x=center.x+42,.y=center.y+10,.touched=1,.count=1}, {.t_ms=820},
  {.t_ms=850,.x=70,.y=560,.touched=1,.count=1}, {.t_ms=870},
  {.t_ms=1000,.x=70,.y=560,.touched=1,.count=1},
  {.t_ms=1000,.x=70,.y=560,.touched=1,.count=1},
  {.t_ms=1010,.x=70,.y=560,.touched=1,.count=1}, {.t_ms=1030},
  {.t_ms=1050,.x=70,.y=560,.touched=1,.count=1}, {.t_ms=1080},
  {.t_ms=1090,.x=10,.y=1500,.touched=1,.count=1}, {.t_ms=1095},
 };
 for(unsigned i=0;i<sizeof(frames)/sizeof(frames[0]);++i){
  const touch_frame_t *f=&frames[i];app_touch_t t;app_input_frame_to_touch(f,&t);
  if(i==9)now_ms=1100;
  bool blocked=app_input_filter_screen_touch(&filter,&t,ESP_OK,false,&ctx,f->t_ms);
  assert(blocked==(i>=2&&i<=6));
  assert(ctx.physical_touch_active==(bool)f->touched);
  if(blocked){app_input_cancel_gesture(&ctx,&page,&gesture);app_input_drop_contact(&latest,&was_down);}
  app_input_ingest_touch_frame(&ctx,&latest,&t,ESP_OK,&was_down,f->t_ms,&last_input,&pressed,&released);
  if(i<10)app_input_dispatch_gesture_frame(&ctx,&page,&gesture,ESP_OK,false,present_replay);
  else if(i==10)assert(pressed&&t.touched&&ui_key_hit_test(t.x,t.y)==0);
  else assert(released);
  was_down=t.touched;
  assert(saves==before+(i>=1)+(i>=9));
  if(i>=1&&i<9)assert(ctx.screen_touch_cutoff_ms==1000&&px==0&&py==0&&scans==1);
 }
 assert(px==-1&&py==0&&scans==2&&ctx.screen_touch_cutoff_ms==1100);
 now_ms=1110;screen_offset_present(&ctx);assert(ctx.screen_touch_cutoff_ms==1100);
 epd_set_output_offset(0,0);px=py=0;
}
static void tap(int index){app_ctx_t c={0};ui_gesture_event_t e=event(UI_GESTURE_PRESS,index);int x=px,y=py,f=feedback,inv=inverted;
 assert(screen_offset_gesture(&c,&e)==APP_REDRAW_DONE&&s_screen_offset_pressed==index);
 assert(px==x&&py==y&&feedback==f+1&&inverted==inv+1);
 e.type=UI_GESTURE_TAP;assert(screen_offset_gesture(&c,&e)==APP_REDRAW_PAGE&&s_screen_offset_pressed==-1);}
int main(void){
 screen_offset_render(NULL,false);guides=true;screen_offset_guides(NULL);guides=false;assert(blocks==128&&ruler_labels==128&&buttons==5);
 tap(0);assert(px==0&&py==-1);tap(1);assert(px==-1&&py==-1);
 tap(2);assert(px==0&&py==-1);tap(3);assert(px==0&&py==0);
 px=32;tap(2);assert(px==32&&s_notice[0]);
 py=-32;tap(0);assert(py==-32);
 fail=true;tap(4);assert(px==32&&py==-32);fail=false;
 tap(4);assert(px==0&&py==0&&!s_notice[0]);
 app_ctx_t c={0};
 for(int cancel=UI_GESTURE_MOVE;cancel<=UI_GESTURE_CANCEL;++cancel){
  if(cancel==UI_GESTURE_TAP)continue;
  ui_gesture_event_t e=event(UI_GESTURE_PRESS,2);screen_offset_gesture(&c,&e);e.type=cancel;
  app_redraw_t result=screen_offset_gesture(&c,&e);
  if(cancel==UI_GESTURE_LONG_PRESS){assert(!result&&s_screen_offset_pressed==2);e.type=UI_GESTURE_CANCEL;result=screen_offset_gesture(&c,&e);}
  assert(result==APP_REDRAW_DONE&&s_screen_offset_pressed==-1&&px==0&&py==0);
 }
 ui_gesture_event_t e=event(UI_GESTURE_PRESS,1);screen_offset_gesture(&c,&e);e.type=UI_GESTURE_TAP;e.x=0;
 assert(screen_offset_gesture(&c,&e)==APP_REDRAW_DONE&&px==0);
 e=event(UI_GESTURE_TAP,2);assert(!screen_offset_gesture(&c,&e)&&px==0);
 e=(ui_gesture_event_t){.type=UI_GESTURE_TAP,.x0=200,.y0=1150,.x=200,.y=1150};assert(!screen_offset_gesture(&c,&e));
 e.x0=e.x=60;e.y0=e.y=110;assert(screen_offset_gesture(&c,&e)==APP_REDRAW_PAGE&&s_page==SETTINGS_MAIN&&prepared==1);
 s_page=SETTINGS_SCREEN_OFFSET;
 for(int replay=0;replay<2;++replay){contact_test(replay,false);contact_test(replay,true);}
 replay_barrier_test();retry_test();
 puts("PASS: offset commit mid-replay rejects stale taps and reused/held frames, preserves raw keys, and retries only after physical lift");
 puts("PASS: live/replay exits cancel without NVS writes, bounded idle retry and applied-state rendering");
 puts("PASS: edge-anchored 1-32px solid staircase rulers, press/release inversion, cancellation, four 1px steps, limits, failed save, reset and navigation");
}
'''

with tempfile.TemporaryDirectory() as folder:
    temp = Path(folder)
    for name, source, args in (
        ("scan", scan, ["-Itools/display_host_stubs", "-Imain/display", "-Icomponents/epdiy/src",
                        "components/epdiy/src/epd_output_offset.c", "main/display/display_orientation.c"]),
        ("ui", ui, ["-Itools/app_loop_stubs", "-Imain", "-Imain/app", "-Imain/ui", "-Imain/display",
                    "-Itools/ui_gesture_stubs", "-Icomponents/epdiy/src",
                    "main/app/app_input.c", "main/ui/ui_gesture.c",
                    "components/epdiy/src/epd_output_offset.c"]),
    ):
        path, binary = temp / (name + ".c"), temp / name
        path.write_text(source)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-pointer-to-int-cast",
                        "-fsanitize=address,undefined", "-g", str(path), *args, "-o", str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary)], check=True)
