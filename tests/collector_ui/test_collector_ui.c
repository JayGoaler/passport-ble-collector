#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp_display_rounding.h"
#include "../../main/collector_ui.c"
#include "src/misc/lv_text_private.h"
#include "collector_fixed_text.h"

static uint16_t frame[240*320], draw[240*20];
static unsigned flushes, key_count;
static col_key last_key;
static bool locked;
void collector_ui_assert_fail(void)
{
    lv_mem_monitor_t stats;
    lv_mem_monitor(&stats);
    fprintf(stderr,"LVGL ASSERT: configured=%u total=%zu used=%zu peak=%zu free=%zu largest=%zu frag=%u%%\n",
        (unsigned)LV_MEM_SIZE,stats.total_size,stats.total_size-stats.free_size,
        stats.max_used,stats.free_size,stats.free_biggest_size,(unsigned)stats.frag_pct);
    abort();
}
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels)
{
    const uint16_t *source=(const uint16_t *)pixels;
    for (int y=area->y1;y<=area->y2;y++) for (int x=area->x1;x<=area->x2;x++) {
        uint16_t pixel=*source++;
        frame[y*240+x]=bsp_display_pixel_outside_rounded_rect(x,y,240,320,30)?0:pixel;
    }
    flushes++;
    lv_display_flush_ready(display);
}
esp_err_t bsp_display_init(void) { return ESP_OK; }
bool bsp_lvgl_init(void)
{
    lv_init();
    lv_display_t *display=lv_display_create(240,320);
    assert(display);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,draw,NULL,sizeof(draw),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    return true;
}
bool bsp_lvgl_lock(uint32_t timeout) { (void)timeout; assert(!locked); locked=true; return true; }
void bsp_lvgl_unlock(void) { assert(locked); locked=false; }
void bsp_display_backlight(int value) { assert(value>=0 && value<=100); }
esp_err_t bsp_button_init(bsp_btn_cb_t callback,void *user) { assert(callback==input); assert(!user); return ESP_OK; }
int xTaskCreate(void (*entry)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{
    assert(entry==worker && !strcmp(name,"collector_ui") && stack>=4096 && !arg && priority>0 && !handle);
    return pdPASS;
}
int xQueueSend(QueueHandle_t queue,const void *item,uint32_t wait)
{
    assert(queue==keys && wait==0);
    last_key=*(const col_key *)item; key_count++; return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue,void *item,uint32_t wait)
{
    (void)queue; (void)item; (void)wait; abort();
}

static void glyphs(void)
{
    unsigned count=0;
    for (size_t i=0;i<sizeof(fixed_text)/sizeof(fixed_text[0]);i++) {
        uint32_t pos=0;
        while (fixed_text[i][pos]) {
            uint32_t cp=lv_text_encoded_next(fixed_text[i],&pos);
            if (cp<32) continue;
            lv_font_glyph_dsc_t d={0};
            if (!lv_font_get_glyph_dsc(&font,&d,cp,0) || d.is_placeholder) {
                fprintf(stderr,"缺字: U+%04X in %s\n",(unsigned)cp,fixed_text[i]); abort();
            }
            if (d.box_w && d.box_h) {
                lv_draw_buf_t *bitmap=lv_draw_buf_create(d.box_w,d.box_h,LV_COLOR_FORMAT_A8,LV_STRIDE_AUTO);
                assert(bitmap && lv_font_get_glyph_bitmap(&d,bitmap));
                lv_font_glyph_release_draw_data(&d);
                lv_draw_buf_destroy(bitmap);
            }
            count++;
        }
    }
    lv_font_glyph_dsc_t missing={0};
    assert(!lv_font_get_glyph_dsc(&font,&missing,0x9F98,0) || missing.is_placeholder);
    printf("Fixed glyph and bitmap checks: %u, known-missing negative: PASS\n",count);
}

static void bounds(lv_obj_t *object)
{
    assert(lv_obj_get_style_text_font(object,LV_PART_MAIN)==&font);
    lv_area_t r; lv_obj_get_coords(object,&r);
    assert(r.y2-r.y1+1>=font.line_height);
    for (int y=r.y1;y<=r.y2;y++) {
        assert(!bsp_display_pixel_outside_rounded_rect(r.x1,y,240,320,30));
        assert(!bsp_display_pixel_outside_rounded_rect(r.x2,y,240,320,30));
    }
}
static void fits(lv_obj_t *object,const char *expected)
{
    lv_point_t size;
    lv_text_get_size(&size,expected,&font,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    if (size.x>lv_obj_get_width(object) || size.y>lv_obj_get_height(object)) {
        fprintf(stderr,"文案溢出: %s (%ld x %ld) > (%ld x %ld)\n",expected,
                (long)size.x,(long)size.y,(long)lv_obj_get_width(object),(long)lv_obj_get_height(object)); abort();
    }
    assert(!strcmp(lv_label_get_text(object),expected));
}
static void screenshot(const char *directory,int page)
{
    if (!directory) return;
    char path[1024]; snprintf(path,sizeof(path),"%s/page-%d.ppm",directory,page);
    FILE *file=fopen(path,"wb"); assert(file); fprintf(file,"P6\n240 320\n255\n");
    for (size_t i=0;i<sizeof(frame)/sizeof(frame[0]);i++) {
        uint16_t value=frame[i];
        uint8_t pixel[3]={(uint8_t)(((value>>11)&31)*255/31),
            (uint8_t)(((value>>5)&63)*255/63),(uint8_t)((value&31)*255/31)};
        assert(fwrite(pixel,1,3,file)==3);
    }
    assert(fclose(file)==0);
}
static void memory(const char *stage)
{
    lv_mem_monitor_t stats;
    lv_mem_monitor(&stats);
    assert(lv_mem_test()==LV_RESULT_OK);
    printf("LVGL memory %s: configured=%u total=%zu used=%zu peak=%zu free=%zu largest=%zu frag=%u%%\n",
        stage,(unsigned)LV_MEM_SIZE,stats.total_size,stats.total_size-stats.free_size,
        stats.max_used,stats.free_size,stats.free_biggest_size,(unsigned)stats.frag_pct);
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    printf("Collector UI memory trial: %u KiB\n",(unsigned)COLLECTOR_UI_HEAP_KIB);
    assert(col_ui_init((void *)1,(void *)2)==ESP_OK);
    memory("initialized");
    glyphs();
    memory("glyphs");
    char name[129];
    display_name(name,sizeof(name),"设备龘😀"); assert(!strcmp(name,"设备[?][?]"));
    display_name(name,4,"设备"); assert(!strcmp(name,"设"));
    display_name(name,1,"设备"); assert(!name[0]);
    display_name(name,sizeof(name),"\xE4"); assert(!strcmp(name,"[?]"));
    display_name(name,sizeof(name),"\xED\xA0\x80"); assert(!strcmp(name,"[?][?][?]"));
    display_name(name,sizeof(name),"a\nb"); assert(!strcmp(name,"a[?]b"));
    for (int b=BSP_BTN_UP;b<=BSP_BTN_OK;b++) {
        unsigned before=key_count;
        input(b,BSP_BTN_PRESS,NULL); input(b,BSP_BTN_DOUBLE,NULL); assert(key_count==before);
        input(b,BSP_BTN_CLICK,NULL); assert(key_count==before+1 && last_key.button==b && last_key.event==BSP_BTN_CLICK);
        input(b,BSP_BTN_LONG,NULL); assert(key_count==before+2 && last_key.event==BSP_BTN_LONG);
    }
    col_view view={.battery=100,.wifi=true,.clock=true,.device_total=1,.device_rssi=-63,.device_age=3,
        .addresses=COL_ADDRS,.events=1024,.pending=COL_QUEUE,.dropped=12,.heap_free=65432,.heap_min=48512,.heap_largest=32768,
        .stack_min=2048,.revision=12};
    strcpy(view.node,"passport-lab"); strcpy(view.ip,"192.168.100.100"); strcpy(view.state,"正在扫描");
    strcpy(view.device_name,"设备示例"); strcpy(view.device_suffix,"12:34:56");
    strcpy(view.last_upload,"2026-10-23T03:04:05.000Z");
    for (int page=0;page<4;page++) {
        static const char *expected_titles[]={"蓝牙采集","设备列表","运行诊断","操作菜单"};
        view.page=page;
        for (int menu=0;menu<(page==3?5:1);menu++) {
            view.menu=menu; view.clearing=page==3; render(&view); lv_obj_update_layout(screen);
            bounds(title); bounds(battery); bounds(status); bounds(footer);
            for (int i=0;i<7;i++) bounds(lines[i]);
            fits(title,expected_titles[page]);
            assert(lv_obj_get_x(title)+lv_obj_get_width(title)<=lv_obj_get_x(battery));
            fits(battery,"100%"); fits(status,"正在扫描");
            fits(footer,page==3?"上/下选择  确认执行":"上/下页 长按确认菜单");
            if (page==3) fits(lines[5],"再按确认清除本机配置");
            for (int i=0;i<6;i++) assert(lv_obj_get_y(lines[i])+lv_obj_get_height(lines[i])<=lv_obj_get_y(lines[i+1]));
            lv_refr_now(NULL);
        }
        if (page==3) { view.menu=3; render(&view); lv_refr_now(NULL); }
        screenshot(argc>1?argv[1]:NULL,page);
        memory(expected_titles[page]);
    }
    view.page=1; view.device_age=-1; render(&view); fits(lines[4],"采样已过期");
    view.page=3; view.provisioning=true; render(&view); fits(lines[5],"配置窗口 120 秒");
    view.battery=-1; render(&view); fits(battery,"--");
    static const char *states[]={"USB 配置中","请先配置","等待网络","等待校时","等待服务配置",
        "认证失败","容量已满 / 未上传","本地暂停","远程暂停","扫描未就绪"};
    for (size_t i=0;i<sizeof(states)/sizeof(states[0]);i++) {
        strcpy(view.state,states[i]); render(&view); fits(status,states[i]);
    }
    /* 长字段反复翻页，覆盖实际标签扩容、缩容和绘制引起的峰值与碎片。 */
    memset(view.node,'n',sizeof(view.node)-1); view.node[sizeof(view.node)-1]=0;
    memset(view.device_name,'d',sizeof(view.device_name)-1); view.device_name[sizeof(view.device_name)-1]=0;
    memset(view.error,'E',sizeof(view.error)-1); view.error[sizeof(view.error)-1]=0;
    view.revision=INT64_MAX; view.events=view.dropped=view.raw_dropped=UINT32_MAX;
    for (unsigned cycle=0;cycle<64;cycle++) {
        view.page=(int)(cycle%4); view.menu=(int)(cycle%5);
        render(&view); lv_obj_update_layout(screen); lv_refr_now(NULL);
        bounds(title); bounds(battery); bounds(status); bounds(footer);
        for (int i=0;i<7;i++) bounds(lines[i]);
    }
    memory("long-fields-64-page-renders");
    assert(flushes>0);
    puts("Collector UI: PASS (真实 LVGL 字形/位图/布局/四页刷新/三键回调；Device NOT RUN)");
    return 0;
}
