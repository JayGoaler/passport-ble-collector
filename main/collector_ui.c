#include "collector_platform.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static QueueHandle_t views,keys;
static lv_obj_t *screen,*title,*battery,*status,*lines[7],*footer;
static lv_font_t font;
LV_FONT_DECLARE(collector_font_16);
static const char *menus[]={"暂停 / 恢复","USB 配置","重新连接","清除配置","返回状态"};
static void input(bsp_btn_t b,bsp_btn_ev_t e,void *unused)
{
    (void)unused; col_key k={b,e};
    if (e==BSP_BTN_CLICK || e==BSP_BTN_LONG) xQueueSend(keys,&k,0);
}
static lv_obj_t *label(int y,int height)
{
    lv_obj_t *l=lv_label_create(screen); lv_obj_set_pos(l,18,y); lv_obj_set_size(l,204,height);
    lv_obj_set_style_text_font(l,&font,0); lv_obj_set_style_text_color(l,lv_color_hex(0xF8FAFC),0);
    lv_label_set_long_mode(l,LV_LABEL_LONG_DOT); return l;
}
/* 动态广播名可能超出内置字库；明确替换，不修改上传内容。 */
static void display_name(char *dst,size_t cap,const char *src)
{
    if (!cap) return;
    size_t used=0;
    while (*src) {
        const unsigned char *s=(const unsigned char *)src;
        unsigned w=s[0]<128?1:s[0]>=0xC2 && s[0]<=0xDF?2:s[0]>=0xE0 && s[0]<=0xEF?3:
            s[0]>=0xF0 && s[0]<=0xF4?4:0;
        bool valid=w!=0;
        uint32_t cp=s[0] & (w==1?127:w==2?31:w==3?15:7);
        for (unsigned i=1;i<w;i++) {
            if (!s[i] || (s[i]&0xC0)!=0x80) { valid=false; break; }
            cp=(cp<<6)|(s[i]&63);
        }
        if ((w==2 && cp<0x80) || (w==3 && cp<0x800) || (w==4 && cp<0x10000) ||
            (cp>=0xD800 && cp<=0xDFFF) || cp>0x10FFFF) valid=false;
        lv_font_glyph_dsc_t d={0};
        bool supported=valid && cp>=32 && lv_font_get_glyph_dsc(&font,&d,cp,0) && !d.is_placeholder;
        size_t take=supported?w:3;
        if (used+take>=cap) break;
        memcpy(dst+used,supported?src:"[?]",take); used+=take;
        src+=valid?w:1;
    }
    dst[used]=0;
}
static void render(const col_view *v)
{
    static const char *titles[]={"蓝牙采集","设备列表","运行诊断","操作菜单"};
    lv_label_set_text(title,titles[v->page]);
    if (v->battery>=0) lv_label_set_text_fmt(battery,"%d%%",v->battery); else lv_label_set_text(battery,"--");
    lv_label_set_text(status,v->state);
    lv_obj_set_style_text_color(status,lv_color_hex(v->error[0]?0xFBBF24:0x4ADE80),0);
    for (int i=0;i<7;i++) lv_label_set_text(lines[i],"");
    if (v->page==0) {
        lv_label_set_text_fmt(lines[0],"节点 %s",v->node[0]?v->node:"--");
        lv_label_set_text_fmt(lines[1],"网络 %s",v->wifi?v->ip:"未连接");
        lv_label_set_text_fmt(lines[2],"时间 %s  版本 %lld",v->clock?"已同步":"等待",(long long)v->revision);
        lv_label_set_text_fmt(lines[3],"地址 %u  事件 %u",v->addresses,v->events);
        lv_label_set_text_fmt(lines[4],"待发 %u  丢弃 %u",v->pending,v->dropped);
        lv_label_set_text_fmt(lines[5],"上传 %s",v->last_upload[0]?v->last_upload+11:"尚未成功");
        lv_label_set_text(lines[6],v->error[0]?v->error:"RAM 配置 / 重启失效");
    } else if (v->page==1) {
        char name[129]; display_name(name,sizeof(name),v->device_name);
        lv_label_set_text_fmt(lines[0],"设备 %d / %d",v->device_total?v->device_index+1:0,v->device_total);
        lv_label_set_text(lines[1],name[0]?name:"匿名设备");
        lv_label_set_text_fmt(lines[2],"地址尾号 %s",v->device_suffix);
        if (v->device_age<0) lv_label_set_text(lines[3],"信号 -- / 无近期采样");
        else lv_label_set_text_fmt(lines[3],"信号 %d dBm",v->device_rssi);
        if (v->device_age<0) lv_label_set_text(lines[4],"采样已过期");
        else lv_label_set_text_fmt(lines[4],"距采样 %d 秒",v->device_age);
        lv_label_set_text(lines[5],"[?] 表示字库未收录");
        lv_label_set_text(lines[6],"确认键查看下一设备");
    } else if (v->page==2) {
        lv_label_set_text_fmt(lines[0],"空闲内存 %lu",(unsigned long)v->heap_free);
        lv_label_set_text_fmt(lines[1],"最低内存 %lu",(unsigned long)v->heap_min);
        lv_label_set_text_fmt(lines[2],"最大空闲块 %lu",(unsigned long)v->heap_largest);
        lv_label_set_text_fmt(lines[3],"控制栈余量 %lu",(unsigned long)v->stack_min);
        lv_label_set_text_fmt(lines[4],"原始包丢弃 %u",v->raw_dropped);
        lv_label_set_text(lines[5],v->error[0]?v->error:"无错误");
        lv_label_set_text(lines[6],COLLECTOR_VERSION);
    } else {
        for (int i=0;i<5;i++) {
            lv_label_set_text_fmt(lines[i],"%s %s",v->menu==i?">":" ",menus[i]);
            lv_obj_set_style_text_color(lines[i],lv_color_hex(v->menu==i?0x4ADE80:0xF8FAFC),0);
        }
        lv_label_set_text(lines[5],v->provisioning?"配置窗口 120 秒":v->clearing?"再按确认清除本机配置":"长按确认返回");
        lv_label_set_text(lines[6],v->local_pause?"本地已暂停":"本地允许扫描");
    }
    if (v->page!=3) for (int i=0;i<7;i++) lv_obj_set_style_text_color(lines[i],lv_color_hex(0xF8FAFC),0);
    lv_label_set_text(footer,v->page==3?"上/下选择  确认执行":"上/下页 长按确认菜单");
#ifdef CONFIG_COLLECTOR_WOKWI
    lv_label_set_text(footer,"SIM / SYNTHETIC BLE");
#endif
}
static void worker(void *unused)
{
    (void)unused; col_view v;
    for (;;) if (xQueueReceive(views,&v,portMAX_DELAY)==pdTRUE && bsp_lvgl_lock(200)) {
        render(&v); bsp_lvgl_unlock();
    }
}
esp_err_t col_ui_init(QueueHandle_t v,QueueHandle_t k)
{
    views=v; keys=k;
    esp_err_t err=bsp_display_init(); if (err!=ESP_OK) return err;
    if (!bsp_lvgl_init() || !bsp_lvgl_lock(1000)) return ESP_FAIL;
    font=collector_font_16; font.fallback=&lv_font_montserrat_14;
    screen=lv_obj_create(NULL); lv_obj_set_style_bg_color(screen,lv_color_hex(0x0F172A),0);
    lv_obj_set_style_pad_all(screen,0,0); lv_obj_set_style_border_width(screen,0,0);
    lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    title=label(20,24); lv_obj_set_width(title,150);
    battery=label(22,22); lv_obj_set_pos(battery,177,22); lv_obj_set_width(battery,46);
    status=label(55,26);
    for (int i=0;i<7;i++) lines[i]=label(89+i*25,22);
    footer=label(283,20); lv_obj_set_style_text_color(footer,lv_color_hex(0x94A3B8),0);
    lv_screen_load(screen); bsp_display_backlight(70); bsp_lvgl_unlock();
    err=bsp_button_init(input,NULL); if (err!=ESP_OK) return err;
    return xTaskCreate(worker,"collector_ui",4096,NULL,3,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
