/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "now_playing.h"
#include "player_ui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
int lab_airplay_thread_create(pthread_t *t,void *(*f)(void *),void *a){return pthread_create(t,NULL,f,a);}
static unsigned tag(unsigned char *p,const char *name,const void *text,unsigned n){memcpy(p,name,4);p[4]=n>>24;p[5]=n>>16;p[6]=n>>8;p[7]=n;memcpy(p+8,text,n);return n+8;}
int main(int argc,char **argv){
    assert(argc==3);assert(np_start()==0);unsigned char data[2048],nested[2100];unsigned n=0;
    const char *name="Невесомость / Weightless";n+=tag(data+n,"minm",name,(unsigned)strlen(name));n+=tag(data+n,"asar","Aurora Ensemble",15);n+=tag(data+n,"asal","Midnight Sessions",17);
    n=tag(nested,"mlit",data,n);np_metadata(nested,(int)n);NowPlaying value;unsigned long revision=0;
    np_snapshot(&value,NULL,&revision);assert(!strcmp(value.title,name));assert(!strcmp(value.artist,"Aurora Ensemble"));assert(value.active);
    np_metadata("minm\xff\xff\xff\xff",8);LabState state={0};np_poll(&state);assert(state.metadata_packets==1 && state.metadata_errors==1);
    np_audio(48000);np_progress(0xffff0000,0xffff0000+48000,0xffff0000u+48000u*240u);np_snapshot(&value,NULL,&revision);assert(value.position>=1 && value.position<1.1 && value.duration==240);
    FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);unsigned char *cover=malloc((size_t)size);assert(fread(cover,1,(size_t)size,f)==(size_t)size);fclose(f);np_coverart(cover,(int)size);free(cover);
    for(int i=0;i<1000;++i){np_snapshot(&value,NULL,&revision);if(value.has_art)break;usleep(1000);}assert(value.has_art);
    uint32_t *pixels=malloc(LAB_WIDTH*LAB_HEIGHT*4);assert(pixels);state.airplay_started=1;strcpy(state.ip,"192.168.0.58");
    np_progress(1000,1000+48000*83,1000+48000*240);player_ui_draw(&state,pixels);
    f=fopen(argv[2],"wb");assert(f);fprintf(f,"P6\n%d %d\n255\n",LAB_WIDTH,LAB_HEIGHT);
    for(int i=0;i<LAB_WIDTH*LAB_HEIGHT;++i){unsigned char rgb[3]={pixels[i]>>16,pixels[i]>>8,pixels[i]};fwrite(rgb,1,3,f);}fclose(f);
    /* Cached draw must restore a target invalidated by video/diagnostics. */
    uint32_t expected=pixels[220*LAB_WIDTH+200];pixels[220*LAB_WIDTH+200]=0;player_ui_invalidate();player_ui_draw(&state,pixels);assert(pixels[220*LAB_WIDTH+200]==expected);
    np_stop();np_snapshot(&value,NULL,&revision);assert(!value.active);player_ui_draw(&state,pixels);
    player_ui_shutdown();np_shutdown();free(pixels);puts("NowPlaying: DMAP/UTF8, malformed lengths, async JPEG, RTP wrap/progress, UI cache passed");return 0;
}
