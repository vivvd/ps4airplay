/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "player_ui.h"
#include "now_playing.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "noto_font.h"
static stbtt_fontinfo font;
static int font_ready;
static uint32_t *canvas,*art,*layout;
static unsigned long art_revision,revision=(unsigned long)-1,cache_id;
static long second=-1;
static int cached_language=-1,cached_settings=-1,cached_item=-1;
static int cached_mode=-1,cached_ready=-1,cached_error,cached_playing=-1;
static struct {uint32_t *ptr;unsigned long id;} targets[2];
static struct {unsigned code;int size,w,h,x,y,advance;unsigned char *pixels;} glyphs[512];
static unsigned glyph_cursor;
static void rect(int x,int y,int w,int h,uint32_t color){
    if(x<0 || y<0 || x+w>LAB_WIDTH || y+h>LAB_HEIGHT || w<0 || h<0)return;
    for(int j=0;j<h;++j)for(int i=0;i<w;++i)canvas[(y+j)*LAB_WIDTH+x+i]=color;
}
static unsigned utf8(const char **text){
    const unsigned char *p=(const unsigned char *)*text;unsigned c=*p++;int n=0;
    if(c<128){*text=(const char *)p;return c;}
    if(c>=0xc2 && c<0xe0){c&=31;n=1;}else if(c<0xf0 && c>=0xe0){c&=15;n=2;}else if(c>=0xf0 && c<0xf5){c&=7;n=3;}else{*text=(const char *)p;return 0xfffd;}
    for(int i=0;i<n;++i){if(!*p || (*p&0xc0)!=0x80){*text=(const char *)p;return 0xfffd;}c=(c<<6)|(*p++&63);}
    *text=(const char *)p;return c<=0x10ffff?c:0xfffd;
}
static int glyph(unsigned code,int size){
    for(unsigned i=0;i<512;++i)if(glyphs[i].pixels && glyphs[i].code==code && glyphs[i].size==size)return (int)i;
    unsigned i=glyph_cursor++%512;free(glyphs[i].pixels);memset(&glyphs[i],0,sizeof(glyphs[i]));
    glyphs[i].code=code;glyphs[i].size=size;
    float scale=stbtt_ScaleForPixelHeight(&font,(float)size);int advance=0;
    stbtt_GetCodepointHMetrics(&font,(int)code,&advance,NULL);glyphs[i].advance=(int)(advance*scale+0.5f);
    glyphs[i].pixels=stbtt_GetCodepointBitmap(&font,0,scale,(int)code,&glyphs[i].w,&glyphs[i].h,&glyphs[i].x,&glyphs[i].y);
    return (int)i;
}
static int text_width(const char *text,int size){int width=0;while(*text){unsigned c=utf8(&text);width+=glyphs[glyph(c,size)].advance;}return width;}
static void text(int x,int baseline,const char *value,int size,uint32_t color,int maxwidth){
    int start=x;while(*value){unsigned c=utf8(&value);int i=glyph(c,size);
        if(x-start+glyphs[i].advance>maxwidth)break;
        for(int y=0;y<glyphs[i].h;++y)for(int j=0;j<glyphs[i].w;++j){
            int px=x+glyphs[i].x+j,py=baseline+glyphs[i].y+y;
            if(px<0 || px>=LAB_WIDTH || py<0 || py>=LAB_HEIGHT)continue;
            unsigned a=glyphs[i].pixels[y*glyphs[i].w+j];uint32_t old=canvas[py*LAB_WIDTH+px],out=0xff000000;
            for(int k=0;k<3;++k){unsigned shift=k*8;out|=((((old>>shift)&255)*(255-a)+((color>>shift)&255)*a+127)/255)<<shift;}
            canvas[py*LAB_WIDTH+px]=out;
        }
        x+=glyphs[i].advance;
    }
}
static void title(const char *value,int x,int y,int width){
    int size=60;if(text_width(value,size)>width*2)size=48;
    char line[512];size_t n=0;int w=0;const char *p=value,*rest=value;
    while(*p){const char *before=p;unsigned c=utf8(&p);int advance=glyphs[glyph(c,size)].advance;
        if(w+advance>width)break;size_t bytes=(size_t)(p-before);if(n+bytes>=sizeof(line))break;
        memcpy(line+n,before,bytes);n+=bytes;w+=advance;rest=p;
    }
    line[n]=0;text(x,y,line,size,0xfffafafa,width);
    if(*rest)text(x,y+70,rest,size,0xfffafafa,width);
}
static void backdrop(int has_art){
    uint32_t colors[36*20];
    for(int y=0;y<20;++y)for(int x=0;x<36;++x){
        unsigned r=0,g=0,b=0,n=0;
        if(has_art){for(int j=-4;j<=4;++j)for(int i=-4;i<=4;++i){int ax=(x+i)*NP_ART_SIZE/36,ay=(y+j)*NP_ART_SIZE/20;if(ax<0)ax=0;if(ay<0)ay=0;if(ax>=NP_ART_SIZE)ax=NP_ART_SIZE-1;if(ay>=NP_ART_SIZE)ay=NP_ART_SIZE-1;uint32_t c=art[ay*NP_ART_SIZE+ax];r+=(c>>16)&255;g+=(c>>8)&255;b+=c&255;++n;}}
        colors[y*36+x]=has_art?0xff000000|((r/n*27/100+8)<<16)|((g/n*27/100+8)<<8)|(b/n*27/100+10):0xff10151f;
    }
    for(int y=0;y<LAB_HEIGHT;++y)for(int x=0;x<LAB_WIDTH;++x){
        float fx=x*35.0f/(LAB_WIDTH-1),fy=y*19.0f/(LAB_HEIGHT-1);int ix=(int)fx,iy=(int)fy,nx=ix<35?ix+1:ix,ny=iy<19?iy+1:iy;float ax=fx-ix,ay=fy-iy;
        uint32_t a=colors[iy*36+ix],b=colors[iy*36+nx],c=colors[ny*36+ix],d=colors[ny*36+nx],out=0xff000000;
        for(int k=0;k<3;++k){int shift=k*8;float v=(1-ay)*((1-ax)*((a>>shift)&255)+ax*((b>>shift)&255))+ay*((1-ax)*((c>>shift)&255)+ax*((d>>shift)&255));out|=(unsigned)v<<shift;}
        canvas[y*LAB_WIDTH+x]=out;
    }
}
static void cover(int has_art){
    for(int y=0;y<600;++y)for(int x=0;x<600;++x){int dx=x<26?26-x:x>573?x-573:0,dy=y<26?26-y:y>573?y-573:0;if(dx*dx+dy*dy>26*26)continue;
        canvas[(y+220)*LAB_WIDTH+x+160]=has_art?art[y*600+x]:0xff26303e;
    }
    if(!has_art){rect(480,405,14,180,0xffcad0da);rect(390,430,14,180,0xffcad0da);rect(390,415,104,20,0xffcad0da);
        for(int y=-22;y<=22;++y)for(int x=-30;x<=30;++x)if(x*x*484+y*y*900<=900*484){canvas[(600+y)*LAB_WIDTH+376+x]=0xffcad0da;canvas[(575+y)*LAB_WIDTH+466+x]=0xffcad0da;}}
}
static void time_label(char *out,size_t size,double seconds){if(!isfinite(seconds) || seconds<0)seconds=0;unsigned n=(unsigned)seconds;if(n>=3600)snprintf(out,size,"%u:%02u:%02u",n/3600,n/60%60,n%60);else snprintf(out,size,"%u:%02u",n/60,n%60);}
void player_ui_invalidate(void){for(int i=0;i<2;++i)targets[i].id=0;}
void player_ui_draw(const LabState *s,uint32_t *pixels){
    if(!font_ready){font_ready=stbtt_InitFont(&font,noto_font,stbtt_GetFontOffsetForIndex(noto_font,0));}
    if(!canvas)canvas=malloc(LAB_WIDTH*LAB_HEIGHT*4);if(!art)art=calloc(NP_ART_SIZE*NP_ART_SIZE,4);if(!layout)layout=malloc(LAB_WIDTH*LAB_HEIGHT*4);
    if(!font_ready || !canvas || !art || !layout){memset(pixels,0,LAB_WIDTH*LAB_HEIGHT*4);return;}
    NowPlaying np;np_snapshot(&np,art,&art_revision);
    int mode=s->direct_active?2:np.active?1:0;long sec=(long)np.position;
    int ru=s->language;
    int changed=cached_language!=ru || cached_settings!=s->settings_open || cached_item!=s->settings_item || revision!=np.revision || cached_mode!=mode || cached_ready!=s->airplay_started || cached_error!=s->direct_error || cached_playing!=np.playing;
    if(changed){
        backdrop(mode==1 && np.has_art);
        text(160,128,"PS4 AirPlay",30,0xffd7dbe3,1100);
        if(mode==1){
            cover(np.has_art);text(880,278,np.playing?(ru?"СЕЙЧАС ИГРАЕТ":"NOW PLAYING"):(ru?"МУЗЫКА":"MUSIC"),23,0xffb9c0ca,840);
            title(np.title[0]?np.title:(ru?"Музыка с iPhone":"Music from iPhone"),880,385,850);
            text(880,555,np.artist,38,0xffd9dce3,850);text(880,610,np.album,29,0xffaeb5c1,850);
            text(880,835,ru?"Управление воспроизведением — на iPhone":"Control playback on your iPhone",24,0xff8f98a7,850);
        }else if(mode==2){text(160,455,ru?"Открываем видео":"Opening video",64,0xfffafafa,1500);text(160,535,ru?"Поток передан с iPhone. Ожидаем изображение…":"Stream received from iPhone. Waiting for video…",32,0xffadb6c7,1500);}
        else{
            text(160,430,s->recovery_mode?(ru?"Требуется перезапуск AirPlay":"AirPlay needs to restart"):s->airplay_started?(ru?"Готов к воспроизведению":"Ready to play"):(ru?"Запускаем AirPlay…":"Starting AirPlay…"),62,0xfffafafa,1600);
            text(160,520,ru?"Откройте AirPlay на iPhone и выберите PS4 AirPlay":"Open AirPlay on iPhone and select PS4 AirPlay",34,0xffbdc5d3,1600);
            text(160,585,ru?"Музыка · Видео из плеера · Повтор экрана":"Music · Video · Screen Mirroring",29,0xff8f9aaf,1600);
            if(s->direct_error)text(160,680,ru?"Не удалось открыть видео. Попробуйте другой поток.":"Could not open video. Try another source.",29,0xfff0b4a6,1600);
        }
        char footer[200];snprintf(footer,sizeof(footer),ru?"%s:%d   ·   SQUARE Диагностика   ·   OPTIONS Настройки   ·   %s":"%s:%d   ·   SQUARE Diagnostics   ·   OPTIONS Settings   ·   %s",s->ip,LAB_PORT,LAB_VERSION);
        text(160,990,footer,23,0xff7f8b9e,1600);
        if(s->settings_open){
            rect(400,230,1120,590,0xff18202e);
            text(470,320,ru?"Настройки":"Settings",50,0xfffafafa,1000);
            const char *labels[]={ru?"Язык: Русский":"Language: English",ru?"Вернуться":"Back",ru?"Отключить AirPlay и выйти":"Disconnect AirPlay and exit"};
            for(int i=0;i<3;++i){if(i==s->settings_item)rect(450,365+i*100,1020,80,0xff30485a);text(480,420+i*100,labels[i],34,0xfffafafa,970);}
            text(470,755,ru?"Вверх/вниз — выбор   X — подтвердить   O — назад":"Up/Down: select   X: confirm   O: back",24,0xffbdc5d3,1000);
        }
        cached_language=ru;cached_settings=s->settings_open;cached_item=s->settings_item;
        memcpy(layout,canvas,LAB_WIDTH*LAB_HEIGHT*4);
        revision=np.revision;cached_mode=mode;cached_ready=s->airplay_started;cached_error=s->direct_error;cached_playing=np.playing;
    }
    if(changed || second!=sec){
        if(mode==1 && !s->settings_open){
            for(int y=705;y<790;++y)memcpy(canvas+y*LAB_WIDTH+880,layout+y*LAB_WIDTH+880,850*4);
            rect(880,722,850,5,0xff505560);if(np.duration>0){double fraction=np.position/np.duration;if(fraction<0)fraction=0;if(fraction>1)fraction=1;rect(880,722,(int)(850*fraction),5,0xfff1f2f5);}
            char label[32];time_label(label,sizeof(label),np.position);text(880,770,label,25,0xffc1c6cf,200);
            if(np.duration>0){time_label(label,sizeof(label),np.duration);text(1730-text_width(label,25),770,label,25,0xffc1c6cf,200);}
        }
        second=sec;++cache_id;
    }
    int target=targets[0].ptr==pixels?0:targets[1].ptr==pixels?1:targets[0].ptr?1:0;
    if(targets[target].ptr!=pixels || targets[target].id!=cache_id){memcpy(pixels,canvas,LAB_WIDTH*LAB_HEIGHT*4);targets[target].ptr=pixels;targets[target].id=cache_id;}
}
void player_ui_shutdown(void){for(unsigned i=0;i<512;++i)free(glyphs[i].pixels);free(canvas);free(art);free(layout);canvas=art=layout=NULL;}
