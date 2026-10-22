/* Original hand-authored pixel recipes: Relay Nine industrial identity.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "relay.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static uint16_t textures[6][64*64];
static bool ready;
static uint16_t color(int r,int g,int b)
{r=r<0?0:r>255?255:r;g=g<0?0:g>255?255:g;b=b<0?0:b>255?255:b;return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));}
static unsigned hash(unsigned x,unsigned y){unsigned v=x*374761393u+y*668265263u;v=(v^(v>>13))*1274126177u;return v^(v>>16);}
static bool box(int x,int y,int a,int b,int c,int d){return x>=a&&x<=c&&y>=b&&y<=d;}
static void init(void)
{
    for(int m=0;m<6;m++)for(int y=0;y<64;y++)for(int x=0;x<64;x++){
        int n=(int)(hash(x+m*71,y)%7)-3,r=90,g=90,b=80;
        if(m==0){ /* two-meter painted-steel panels, fine wear, small fittings */
            r=80+n;g=89+n;b=82+n;
            if(x<2||y<2){r=24;g=31;b=30;}
            if(x==2||y==2){r=116;g=120;b=105;}
            if((x==6||x==58)&&(y==6||y==58)){r=173;g=165;b=138;}
            if(box(x,y,42,5,48,58)){r=46+n;g=52+n;b=49+n;if(x==43){r=128;g=130;b=112;}}
            if(box(x,y,9,30,30,42)){r=31;g=41;b=39;if(y%3==0&&x>12&&x<27){r=63;g=74;b=68;}}
            if(box(x,y,12,16,26,18)){r=202;g=156;b=71;}
            if((hash(x/2,y)&255)==1){r+=25;g+=17;b+=4;}
        }else if(m==1){ /* broad concrete blocks and sparse water streaks */
            r=146+n;g=143+n;b=127+n;
            if(y<2||x<1){r=73;g=78;b=70;}
            if(y==2){r=166;g=164;b=144;}
            if((hash(x/3,0)&31)==0&&y>10){r-=18;g-=17;b-=10;}
            if((hash(x,y)&511)==0){r-=32;g-=32;b-=28;}
        }else if(m==2){ /* steel lift door, recessed ribs, slim safety bands */
            r=99+n;g=108+n;b=101+n;
            if(x<4||x>59||x==31||x==32){r=26;g=36;b=35;}
            if(y%16==0){r=49;g=58;b=55;}
            if(y%16==1){r=148;g=151;b=132;}
            if(y>53&&y<61){bool stripe=((x+y)/8)%2;r=stripe?204:31;g=stripe?161:37;b=stripe?63:33;}
            if(box(x,y,27,21,36,36)){r=29;g=39;b=37;if(y==26&&x>29&&x<34){r=222;g=83;b=38;}}
        }else if(m==3){ /* half-meter inset floor panels, fine cross tread */
            r=82+n;g=87+n;b=79+n;
            if(x%16<2||y%16<2){r=35;g=43;b=40;}
            else if((x+y)%8==0&&x%16>4&&y%16>4){r=112;g=113;b=96;}
            if(x%16==2||y%16==2){r=129;g=127;b=108;}
        }else if(m==4){
            r=114+n;g=119+n;b=105+n;
            if(x%8==0){r=64;g=73;b=65;}
            if(y<3){r=36;g=43;b=38;}
            if(y>57){bool stripe=(x/8)%2;r=stripe?202:32;g=stripe?160:39;b=stripe?65:35;}
        }else{r=61+n;g=71+n;b=68+n;if(x%32<2||y%32<2){r=26;g=35;b=34;}if(y==6&&x>10&&x<22){r=178;g=150;b=94;}}
        textures[m][y*64+x]=color(r,g,b);
    }ready=true;
}
uint16_t rn_texture(int m,int x,int y,int light)
{
    if(!ready)init();
    if(m<0||m>5)m=1;
    uint16_t c=textures[m][((y&63)*64)+(x&63)];
    int r=((c>>11)&31)*8,g=((c>>5)&63)*4,b=(c&31)*8;
    light=light<55?55:light>255?255:light;return color(r*light/255,g*light/255,b*light/255);
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
            if(x==9||x==55||y==29)return color(23,31,32);
            if(y<18)return color(226,219,188);
            return color(182+n,185+n,162+n);
        }
        if(box(x,y,23,15,41,40)){
            if(x==23||x==41||y==40)return color(25,33,34);
            if(back&&box(x,y,27,20,37,34))return color(y%4?46:133, y%4?58:139, y%4?55:119);
            if(box(x,y,27,23,37,31))return color(49,63,60);
            if(y==19||y==35)return color(211,172,79);
            if(state==4&&y>26&&y<34&&abs(x-32)==abs(y-30))return color(255,222,132);
            return color(137+n,150+n,138+n);
        }
        if(box(x,y,24,3,40,14)){
            if(y<6||x==24||x==40)return color(206,207,182);
            if(y>=8&&y<=10&&!back)return state==1?color(255,194,75):color(255,64,35);
            return color(39,51,51);
        }
        if(box(x,y,13,29,20,44))return color(74,90,84);
        if(box(x,y,44,28,51,40))return color(96,113,103);
        if(box(x,y,43,34,59,40))return color(y==34?184:40,y==34?176:52,y==34?137:48);
        if(state==2&&abs(x-58)+abs(y-36)<10)return color(255,207,106);
        int lf=63-(a<0?-a:0),rf=63-(a>0?a:0);
        if(box(x,y,23,40,29,lf-4)||box(x,y,35,40,41,rf-4))return color(49,64,60);
        if(box(x,y,25,43,27,51+a)||box(x,y,37,43,39,51-a))return color(188,189,163);
        if(box(x,y,19,lf-6,31,lf)||box(x,y,34,rf-6,46,rf))return color(y%3?93:171,y%3?111:176,y%3?101:147);
    }else if(kind==1){
        for(int j=0;j<3;j++){int yy=38+j*10+((phase+j)&1?2:-2);if(box(x,y,3+j*2,yy,23,yy+3)||box(x,y,41,yy,60-j*2,yy+3))return color(y==yy?210:71,y==yy?161:87,y==yy?70:77);}
        if(box(x,y,16,24,48,49)){
            if(x==16||x==48||y==49)return color(26,35,34);
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
    if(reload){int duration=weapon?1500:900;double f=(double)reload/duration;int dip=reduced_motion?12:(int)(sin(f*RN_PI)*24);y-=dip;x-=dip/3;}
    if(weapon==RN_SHOTGUN){
        /* Twin blued-steel tubes, brass open breech, ribbed amber foregrip. */
        if(y>15&&y<70&&x>45-(y-15)/6&&x<74+(y-15)/4){
            if(y<21)return color(23,31,33);
            if(x<49||x>70+(y-15)/6)return color(168+n,179+n,169+n);
            if(x==59||x==60)return color(27,37,39);
            return color(75+n,94+n,92+n);
        }
        if(y>=50&&x>32+(y-50)/3&&x<88+(y-50)/2){
            if(y<55||y%7<2)return color(48,52,44);
            if(y<76)return color(164+n,111+n,54+n);
            if(x<57)return color(105,106,77);
            return color(50,65,57);
        }
        if(flash&&y<23&&abs(x-59)+abs(y-9)<18)return color(255,217,126);
    }else{
        if(y>16&&y<54&&x>53-(y-16)/5&&x<64+(y-16)/4){if(x<56)return color(181+n,190+n,171+n);if(x>62)return color(45,62,60);return color(100+n,120+n,112+n);}
        if(box(x,y,53,11,56,27)||box(x,y,63,11,66,27)||box(x,y,53,11,66,14))return color(29,41,41);
        if(y>=47&&x>33+(y-47)/2&&x<87+(y-47)/2){
            if(y<52)return color(189+n,193+n,166+n);
            if(box(x,y,63,56,85,71)){if(y==56||y==71||x==63||x==85)return color(64,69,55);return color(193+n,150+n,72+n);}
            if(x<54&&y%6<2)return color(139,153,125);
            if(y>78&&x>75&&x<80)return color(195,193,162);
            return color(52+n,77+n,67+n);
        }
        if(flash&&abs(x-59)+abs(y-7)<11)return color(255,224,147);
    }
    if(y>70&&x>17+(y-70)/2&&x<56+(y-70)/2){if(y%9==0)return color(42,55,45);return color(110+n,117+n,84+n);}
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
