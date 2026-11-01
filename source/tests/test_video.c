/* Actual asynchronous H.264 decode and BGRA presentation, with encoded fixtures. */
#include "video_receiver.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
void lab_session_trace(const char *a,const char *b){(void)a;(void)b;}
static uint32_t screen[LAB_WIDTH*LAB_HEIGHT+2];
static unsigned char *first_idr;
static int first_size;
static unsigned long expected;
static void wait_frames(LabState *s,unsigned long count) {
    for(int i=0;i<1000;++i){lab_video_poll(s);if(s->video_decoded>=count)return;usleep(5000);}
    fprintf(stderr,"Video timeout: got %lu expected %lu errors=%lu drops=%lu\n",s->video_decoded,count,s->video_errors,s->video_dropped);
    abort();
}
static void wait_converted(LabState *s,unsigned long count) {
    for(int i=0;i<1000;++i){lab_video_poll(s);if(s->video_converted>=count)return;usleep(5000);}
    fprintf(stderr,"Conversion timeout\n");abort();
}
static void *blocked_producer(void *unused) {
    (void)unused;
    for(int i=0;i<1000;++i)lab_video_submit(first_idr,first_size,0,0);
    return NULL;
}
static atomic_int render_running;
static void *concurrent_renderer(void *unused) {
    (void)unused;
    uint32_t *pixels=malloc(LAB_WIDTH*LAB_HEIGHT*sizeof(*pixels));assert(pixels);
    while(atomic_load(&render_running)){
        lab_video_draw(pixels);usleep(500);
    }
    free(pixels);return NULL;
}
static void decode_fixture(LabState *s,const char *path,unsigned width,unsigned height,int color,int configure) {
    if(configure){assert(lab_video_configure(1,width,height)==0);assert(!lab_video_draw(screen+1));}
    lab_video_poll(s);expected=s->video_converted;
    FILE *f=fopen(path,"rb");assert(f);
    uint32_t length;unsigned packets=0;
    while(fread(&length,4,1,f)==1){
        assert(length>0 && length<1000000);
        unsigned char *data=malloc(length);assert(data);
        assert(fread(data,1,length,f)==length);
        if(!first_idr){first_idr=malloc(length);memcpy(first_idr,data,length);first_size=(int)length;}
        lab_video_submit(data,(int)length,0,packets*33333);
        memset(data,0,length);free(data); /* receiver must own its queued copy */
        wait_converted(s,++expected);
        screen[0]=0x12345678;screen[LAB_WIDTH*LAB_HEIGHT+1]=0x87654321;
        assert(lab_video_draw(screen+1));
        assert(screen[0]==0x12345678 && screen[LAB_WIDTH*LAB_HEIGHT+1]==0x87654321);
        uint32_t center=screen[1+(LAB_HEIGHT/2)*LAB_WIDTH+LAB_WIDTH/2];
        unsigned r=(center>>16)&255,g=(center>>8)&255,b=center&255;
        if(color==1)assert(r>200 && g<30 && b<30);
        else if(color==2)assert(g>200 && r<30 && b<30);
        else assert(b>200 && r<30 && g<30);
        /* Both vectors have aspect ratios differing from the 16:9 output. */
        if(width*LAB_HEIGHT!=height*LAB_WIDTH)assert(screen[1]==0xff000000);
        assert(s->video_width==width && s->video_height==height);
        assert(s->video_queue_bytes<=8*1024*1024);
        ++packets;
    }
    assert(packets==12);fclose(f);
}
int main(int argc,char **argv) {
    assert(argc==7);
    LabState s;lab_init(&s,"video-host");
    assert(lab_video_start(&s)==0);
    lab_video_stream_state(1);
    decode_fixture(&s,argv[1],160,96,1,1);
    decode_fixture(&s,argv[2],96,160,0,0);
    decode_fixture(&s,argv[3],1280,720,2,0);
    decode_fixture(&s,argv[6],1920,1080,2,0);
    assert(s.video_errors==0);
    assert(lab_video_configure(0,1280,720)==-1); /* HEVC is rejected */
    assert(lab_video_configure(1,9000,720)==-1); /* absurd client hint */
    assert(lab_video_configure(1,0,0)==-1);
    assert(lab_video_configure(1,160,96)==0);
    unsigned char invalid[4]={1,2,3,4};
    lab_video_submit(invalid,4,0,0);
    lab_video_submit(invalid,5*1024*1024,0,0); /* reject before reading input */
    lab_video_submit(invalid,4,1,0);
    lab_video_poll(&s);assert(s.video_errors==6);
    /* Metadata can describe a larger phone, but encoded frames have a hard budget. */
    assert(lab_video_configure(1,3000,4000)==0);
    FILE *too_big=fopen(argv[4],"rb");assert(too_big);
    uint32_t big_size;assert(fread(&big_size,4,1,too_big)==1 && big_size<1000000);
    unsigned char *big=malloc(big_size);assert(big);
    assert(fread(big,1,big_size,too_big)==big_size);fclose(too_big);
    lab_video_submit(big,(int)big_size,0,0);free(big);
    for(int i=0;i<1000;++i){lab_video_poll(&s);if(s.video_errors>6)break;usleep(5000);}
    assert(s.video_errors>6 && !lab_video_draw(screen+1));
    assert(lab_video_configure(1,160,96)==0);
    lab_video_poll(&s);
    unsigned long before=s.video_decoded,dropped=s.video_dropped;
    FILE *long_gop=fopen(argv[5],"rb");assert(long_gop);
    uint32_t length;unsigned received=0;
    while(fread(&length,4,1,long_gop)==1){
        assert(length>0 && length<1000000);
        unsigned char *bytes=malloc(length);assert(bytes);
        assert(fread(bytes,1,length,long_gop)==length);
        lab_video_submit(bytes,(int)length,0,received*33333);free(bytes);++received;
    }
    fclose(long_gop);assert(received==24);
    wait_frames(&s,before+24);lab_video_poll(&s);
    assert(s.video_dropped==dropped && s.video_queue_bytes<=8*1024*1024);
    assert(s.video_backpressure>0);
    /* A reset must wake a producer that is waiting for queue space. */
    atomic_store(&render_running,1);
    pthread_t renderer;assert(pthread_create(&renderer,NULL,concurrent_renderer,NULL)==0);
    pthread_t producer;assert(pthread_create(&producer,NULL,blocked_producer,NULL)==0);
    usleep(10000);lab_video_stream_state(0);pthread_join(producer,NULL);
    atomic_store(&render_running,0);pthread_join(renderer,NULL);
    assert(!lab_video_draw(screen+1));
    lab_video_reset();assert(!lab_video_draw(screen+1));
    /* Reconnect after overload: cached state from the old session is discarded. */
    decode_fixture(&s,argv[1],160,96,1,1);
    lab_video_poll(&s);unsigned long copies=s.video_copies;
    assert(lab_video_draw(screen+1));lab_video_poll(&s);
    assert(s.video_copies==copies); /* unchanged back buffer is not recopied */
    lab_video_invalidate_output();assert(lab_video_draw(screen+1));lab_video_poll(&s);
    assert(s.video_copies==copies+1);
    lab_video_stream_state(0);assert(!lab_video_draw(screen+1));
    lab_video_stop();assert(!lab_video_draw(screen+1));
    free(first_idr);
    puts("H264 colors, ownership, portrait, letterboxing, reset, overload and reconnect passed");
    return 0;
}
