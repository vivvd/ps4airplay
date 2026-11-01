/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "direct_player.h"
#include "avplayer_abi.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
static atomic_int active,paused,closed,seeks,video_count,audio_count;
static atomic_ullong seek_ms;
static AP_Init configuration;static const char *source_url;static char retained_url[8193];
uint64_t sceKernelGetProcessTime(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
void sceKernelUsleep(unsigned n){usleep(n);}
size_t sceKernelGetDirectMemorySize(void){return 256*1024*1024;}
int sceKernelAllocateDirectMemory(off_t a,off_t b,size_t n,size_t align,int type,off_t *out){(void)a;(void)b;(void)n;(void)align;assert(type==3);*out=0;return 0;}
int sceKernelMapDirectMemory(void **p,size_t n,int prot,int flags,off_t offset,size_t align){(void)flags;(void)offset;assert(prot==0x33);return posix_memalign(p,align,n);}
int sceKernelMunmap(void *p,size_t n){(void)n;free(p);return 0;}
int sceKernelReleaseDirectMemory(off_t p,size_t n){(void)p;(void)n;return 0;}
int sceSysmoduleLoadModule(int id){assert(id==0xa5);return 0;}
int sceKernelGetModuleList(int *modules,size_t size,size_t *count){(void)size;*modules=1;*count=1;return 0;}
int lab_airplay_thread_create(pthread_t *t,void *(*f)(void *),void *a){return pthread_create(t,NULL,f,a);}
void lab_session_trace(const char *a,const char *b){(void)a;(void)b;}
void lab_audio_direct_mode(int mode){(void)mode;}
unsigned lab_audio_direct_space(void){return 8192;}
void lab_audio_direct_pcm(const int16_t *p,unsigned n){assert(p && n>0 && n<16384);atomic_fetch_add(&audio_count,1);}
static void *mock_init(AP_Init *init){configuration=*init;assert(init->auto_start && init->buffers==3);void *p=init->memory.texture(NULL,16,4096);assert(p);init->memory.free_texture(NULL,p);return &configuration;}
static int mock_close(void *h){assert(h);assert(!source_url || !strcmp(source_url,retained_url));atomic_fetch_add(&closed,1);active=0;return 0;}
static int mock_add(void *h,uint32_t type,AP_Source *s){assert(h && type==0);assert(s->uri.length==strlen(s->uri.name));source_url=s->uri.name;snprintf(retained_url,sizeof(retained_url),"%s",source_url);if(strstr(source_url,"fail"))return -777;active=1;paused=0;video_count=0;return 0;}
static bool mock_video(void *h,AP_FrameEx *frame){assert(h);assert(source_url && !strncmp(source_url,"https://",8));if(paused || atomic_fetch_add(&video_count,1)>1)return false;static unsigned char nv12[12]={80,100,120,150,80,100,120,150,128,128,128,128};frame->data=nv12;frame->details.video=(AP_VideoEx){.width=4,.height=2,.aspect=2,.pitch=4,.luma=8,.chroma=8};return true;}
static bool mock_audio(void *h,AP_Frame *f){assert(h);if(paused)return false;static int16_t pcm[2048];f->data=pcm;f->details.audio=(AP_Audio){.channels=2,.rate=44100,.size=sizeof(pcm)};return true;}
static bool mock_active(void *h){assert(h);return active;}
static int mock_pause(void *h){assert(h);paused=1;return 0;}
static int mock_resume(void *h){assert(h);paused=0;return 0;}
static int mock_stop(void *h){assert(h);active=0;return 0;}
static int mock_seek(void *h,uint64_t ms){assert(h);seek_ms=ms;atomic_fetch_add(&seeks,1);return 0;}
static uint64_t mock_time(void *h){assert(h);return seek_ms;}
static int mock_count(void *h){assert(h);return 2;}
static int mock_stream(void *h,uint32_t i,AP_Stream *s){assert(h && i<2);s->type=i;s->duration=120000;return 0;}
static int mock_enable(void *h,uint32_t i){assert(h && i<2);return 0;}
int sceKernelDlsym(int module,const char *name,void **p){(void)module;
#define MAP(s,f) if(!strcmp(name,s)){*p=(void *)f;return 0;}
MAP("sceAvPlayerInit",mock_init)MAP("sceAvPlayerClose",mock_close)MAP("sceAvPlayerAddSourceEx",mock_add)
MAP("sceAvPlayerGetVideoDataEx",mock_video)MAP("sceAvPlayerGetAudioData",mock_audio)MAP("sceAvPlayerIsActive",mock_active)
MAP("sceAvPlayerPause",mock_pause)MAP("sceAvPlayerResume",mock_resume)MAP("sceAvPlayerStop",mock_stop)MAP("sceAvPlayerJumpToTime",mock_seek)
MAP("sceAvPlayerCurrentTime",mock_time)MAP("sceAvPlayerStreamCount",mock_count)MAP("sceAvPlayerGetStreamInfo",mock_stream)MAP("sceAvPlayerEnableStream",mock_enable)
#undef MAP
return -1;}
#define WAIT(test) do{int ok=0;for(int i=0;i<2000;++i){if(test){ok=1;break;}usleep(1000);}assert(ok);}while(0)
int main(void){
    assert(direct_start()==1);direct_play(NULL,"file:///rejected",0);usleep(10000);assert(!active);
    direct_play_fraction(NULL,"https://test.invalid/movie.mp4",0.25f);WAIT(seeks==1);assert(seek_ms==30000);WAIT(audio_count>0);WAIT(video_count>0);
    LabState state={0};for(int i=0;i<2000;++i){direct_poll(&state);if(state.direct_video_frames>0)break;usleep(1000);}assert(state.direct_video_frames>0 && state.direct_audio_frames>0 && state.direct_errors==0);
    uint32_t *pixels=malloc(LAB_WIDTH*LAB_HEIGHT*4);assert(pixels);assert(direct_draw(pixels));assert(pixels[LAB_WIDTH*500+900]!=0);free(pixels);
    direct_rate(NULL,0);WAIT(paused==1);playback_info_t info={0};direct_info(NULL,&info);assert(info.rate==0 && info.duration==120);
    direct_seek(NULL,42);WAIT(seek_ms==42000);direct_rate(NULL,1);WAIT(paused==0);
    direct_play(NULL,"https://test.invalid/replacement.mp4",0);WAIT(closed>=1);usleep(10000);assert(active);
    direct_stop(NULL);WAIT(closed>=2);WAIT(!active);direct_play(NULL,"https://test.invalid/fail.mp4",0);WAIT(closed>=3);
    direct_poll(&state);assert(state.direct_error==-777 && !state.direct_active);direct_shutdown();
    puts("Direct player: ABI, module probe, allocators, URL lifetime, NV12/PCM, fractional start, seek, pause/resume, stop and source error passed");return 0;
}
