/* Original pixel artwork recipes: worn steel/concrete, articulated machines,
 * carbine and pickups. No imported assets or concept-image pixels.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "relay.h"
#include <math.h>
#include <stdlib.h>
static uint16_t textures[6][64*64];
static bool ready;
static uint16_t color(int r,int g,int b)
{r=r<0?0:r>255?255:r;g=g<0?0:g>255?255:g;b=b<0?0:b>255?255:b;return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));}
static unsigned hash(unsigned x,unsigned y){unsigned v=x*374761393u+y*668265263u;v=(v^(v>>13))*1274126177u;return v^(v>>16);}
static void init(void)
{
    for(int m=0;m<6;m++)for(int y=0;y<64;y++)for(int x=0;x<64;x++){
        int noise=(int)(hash(x+m*71,y)%19)-9,r=90,g=90,b=80;
        if(m==0){ /* steel plates, seams, rivets and a recessed utility pipe */
            r=73+noise;g=77+noise;b=70+noise;
            if(x<2||y<2||y==31){r=24;g=26;b=24;}
            if((x==5||x==58)&&(y==5||y==27||y==58)){r=155;g=147;b=123;}
            if(x>44&&x<51){r=105+noise;g=108+noise;b=97+noise;if(x==45||x==50)r=g=b=35;}
            if(y>8&&y<14&&x>12&&x<34){r=45;g=49;b=44;}
            if(y==10&&x>14&&x<30){r=174;g=141;b=65;}
        }else if(m==1){r=143+noise;g=137+noise;b=119+noise;if(y<2||x==0){r-=45;g-=45;b-=40;}if((hash(x/3,y/8)&15)==0){r-=22;g-=25;b-=24;}}
        else if(m==2){r=75+noise;g=78+noise;b=73+noise;if(x<3||x>60||y%16<2)r=g=b=26;if(y>48){bool stripe=((x+y)/7)%2;r=stripe?174:29;g=stripe?140:30;b=stripe?48:25;}if(x>27&&x<36){r=35;g=37;b=32;}if(x==29&&y>18&&y<43){r=150;g=44;b=30;}}
        else if(m==3){r=94+noise;g=89+noise;b=74+noise;if(x%16<2||y%16<2){r=43;g=44;b=37;}if(x%16==3||y%16==3){r=130;g=121;b=94;}}
        else if(m==4){r=96+noise;g=94+noise;b=81+noise;if(y<2||x<2){r=32;g=34;b=29;}if(y>54){bool stripe=(x/7)%2;r=stripe?178:35;g=stripe?148:35;b=stripe?61:29;}}
        else {r=58+noise/2;g=61+noise/2;b=54+noise/2;if(y%32<3||x%32<2)r=g=b=24;}
        textures[m][y*64+x]=color(r,g,b);
    }ready=true;
}
uint16_t rn_texture(int m,int x,int y,int light)
{
    if(!ready)init();
    if(m<0||m>5)m=1;
    uint16_t c=textures[m][((y&63)*64)+(x&63)];
    int r=((c>>11)&31)*8,g=((c>>5)&63)*4,b=(c&31)*8;
    light=light<40?40:light>255?255:light;
    return color(r*light/255,g*light/255,b*light/255);
}
static bool box(int x,int y,int x0,int y0,int x1,int y1){return x>=x0&&x<=x1&&y>=y0&&y<=y1;}
uint16_t rn_sprite(int kind,int x,int y,int phase,int facing,int state)
{
    if(x<0||y<0||x>=64||y>=64)return 0;
    int n=(int)(hash(x,y)%9)-4;
    if(kind<2&&state==4&&box(x,y,22,15,42,35)&&((x+y)&7)<2)return color(220,211,167);
    if(kind==0&&state==1&&box(x,y,24,8,40,10))return color(255,169,55);
    if(kind==0){
        int lean=(facing==2||facing==6)?5:0;
        x=(int)((x-32)*(lean?1.4:1))+32;
        static const int walk[4]={-3,0,3,0};
        int gait=walk[phase&3];if(state==3){y=48+(y-48)*3;gait=0;}
        if(box(x,y,22,13,42,39)){
            if(y<18)return color(78+n,83+n,73+n);
            if(box(x,y,27,22,37,32))return color(43,45,39);
            if(y==33||x==23||x==41)return color(174,159,115);
            return color(111+n,116+n,102+n);
        }
        if(box(x,y,12,14,22,25)||box(x,y,42,14,52,25))return color(y<17?199:149, y<17?192:147,y<17?166:128);
        if(box(x,y,24,3,40,13)){
            if(y>=8&&y<=10&&facing!=4)return color(240,42,20);
            if(x<27||y<6)return color(170+n,166+n,145+n);
            return color(55+n,59+n,51+n);
        }
        if(box(x,y,16,24,22,43))return color(75+n,79+n,66+n);
        if(box(x,y,43,24,49,39))return color(75+n,79+n,66+n);
        if(box(x,y,42,32,57,38))return color(x>52?41:112,x>52?44:104,x>52?37:77);
        if(state==2&&box(x,y,55,28,63,42)&&abs(x-58)+abs(y-35)<9)return color(255,206,92);
        if(box(x,y,22,38,30,51+gait)||box(x,y,35,38,43,51-gait))return color(56+n,63+n,55+n);
        if(box(x,y,19,50+gait,31,56+gait)||box(x,y,34,50-gait,46,56-gait))return color(105+n,107+n,90+n);
    }else if(kind==1){
        if(state==3)y=45+(y-45)*3;
        for(int j=0;j<3;j++){int yy=34+j*8+((phase+j)&1?2:-2);if((box(x,y,4+j*2,yy,22,yy+3)||box(x,y,42,yy,59-j*2,yy+3)))return color(104+n,100+n,75+n);}
        if(box(x,y,17,26,46,48)){if(y<30)return color(174+n,156+n,99+n);if(y>42&&x%5<3)return color(181,166,122);if(y>34&&y<38)return color(212,49,22);return color(70+n,78+n,61+n);}
    }else if(kind==2){if(box(x,y,12,25,51,50)){if(y<29)return color(158,145,93);if(x%10<3)return color(203,174,83);return color(70,76,53);}}
    else if(kind==3){if(box(x,y,12,23,51,51)){if((x>27&&x<36)||(y>34&&y<42))return color(178,47,32);return color(204+n,202+n,173+n);}}
    else if(kind==4){if(box(x,y,6,4,57,60)){if(x<10||x>53||y<9||y>55)return color(149,153,120);if(box(x,y,17,17,46,36))return color(28,93,52);if(y>40&&x%8<3)return color(204,173,70);return color(52,59,49);}}
    else if(kind==6){
        if(y>4&&y<60&&x>9&&x<55){int shade=150-abs(x-32)*3+n;
            if(y<9||y>54||y==23||y==44)return color(47,51,44);
            if(y>25&&y<41&&x>21&&x<45)return color(63,66,53);
            if(y==30&&x>24&&x<42)return color(182,155,88);
            return color(shade,shade,shade-15);
        }
    }
    else if(kind==5){if(x<6||x>57||y<5||y>58)return 0;if(x<10||x>53||y<9||y>55)return color(180,154,69);if(y>28&&y<34&&x>17&&x<47)return color(124,193,113);return color(31,45,33);}
    if(state==1&&kind<2&&y<15&&x>25&&x<40)return color(252,162,41);
    return 0;
}
uint16_t rn_weapon(int x,int y,int flash,int reload)
{
    if(x<0||y<0||x>=128||y>=96)return 0;
    if(reload){x-=15;y-=9;}
    int n=(int)(hash(x,y)%11)-5;
    /* Perspective barrel, rectangular front sight, receiver, brass breech,
     * stock and gloved support hand, all intentionally asymmetric. */
    if(y>=20&&y<55&&x>=53-(y-20)/4&&x<=63+(y-20)/3){if(x<57)return color(132+n,135+n,120+n);return color(43+n,48+n,42+n);}
    if(box(x,y,54,13,57,27)||box(x,y,61,13,64,27)||box(x,y,54,13,64,16))return color(37,42,37);
    if(y>=48&&x>34+(y-48)/2&&x<85+(y-48)/2){
        if(y<54||x%17<2)return color(148+n,144+n,119+n);
        if(box(x,y,63,57,85,71))return color(164+n,132+n,65+n);
        if(x<57)return color(74+n,82+n,65+n);
        return color(47+n,52+n,45+n);
    }
    if(y>67&&x>19+(y-67)/2&&x<55+(y-67)/2)return color(89+n,80+n,56+n);
    if(flash&&abs(x-59)+abs(y-9)<12)return color(255,220,135);
    return 0;
}
