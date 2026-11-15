/* Original cached reactor materials and sculpted Relay Nine component recipes.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "relay.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static uint16_t textures[6][64*64];
static bool ready;
/* Exact integer channel shading, bounded to the authored light range. */
static uint8_t shade5[201][32],shade6[201][64];
static uint16_t color(int r,int g,int b)
{r=r<0?0:r>255?255:r;g=g<0?0:g>255?255:g;b=b<0?0:b>255?255:b;return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));}
static unsigned hash(unsigned x,unsigned y){unsigned v=x*374761393u+y*668265263u;v=(v^(v>>13))*1274126177u;return v^(v>>16);}
static bool box(int x,int y,int a,int b,int c,int d){return x>=a&&x<=c&&y>=b&&y<=d;}
/* Cached albedo/emission recipes. Edges are bevels, not neon rectangles:
 * layered ceramic ribs, recessed channels and split reactor shutters. */
static uint16_t emission[6][64*64];
static void init(void)
{
    for(int m=0;m<6;m++)for(int y=0;y<64;y++)for(int x=0;x<64;x++){
        int n=(int)(hash(x+m*71,y)%5)-2,r=17+n,g=27+n,b=42+n;
        int er=0,eg=0,eb=0;
        if(m==0){
            int rib=abs(x-32),bevel=abs(y-32);
            r=24+n;g=37+n;b=55+n;
            if(rib>25){r=9;g=15;b=26;}
            if(rib==25||bevel==29){r=84;g=111;b=133;}
            if(rib==24||bevel==28){r=39;g=60;b=79;}
            if(x>8&&x<19){r=36+n;g=54+n;b=74+n;if(y%8<2){r=10;g=18;b=30;}}
            if(x>39&&x<52&&y>12&&y<51){r=6;g=14;b=25;}
            int d=abs(x-45);
            if(y>15&&y<48&&d<5){eg=(5-d)*27;eb=(5-d)*35;er=(5-d)*6;}
            if(y>17&&y<46&&d==0){er=149;eg=248;eb=255;}
            if((x==5||x==58)&&(y==6||y==57)){r=133;g=155;b=173;}
            if(y>53&&y<56&&x>12&&x<33){r=154;g=110;b=48;}
        }else if(m==1){
            /* Faceted superconducting housings, indigo recess and cyan spine. */
            int dx=abs(x-32),dy=abs(y-32);
            r=26+n;g=32+n;b=56+n;
            if(dx+dy>49){r=10;g=17;b=31;}
            if(dx+dy==48||x==3){r=88;g=102;b=133;}
            if(x>10&&x<51&&y>19&&y<45){r=10;g=17;b=34;}
            if(y>=26&&y<=37&&x>13&&x<49){int d=abs(y-32);er=15;eg=105-d*14;eb=160-d*19;}
            if(y==32&&x>13&&x<49){er=144;eg=237;eb=255;}
            if(x>52&&y%8<3){r=95;g=81;b=133;}
            if(y<2||x<2){r=4;g=8;b=17;}
        }else if(m==2){
            /* Split blast iris: diagonal titanium leaves and amber lock. */
            int d=abs(x-32),rib=(y+d/2)%16;
            r=32+n;g=44+n;b=65+n;
            if(rib<2){r=7;g=12;b=24;}
            if(rib==2){r=105;g=132;b=153;}
            if(d<3){r=6;g=9;b=18;er=112;eg=57;eb=12;}
            if(d>27){r=8;g=20;b=31;er=14;eg=120;eb=155;}
            if(d<8&&y>22&&y<41){r=10;g=16;b=25;er=200;eg=98;eb=22;}
            if(d<3&&y>27&&y<36){er=255;eg=210;eb=116;}
        }else if(m==3){
            /* Broad basalt floor plates with a recessed power rail. */
            r=13+n;g=23+n;b=34+n;
            if(x<2||y<2){r=3;g=7;b=14;}
            if(y==2){r=39;g=58;b=70;}
            if(x>5&&x<58&&y>7&&y<57&&(x+y)%16==0){r=24;g=37;b=49;}
            int d=abs(x-32);
            if(d<4){r=3;g=14;b=23;er=0;eg=48-d*10;eb=67-d*13;}
            if(d==0){er=23;eg=151;eb=182;}
            if(y>53&&x>44&&x<57&&(x+y)%6<2){r=111;g=86;b=43;}
        }else if(m==4){
            r=36+n;g=49+n;b=67+n;
            if(y<4||y>59){r=7;g=15;b=27;er=8;eg=92;eb=124;}
            if(y==4){r=121;g=148;b=165;}
            if(x%16<3){r=12;g=22;b=36;}
            if(y>24&&y<39){r=16;g=29;b=45;if((x+y/2)%16<5){er=160;eg=88;eb=28;}}
        }else{
            r=7+n;g=13+n;b=24+n;
            if(x%32<3||y%32<3){r=27;g=40;b=59;}
            if(x>22&&x<28){r=40;g=53;b=68;}
            int d=abs(y-12);
            if(x>6&&x<18&&d<5){er=33-d*6;eg=126-d*22;eb=166-d*28;}
            if(x>7&&x<17&&d<2){er=170;eg=236;eb=255;}
        }
        textures[m][y*64+x]=color(r,g,b);
        emission[m][y*64+x]=color(er,eg,eb);
    }
    for(int l=55;l<=255;l++){
        for(int v=0;v<32;v++)shade5[l-55][v]=(uint8_t)((v*8*l/255)>>3);
        for(int v=0;v<64;v++)shade6[l-55][v]=(uint8_t)((v*4*l/255)>>2);
    }
    ready=true;
}
uint16_t rn_texture(int m,int x,int y,int light)
{
    if(!ready)init();
    if(m<0||m>5)m=1;
    int p=((y&63)*64)+(x&63);
    uint16_t c=textures[m][p],e=emission[m][p];
    light=light<55?0:light>255?200:light-55;
    int r=shade5[light][c>>11]+(e>>11);
    int g=shade6[light][(c>>5)&63]+((e>>5)&63);
    int b=shade5[light][c&31]+(e&31);
    return (uint16_t)(((r>31?31:r)<<11)|((g>63?63:g)<<5)|(b>31?31:b));
}
/* Bounded emissive lookup for the animated current; no texture rebuild. */
uint16_t rn_emission(int m,int x,int y)
{
    if(!ready)init();
    return m>=0&&m<6?emission[m][(y&63)*64+(x&63)]:0;
}
/* Original silhouettes: suspended sentinel, segmented crawler, levitating
 * supply cells and relay coils. Red/amber telegraphs remain mechanically timed. */
