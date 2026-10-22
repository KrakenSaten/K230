/* Original hand-authored pixel recipes: Relay Nine industrial identity.
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
static void init(void)
{
    for(int m=0;m<6;m++)for(int y=0;y<64;y++)for(int x=0;x<64;x++){
        int n=(int)(hash(x+m*71,y)%7)-3,r=90,g=90,b=80;
        if(m==0){ /* two-meter painted-steel panels, fine wear, small fittings */
            r=77+n;g=91+n;b=90+n;
            if(x<2||y<2){r=24;g=31;b=30;}
            if(x==2||y==2){r=116;g=120;b=105;}
            if((x==6||x==58)&&(y==6||y==58)){r=35;g=43;b=42;}
            if((x==5||x==57)&&(y==5||y==57)){r=183;g=181;b=153;}
            if(y==4&&x>9&&x<54){r=133;g=144;b=133;}
            if(box(x,y,42,5,49,58)){
                r=31+n;g=42+n;b=43+n;
                if(x==43){r=141;g=151;b=138;}
                if(x==44||x==45){r=93;g=110;b=105;}
                if(y==12||y==13||y==48||y==49){r=112;g=119;b=101;}
            }
            if(box(x,y,9,30,30,43)){
                r=22;g=31;b=32;
                if(y==30||x==9){r=115;g=124;b=112;}
                if(y%4==1&&x>11&&x<28){r=74;g=88;b=82;}
                if(y%4==2&&x>11&&x<28){r=40;g=53;b=51;}
            }
            if(box(x,y,12,16,26,18)){r=202;g=156;b=71;}
            if((hash(x/2,y)&255)==1){r+=25;g+=17;b+=4;}
        }else if(m==1){ /* broad concrete blocks and sparse water streaks */
            r=149+n;g=142+n;b=124+n;
            if(y>45&&((x+y/8)%29==0)){r-=14;g-=14;b-=11;}
            if(y<2||x<1){r=73;g=78;b=70;}
            if(y==2){r=166;g=164;b=144;}
            if((hash(x/3,0)&31)==0&&y>10){r-=18;g-=17;b-=10;}
            if((hash(x,y)&511)==0){r-=32;g-=32;b-=28;}
        }else if(m==2){ /* steel lift door, recessed ribs, slim safety bands */
            r=99+n;g=108+n;b=101+n;
            if(x<4||x>59||x==31||x==32){r=26;g=36;b=35;}
            if(y%16==0){r=49;g=58;b=55;}
            if(y%16==1){r=162;g=171;b=153;}
            if(x==5||x==58){r=157;g=164;b=145;}
            if(y%16>12&&x>6&&x<57){r-=12;g-=13;b-=11;}
            if(y>53&&y<61){bool stripe=((x+y)/8)%2;r=stripe?204:31;g=stripe?161:37;b=stripe?63:33;}
            if(box(x,y,27,21,36,36)){r=29;g=39;b=37;if(y==26&&x>29&&x<34){r=222;g=83;b=38;}}
        }else if(m==3){ /* half-meter inset floor panels, fine cross tread */
            r=82+n;g=87+n;b=79+n;
            if(x%16<2||y%16<2){r=35;g=43;b=40;}
            else if((x+y)%8==0&&x%16>4&&y%16>4){r=98;g=106;b=95;}
            if(x%16==2||y%16==2){r=105;g=113;b=101;}
        }else if(m==4){
            r=114+n;g=119+n;b=105+n;
            if(x%16==0){r=54;g=65;b=59;}
            if((x+y)%12==0&&y>5&&y<55){r=122;g=130;b=114;}
            if(y<3){r=36;g=43;b=38;}
            if(y>57){bool stripe=(x/8)%2;r=stripe?202:32;g=stripe?160:39;b=stripe?65:35;}
        }else{r=61+n;g=71+n;b=68+n;if(x%32<2||y%32<2){r=26;g=35;b=34;}if(y==6&&x>10&&x<22){r=178;g=150;b=94;}}
        textures[m][y*64+x]=color(r,g,b);
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
    uint16_t c=textures[m][((y&63)*64)+(x&63)];
    light=light<55?0:light>255?200:light-55;
    return (uint16_t)((shade5[light][c>>11]<<11)|(shade6[light][(c>>5)&63]<<5)|shade5[light][c&31]);
}
uint16_t rn_sprite(int kind,int x,int y,int phase,int facing,int state)
{
    if(x<0||y<0||x>=64||y>=64)return 0;
    int n=(int)(hash(x,y)%5)-2;
    if(kind<2&&state==3)y=63+(y-63)*3; /* compressed wreck silhouette */
    if(kind==0){
        bool back=facing==3||facing==4||facing==5,side=facing==2||facing==6;
        x=(int)((x-32)*(side?1.35:1))+32;
        static const int gait[4]={-3,0,3,0};int a=gait[phase&3];if(state) a=0;
        /* Broad shoulders / narrow articulated waist / independent feet. */
        if(box(x,y,9,14,23,29)||box(x,y,41,14,55,29)){
            if(y<17&&(x<12||x>52))return 0;
            if(x==9||x==55||y==29)return color(23,31,32);
            if(y<18)return color(226,219,188);
            if(y==24&&x>13&&x<51)return state==1?color(237,164,66):color(80,101,93);
            return color(182+n,185+n,162+n);
        }
        if(box(x,y,23,15,41,40)){
            if(x==23||x==41||y==40)return color(25,33,34);
            if(back&&box(x,y,27,20,37,34))return color(y%4?46:133, y%4?58:139, y%4?55:119);
            if(state==4&&abs(x-32)+abs(y-30)<5)return color(255,222,132);
            if(box(x,y,27,23,37,31))return color(y==23?19:49,y==23?28:63,y==23?30:60);
            if(y==19||y==35)return color(211,172,79);
            if(state==4&&y>26&&y<34&&abs(x-32)==abs(y-30))return color(255,222,132);
            return color(137+n,150+n,138+n);
        }
        if(box(x,y,24,3,40,14)){
            if(y<6||x==24||x==40)return color(206,207,182);
            if(y>=7&&y<=11&&!back){
                if(y==7||y==11)return color(15,24,25);
                return state==1?color(255,194,75):color(255,64,35);
            }
            return color(39,51,51);
        }
        if(box(x,y,13,29,20,44))return color(74,90,84);
        if(box(x,y,44,28,51,40))return color(96,113,103);
        if(box(x,y,43,34,59,40))return color(y==34?184:40,y==34?176:52,y==34?137:48);
        if(state==2&&abs(x-58)+abs(y-36)<10)return color(255,207,106);
        int lf=63-(a<0?-a:0),rf=63-(a>0?a:0);
        if(box(x,y,23,46+a,30,51+a)||box(x,y,35,46-a,42,51-a))return color(188,189,163);
        if(box(x,y,23,40,29,lf-4)||box(x,y,35,40,41,rf-4))return color(49,64,60);
        if(box(x,y,19,lf-6,31,lf)||box(x,y,34,rf-6,46,rf))return color(y%3?93:171,y%3?111:176,y%3?101:147);
    }else if(kind==1){
        for(int j=0;j<3;j++){
            int yy=38+j*10+((phase+j)&1?2:-2),tip=3+j*2;
            if(box(x,y,tip,yy,23,yy+3)||box(x,y,41,yy,63-tip,yy+3))return color(y==yy?210:71,y==yy?161:87,y==yy?70:77);
            if(box(x,y,tip,yy+3,tip+3,yy+6)||box(x,y,60-tip,yy+3,63-tip,yy+6))return color(39,53,51);
        }
        if(box(x,y,16,24,48,49)){
            if(y<27&&(x<19||x>45))return 0;
            if(x==16||x==48||y==49)return color(26,35,34);
            if(y==24)return color(243,201,112);
            if(y<29)return color(211+n,164+n,79+n);
            if(box(x,y,20,42,44,48))return color(x%6<3?213:41,x%6<3?207:53,x%6<3?173:50);
            if(y>32&&y<36)return color(255,state==1?184:66,35);
            if(state==4&&abs(x-31)+abs(y-38)<4)return color(255,224,151);
            return color(95+n,115+n,98+n);
        }
        if(state==2&&box(x,y,17,47,47,51))return color(239,187,89);
    }else if(kind==2||kind==3||kind==7||kind==8){
        y=23+y*30/63; /* fill the world-height cell and rest on the floor */
        if(box(x,y,10,23,53,53)){
            if(x==10||y==53||x==53)return color(27,36,34);
            if(y<27)return color(219,210,175);
            if(kind==3){if((x>27&&x<36)||(y>35&&y<43))return color(222,66,44);return color(225,225,199);}
            if(kind==7){if(box(x,y,17,33,47,36)||box(x,y,31,36,39,43))return color(208,211,190);return color(62,89,82);}
            if(kind==8){if(y>32&&y<47&&x%10<6)return color(y>43?193:167,y>43?151:61,y>43?73:42);return color(62,75,63);}
            if(x%10<3)return color(209,173,80);
            return color(67,87,66);
        }
    }else if(kind==4){
        if(box(x,y,6,4,57,60)){if(x<10||x>53||y<9||y>55)return color(176,184,156);if(box(x,y,16,15,47,36))return color(y%5?35:72,y%5?105:163,76);if(y>43&&x%8<3)return color(220,178,80);return color(45,60,54);}
    }else if(kind==5){if(box(x,y,6,5,57,58)){if(x<10||x>53||y<9||y>55)return color(211,178,83);if(y>28&&y<34&&x>17&&x<47)return color(135,218,157);return color(31,48,40);}}
    if(kind==9&&box(x,y,3,10,60,58)){
        if(x<6||x>57||y<13||y>55)return color(172,160,111);
        if(phase==2){ /* three warning studs identify the scored service panel */
            if(y>27&&y<38&&(abs(x-17)<4||abs(x-32)<4||abs(x-47)<4))return color(225,168,67);
        }else if(phase==1){ /* west-stair arrow, deliberately no tiny text */
            if((x>14&&x<50&&y>29&&y<36)||(x<29&&abs(y-32)<29-x))return color(226,183,84);
        }else{ /* station 09: original block numerals */
            if(box(x,y,12,22,27,45)&&(x<16||x>23||y<26||y>41))return color(218,214,180);
            if(box(x,y,35,22,50,45)&&(x>46||y<26||(y>31&&y<36)||(x<39&&y<34)))return color(218,214,180);
        }
        return color(35,53,49);
    }
    return 0;
}
uint16_t rn_weapon(int x,int y,int flash,int reload,int weapon,bool reduced_motion)
{
    if(x<0||y<0||x>=128||y>=96)return 0;
    int n=(int)(hash(x,y)%5)-2;
    int duration=weapon?1500:900;
    bool open=reload&&(reduced_motion||(reload>duration/4&&reload<duration*4/5));
    if(reload){double f=(double)reload/duration;int dip=reduced_motion?12:(int)(sin(f*RN_PI)*16);y-=dip;x-=dip/3;}
    /* Sparse asymmetric muzzle crown, with a pale hot core. */
    if(flash){
        int dx=abs(x-59),dy=abs(y-10);
        if(dx+dy<8)return color(255,246,200);
        if((dx+dy<(weapon?19:14)&&(dx<5||dy<4))||
           (x>60&&x<70&&y>2&&y<7))return color(247,176,66);
    }
    if(weapon==RN_SHOTGUN){
        /* Two separate bores, a raised joining rib and tapered steel shrouds. */
        if(y>=17&&y<57&&x>43-(y-17)/9&&x<76+(y-17)/6){
            if(y<24){
                if(box(x,y,47,19,55,23)||box(x,y,64,19,72,23))return color(12,21,24);
                return color(y==17?183:79,y==17?192:96,y==17?179:94);
            }
            if(x==59||x==60)return color(195,198,175);
            if(x<46||x>74+(y-17)/6)return color(135+n,155+n,151+n);
            if(x==48||x==66)return color(168,181,171);
            return color(x<59?47:62,x<59?66:82,x<59?70:81);
        }
        if(y>=53&&x>32+(y-53)/3&&x<87+(y-53)/2){
            if(open&&y<66){
                if((x>51&&x<59)||(x>67&&x<75))return color(y<59?223:156,y<59?179:62,y<59?83:44);
                return color(22,32,34);
            }
            if(y<58)return color(171,184,167);
            if(y<78){if(y%6<2)return color(52,53,42);return color(x<46?199:149,x<46?144:99,54);}
            if(x==82||y==82)return color(126,144,121);
            return color(42+n,61+n,56+n);
        }
    }else{
        /* Hooded front sight, narrow barrel and an asymmetric box receiver. */
        if(box(x,y,51,10,54,25)||box(x,y,64,10,67,25)||box(x,y,51,10,67,12))return color(y==10?142:24,y==10?156:36,y==10?143:37);
        if(box(x,y,58,15,60,21))return color(214,192,128);
        if(y>=22&&y<52&&x>52-(y-22)/7&&x<66+(y-22)/5){
            if(x<53)return color(166+n,183+n,170+n);
            if(x==56)return color(104,126,119);
            if(x>65)return color(27,45,46);
            return color(66+n,90+n,87+n);
        }
        if(y>=47&&x>31+(y-47)/2&&x<87+(y-47)/2){
            if(y<52)return color(184+n,198+n,175+n);
            if(x<42+(y-47)/2)return color(101,122,103);
            if(box(x,y,63,56,88,70)){
                if(y==56||x==63)return color(139,154,132);
                if(y==70||x==88)return color(20,33,34);
                if(open)return color(x<74?19:164,x<74?29:128,x<74?30:65);
                if(y<60)return color(195+n,158+n,79+n);
                return color(42,57,52);
            }
            if(y==75&&x>61&&x<89)return color(134,149,120);
            if((x==61&&y==54)||(x==88&&y==74))return color(214,212,171);
            if(y>78&&x>75&&x<81)return color(141,157,130);
            return color(41+n,63+n,58+n);
        }
        /* Magazine well and visible brass-tipped replacement during reload. */
        if(open&&box(x,y,37,67,51,88))return color(y<70?202:56,y<70?161:70,y<70?77:58);
    }
    /* Glove knuckles, cuff and a dark seam remain legible at native scale. */
    if(y>71&&x>17+(y-71)/2&&x<56+(y-71)/2){
        if(y>88)return color(y==89?35:70,y==89?47:86,y==89?41:66);
        if(y%8==0)return color(44,57,46);
        if(x<27+(y-71)/2)return color(151,153,111);
        return color(103+n,117+n,84+n);
    }
    return 0;
}
/* Small original bitmap lettering: only the title's uppercase alphabet.
 * Seven 5-bit rows per glyph; no external font or logo raster. */
