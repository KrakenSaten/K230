/* Isolated production-style RGB565 -> LVGL scaled image draw, no SDL/DRM.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "relay.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static uint8_t buffer[1920*20*4];
static struct rn_frame frame;
static double now(void){struct timespec t;clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t);return t.tv_sec*1000.0+t.tv_nsec/1000000.0;}
static int order(const void *a,const void *b){double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y);}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){(void)a;(void)p;lv_display_flush_ready(d);}
int main(void)
{
    struct rn_game g;if(!rn_restart(&g,true)||!rn_render(&g,&frame))return 1;
    lv_init();lv_display_t *d=lv_display_create(1280,720);
    lv_display_set_buffers(d,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    lv_image_dsc_t src={0};src.header.magic=LV_IMAGE_HEADER_MAGIC;src.header.cf=LV_COLOR_FORMAT_RGB565;
    src.header.w=RN_W;src.header.h=RN_H;src.header.stride=RN_W*2;src.data_size=sizeof(frame.pixels);src.data=(uint8_t *)frame.pixels;
    lv_obj_t *image=lv_image_create(lv_screen_active());lv_image_set_src(image,&src);lv_image_set_antialias(image,false);
    const int scales[]={256,400,640};
    for(unsigned mode=0;mode<sizeof(scales)/sizeof(scales[0]);mode++){
        double times[200];lv_image_set_scale(image,scales[mode]);lv_obj_center(image);lv_obj_update_layout(lv_screen_active());lv_refr_now(d);
        for(int i=0;i<220;i++){lv_obj_invalidate(image);double begin=now();lv_refr_now(d);if(i>=20)times[i-20]=now()-begin;}
        qsort(times,200,sizeof(*times),order);
        printf("LVGL scale=%d/256 p50=%.6f p95=%.6f p99=%.6f ms (no-op flush)\n",scales[mode],times[100],times[190],times[198]);
    }
    lv_obj_delete(image);lv_display_delete(d);lv_deinit();return 0;
}
