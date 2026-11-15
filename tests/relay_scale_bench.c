/* Relay Nine's portrait scene draw, isolated: LVGL's image transform of the
 * 320x360 frame against the app's scaler (relay_scale.h) followed by an
 * unscaled LVGL draw. A 568x1232 RGB565 display in DIRECT mode, as on the
 * K230, with a no-op flush. Prints CPU ms percentiles per path, the scaled
 * buffer's size, and how far each app-scaled image is from LVGL's at the
 * same placement (mean absolute difference in 8-bit units, and PSNR).
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "relay.h"
#include "relay_scale.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define DW 568
#define DH 1232
#define RUNS 200
static uint16_t screen[DW*DH];
static struct rn_frame frame;
static double now(void){struct timespec t;clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t);return t.tv_sec*1000.0+t.tv_nsec/1000000.0;}
static int order(const void *a,const void *b){double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y);}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){(void)a;(void)p;lv_display_flush_ready(d);}
static void report(const char *name,double *t)
{
    qsort(t,RUNS,sizeof(*t),order);
    printf("%-34s p50=%7.3f p95=%7.3f p99=%7.3f ms\n",name,t[RUNS/2],t[RUNS*95/100],t[RUNS*99/100]);
}
static void rgb(uint16_t c,int *p){p[0]=(c>>11)*255/31;p[1]=((c>>5)&63)*255/63;p[2]=(c&31)*255/31;}
/* Inner area only: LVGL fades the outermost pixel of a transformed image. */
static void compare(const char *name,const uint16_t *a,const uint16_t *b,int x0,int y0,int w,int h)
{
    double abs_sum=0,sq=0;long n=0;
    for(int y=y0+2;y<y0+h-2;y++)for(int x=x0+2;x<x0+w-2;x++){
        int p[3],q[3];rgb(a[y*DW+x],p);rgb(b[y*DW+x],q);
        for(int k=0;k<3;k++){double d=p[k]-q[k];abs_sum+=fabs(d);sq+=d*d;n++;}
    }
    double mse=sq/n;
    printf("%-34s mean |d|=%.2f/255  PSNR=%.1f dB\n",name,abs_sum/n,mse>0?10*log10(255.0*255.0/mse):99.0);
}
int main(int argc,char **argv)
{
    struct rn_game g;if(!rn_restart(&g,false))return 1;
    /* A wall-, floor- and sprite-heavy view a few seconds into the route. */
    for(int i=0;i<argc-1&&i<1;i++)g.angle+=atof(argv[1]);
    if(!rn_render(&g,&frame))return 1;
    lv_init();lv_display_t *d=lv_display_create(DW,DH);
    lv_display_set_buffers(d,screen,NULL,sizeof(screen),LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d,flush);
    lv_obj_set_style_bg_color(lv_screen_active(),lv_color_black(),0);
    unsigned scale=DW*256/RN_W;int w=RN_W*(int)scale/256,h=RN_H*(int)scale/256,x=(DW-w)/2,y=178;
    printf("portrait view %dx%d at scale %u/256; scaled buffer %zu bytes\n",w,h,scale,rn_scaler_bytes(RN_W,w,h));
    lv_image_dsc_t native={0};native.header.magic=LV_IMAGE_HEADER_MAGIC;native.header.cf=LV_COLOR_FORMAT_RGB565;
    native.header.w=RN_W;native.header.h=RN_H;native.header.stride=RN_W*2;native.data_size=sizeof(frame.pixels);native.data=(uint8_t *)frame.pixels;
    struct rn_scaler s={0};if(!rn_scaler_resize(&s,RN_W,RN_H,w,h))return 1;
    lv_image_dsc_t scaled=native;scaled.header.w=w;scaled.header.h=h;scaled.header.stride=w*2;scaled.data_size=w*h*2;scaled.data=(uint8_t *)s.pixels;
    lv_obj_t *image=lv_image_create(lv_screen_active());
    static uint16_t shot[4][DW*DH];double t[RUNS],u[RUNS];
    for(int mode=0;mode<4;mode++){
        bool app=mode>=2,smooth=!(mode&1);
        lv_image_set_src(image,app?&scaled:&native);lv_image_set_antialias(image,smooth);
        lv_image_set_scale(image,app?LV_SCALE_NONE:scale);
        lv_obj_set_pos(image,app?x:x-(RN_W-w)/2,app?y:y-(RN_H-h)/2);
        lv_obj_update_layout(lv_screen_active());
        if(app)rn_scale(&s,frame.pixels,smooth);
        lv_obj_invalidate(lv_screen_active());lv_refr_now(d);
        for(int i=0;i<RUNS+20;i++){
            double begin=now();
            if(app){rn_scale(&s,frame.pixels,smooth);lv_image_cache_drop(&scaled);}
            double mid=now();
            lv_obj_invalidate(image);lv_refr_now(d);
            if(i>=20){t[i-20]=now()-begin;u[i-20]=mid-begin;}
        }
        memcpy(shot[mode],screen,sizeof(screen));
        const char *names[]={"LVGL transform, antialiased","LVGL transform, nearest","app filtered + LVGL copy","app nearest + LVGL copy"};
        report(names[mode],t);
        if(app){char n[64];snprintf(n,sizeof(n),"  of which app %s scale",smooth?"filtered":"nearest");report(n,u);}
    }
    compare("app filtered vs LVGL antialiased",shot[2],shot[0],x,y,w,h);
    compare("app nearest vs LVGL nearest",shot[3],shot[1],x,y,w,h);
    compare("app nearest vs LVGL antialiased",shot[3],shot[0],x,y,w,h);
    compare("LVGL nearest vs LVGL antialiased",shot[1],shot[0],x,y,w,h);
    const char *dump=getenv("RELAY_SCALE_DUMP");
    if(dump){FILE *f=fopen(dump,"wb");if(f){for(int m=0;m<4;m++)for(int r=y;r<y+h;r++)fwrite(shot[m]+r*DW+x,2,(size_t)w,f);fclose(f);}}
    lv_obj_delete(image);rn_scaler_free(&s);lv_display_delete(d);lv_deinit();return 0;
}