static const unsigned char letters[][7]={
 {30,17,17,30,20,18,17}, /* R */ {31,16,16,30,16,16,31}, /* E */
 {16,16,16,16,16,16,31}, /* L */ {14,17,17,31,17,17,17}, /* A */
 {17,17,10,4,4,4,4}, /* Y */ {17,25,25,21,19,19,17}, /* N */
 {31,4,4,4,4,4,31} /* I */ };
static void word(struct rn_frame *f,const char *s,int x,int y,int scale,uint16_t c)
{
    const char *alphabet="RELAYNI";
    for(;*s;s++,x+=6*scale){const char *p=strchr(alphabet,*s);if(!p)continue;int g=(int)(p-alphabet);
        for(int j=0;j<7;j++)for(int i=0;i<5;i++)if(letters[g][j]&(16>>i))for(int yy=0;yy<scale;yy++)for(int xx=0;xx<scale;xx++){
            int px=x+i*scale+xx,py=y+j*scale+yy;if(px>=0&&px<RN_W&&py>=0&&py<RN_H)f->pixels[py*RN_W+px]=c;
        }
    }
}
void rn_title(struct rn_frame *f)
{
    memset(f,0,sizeof(*f));
    for(int y=0;y<RN_H;y++)for(int x=0;x<RN_W;x++){
        int n=(int)(hash(x,y)%4);uint16_t c=color(18+n,30+n,29+n);
        if(x%32==0||y%32==0)c=color(28,42,39);
        if((y==18||y==179)&&x>18&&x<302)c=color(100,112,94);
        if(y>174&&y<180&&x>18&&x<70)c=color((x/7)%2?197:29,(x/7)%2?154:41,(x/7)%2?68:36);
        /* Relay trace emblem: open square, nine contacts, interrupted link. */
        if(box(x,y,25,44,86,47)||box(x,y,25,44,28,122)||box(x,y,25,119,86,122)||box(x,y,83,44,86,78)||box(x,y,83,98,86,122))c=color(207,164,76);
        for(int i=0;i<3;i++)for(int j=0;j<3;j++)if(box(x,y,37+i*15,60+j*18,43+i*15,66+j*18))c=color(202,213,182);
        if(box(x,y,83,86,102,90))c=color(231,85,44);
        f->pixels[y*RN_W+x]=c;
    }
    word(f,"RELAY",112,49,5,color(226,225,198));word(f,"NINE",112,98,5,color(208,167,77));
    /* Power indicator: one failing section, four steady reserves. */
    for(int i=0;i<5;i++)for(int y=151;y<155;y++)for(int x=113+i*29;x<136+i*29;x++)f->pixels[y*RN_W+x]=i?color(79,115,88):color(227,85,43);
}