uint16_t rn_sprite(int kind,int x,int y,int phase,int facing,int state)
{
    if(x<0||y<0||x>=64||y>=64)return 0;
    if(kind<2&&state==3)y=63+(y-63)*3;
    int dx=abs(x-32),dy=abs(y-28);
    bool alert=state==1,hit=state==4;
    uint16_t hot=hit?color(255,248,206):alert?color(255,187,57):color(255,64,99);
    if(kind==0){
        bool back=facing>=3&&facing<=5,side=facing==2||facing==6;
        int shift=side?3:0;
        /* Blade shoulders, tapered carapace and an exposed reactor sphere. */
        if(y>12&&y<35&&dx>13&&dx<27-(y-13)/3){
            if(y<17||dx==25-(y-13)/3)return color(148,182,207);
            if(y==24)return hot;
            return color(36+dx,45+dx,68+dx);
        }
        if(y>9&&y<43&&dx<15-abs(y-25)/4){
            if(dx>10-abs(y-25)/5)return color(89,116,143);
            if(dx+abs(y-27)<9)return back?color(32,117,167):hot;
            if(dx+abs(y-27)<12)return color(19,40,66);
            if(y==12||y==38)return color(170,198,214);
            return color(24+dx*2,35+dx*2,57+dx*2);
        }
        if(y>2&&y<13&&dx<9-abs(y-8)/2){
            if(y==8&&!back)return hot;
            return color(y<6?128:28,y<6?160:39,y<6?186:62);
        }
        static const int gait[4]={-3,0,3,0};int a=state?0:gait[phase&3];
        if((box(x,y,22+shift,40+a,28+shift,56+a))||box(x,y,36+shift,40-a,42+shift,56-a))
            return y%9<3?color(122,157,181):color(25,44,67);
        if(box(x,y,18+shift,54+a,29+shift,60+a)||box(x,y,35+shift,54-a,46+shift,60-a))return color(52,80,107);
        if(box(x,y,44,29,57,36))return y==29?color(166,199,220):color(20,36,58);
        if(state==2&&abs(x-58)+abs(y-32)<10)return color(255,218,138);
    }else if(kind==1){
        for(int j=0;j<3;j++){
            int yy=35+j*10+((phase+j)&1?2:-2),tip=3+j*2;
            if(y>=yy&&y<yy+3&&dx>12&&dx<30-j*2)return color(99,138,169);
            if((x==tip||x==63-tip)&&y>yy&&y<yy+7)return color(32,199,221);
        }
        if(dx*dx*2+dy*dy<340&&y>15){
            if(dx*dx*2+dy*dy>245)return color(104,135,168);
            if(abs(y-26)<3&&dx<14)return hot;
            if(y>32&&x%8<3)return color(30,191,217);
            return color(22+dx,32+dx,60+dx);
        }
    }else if(kind==2||kind==3||kind==7||kind==8){
        if(y>52&&y<57&&dx<23)return color(9,53,74);
        if(y>48&&y<52&&dx<17)return color(28,117,145);
        int d=abs(y-28);
        if(dx+d/2<21&&y>8&&y<46){
            uint16_t energy=kind==3?color(77,250,171):kind==7?color(186,135,255):kind==8?color(255,166,59):color(47,218,255);
            if(dx+d/2>17)return color(115,153,179);
            if(y<13||y>41)return color(24,43,68);
            if(kind==3&&(dx<3||abs(y-27)<3))return energy;
            if(kind==7&&(box(x,y,19,22,45,26)||box(x,y,32,26,37,35)))return energy;
            if(kind!=3&&kind!=7&&x%9<4&&y>18&&y<36)return energy;
            return color(12,26,48);
        }
    }else if(kind==4||kind==5){
        int d=dx+abs(y-31)/2;
        if(d<26&&y>3&&y<61){
            if(d>22||y<7||y>57)return color(101,138,171);
            if(dx<3||abs(y-31)<2)return color(122,246,238);
            if(dx<14&&y>14&&y<49){if((y+phase*4)%9<3)return color(35,177,192);return color(19,69,111);}
            return color(9,22,43);
        }
    }else if(kind==9&&box(x,y,3,10,60,58)){
        if(x<6||x>57||y<13||y>55)return color(62,100,137);
        if(phase==2){if(y>27&&y<38&&(abs(x-17)<4||abs(x-32)<4||abs(x-47)<4))return color(255,181,70);}
        else if(phase==1){if((x>14&&x<50&&y>29&&y<36)||(x<29&&abs(y-32)<29-x))return color(79,223,245);}
        else{
            if(box(x,y,12,22,27,45)&&(x<16||x>23||y<26||y>41))return color(120,230,255);
            if(box(x,y,35,22,50,45)&&(x>46||y<26||(y>31&&y<36)||(x<39&&y<34)))return color(120,230,255);
        }
        return color(6,17,34);
    }
    return 0;
}
uint16_t rn_weapon(int x,int y,int flash,int reload,int weapon,bool reduced_motion)
{
    if(x<0||y<0||x>=128||y>=96)return 0;
    int duration=weapon?1500:900;
    bool open=reload&&(reduced_motion||(reload>duration/4&&reload<duration*4/5));
    if(reload){int dip=reduced_motion?12:(int)(sin((double)reload/duration*RN_PI)*20);y-=dip;x-=dip/3;}
    int dx=abs(x-60);
    /* Short, luminous plasma crown retains the original shot duration. */
    if(flash){
        int d=dx+abs(y-13);
        if(d<6)return color(236,255,255);
        if(d<(weapon?24:17)&&(dx<4||abs(y-13)<3||abs(dx-abs(y-13))<2))return color(87,220,255);
        if(d<12)return color(49,119,201);
    }
    if(y>17&&y<55&&dx<10+(y-17)/5){
        if(y<25){
            if(weapon&&((x>51&&x<57)||(x>63&&x<69)))return color(7,16,29);
            if(!weapon&&dx<5&&y>19)return color(12,22,37);
            return color(110,154,185);
        }
        if(dx<2)return color(136,234,255);
        if(dx==7||dx==12)return color(77,128,170);
        if(weapon&&y%9<3)return color(12,29,49);
        return color(25+dx*2,43+dx*2,66+dx*2);
    }
    if(y>=49&&x>30+(y-49)/3&&x<87+(y-49)/2){
        if(y<53)return color(170,202,220);
        if(x<38+(y-49)/3)return color(75,107,144);
        if(box(x,y,56,57,84,73)){
            if(x==56||y==57)return color(85,144,176);
            if(open)return color(x%9<5?102:12,x%9<5?209:23,x%9<5?255:41);
            if(y>61&&y<67&&x>60&&x<80)return weapon?color(190,147,255):color(72,221,254);
            return color(10,24,43);
        }
        if(y>79&&x%9<2)return color(55,118,159);
        return color(18+(x-40)/4,31+(x-40)/4,52+(x-40)/3);
    }
    /* Segmented pressure glove and cool rim-lit sleeve. */
    if(y>70&&x>16+(y-70)/2&&x<53+(y-70)/2){
        if(y>88)return color(19,29,49);
        if(y%7<2)return color(13,23,41);
        if(x<23+(y-70)/2)return color(100,145,175);
        return color(38,59,82);
    }
    return 0;
}
void rn_title(struct rn_frame *f)
{
    memset(f,0,sizeof(*f));
    int cy=RN_H/2-18;
    for(int y=0;y<RN_H;y++)for(int x=0;x<RN_W;x++){
        int dx=x-RN_W/2,dy=y-cy,d=(int)sqrt(dx*dx+dy*dy);
        int halo=d>67&&d<117?(117-d)/3:0;
        int r=3+halo/4,g=7+halo,b=17+halo*2;
        if(d>72&&d<91){int ring=(d-72)%7;r=ring<2?55:8;g=ring<2?139:29;b=ring<2?188:51;}
        if(d>93&&d<96&&((x+y)/16)%3){r=70;g=205;b=239;}
        if(d<62){r=5;g=15;b=31;if(abs(dx)<3||abs(dy)<2){r=42;g=150;b=188;}}
        if(d<55&&abs(dx)<36&&abs(dy)<36){
            for(int i=-1;i<=1;i++)for(int j=-1;j<=1;j++){
                int a=abs(dx-i*23),b0=abs(dy-j*23);
                if(a+b0<8){r=112;g=232;b=255;}else if(a+b0<11){r=24;g=89;b=129;}
            }
        }
        if(y>RN_H-54&&((x-160)*(x-160)/(1+(RN_H-y)*2))%37<2){g+=10;b+=15;}
        f->pixels[y*RN_W+x]=color(r,g,b);
    }
    for(int i=0;i<9;i++)for(int y=RN_H-44;y<RN_H-40;y++)for(int x=80+i*18;x<91+i*18;x++)
        f->pixels[y*RN_W+x]=color(35,130+i*9,180+i*7);
}
