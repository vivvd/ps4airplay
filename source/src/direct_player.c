/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "direct_player.h"
#include "avplayer_abi.h"
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
extern int lab_airplay_thread_create(pthread_t *,void *(*)(void *),void *);
static struct {
    void *(*init)(AP_Init *);int (*close)(void *);int (*add)(void *,uint32_t,AP_Source *);
    bool (*video)(void *,AP_FrameEx *),(*audio)(void *,AP_Frame *),(*active)(void *);
    int (*pause)(void *),(*resume)(void *),(*stop)(void *),(*seek)(void *,uint64_t);
    uint64_t (*time)(void *);int (*count)(void *),(*stream)(void *,uint32_t,AP_Stream *),(*enable)(void *,uint32_t);
} api;
static struct {
    pthread_mutex_t mutex,image_mutex,mem_mutex;
    pthread_t worker;int started,running,available,active,paused,ready,error,event;
    unsigned commands;char url[8193];double start,seek;int fraction,rate;
    double duration,position;unsigned long video_frames,audio_frames,errors,revision;
    uint32_t *image;unsigned long image_revision;int image_ready;
    struct SwsContext *sws;
    struct {void *ptr;off_t offset;size_t size;} textures[32];size_t texture_bytes;
} dp={.mutex=PTHREAD_MUTEX_INITIALIZER,.image_mutex=PTHREAD_MUTEX_INITIALIZER,.mem_mutex=PTHREAD_MUTEX_INITIALIZER};
enum {PLAY=1,SEEK=2,RATE=4,STOP=8};
static void *heap_alloc(void *obj,uint32_t alignment,uint32_t size) {
    (void)obj;void *p=NULL;if(!size || size>128*1024*1024)return NULL;
    if(alignment<sizeof(void *))alignment=sizeof(void *);
    if((alignment&(alignment-1)) || alignment>2*1024*1024)return NULL;
    return posix_memalign(&p,alignment,size)==0?p:NULL;
}
static void heap_free(void *obj,void *p){(void)obj;free(p);}
static void *texture_alloc(void *obj,uint32_t alignment,uint32_t size) {
    (void)obj;if(!size || size>64*1024*1024)return NULL;
    size_t n=((size_t)size+0x3fff)&~(size_t)0x3fff;
    if(alignment<0x4000)alignment=0x4000;
    if(alignment>0x200000 || (alignment&(alignment-1)))return NULL;
    pthread_mutex_lock(&dp.mem_mutex);void *ptr=NULL;
    for(unsigned i=0;i<32;++i)if(!dp.textures[i].ptr && dp.texture_bytes+n<=128*1024*1024){
        off_t offset=0;
        if(sceKernelAllocateDirectMemory(0,sceKernelGetDirectMemorySize(),n,alignment,3,&offset)<0)break;
        if(sceKernelMapDirectMemory(&ptr,n,0x33,0,offset,alignment)<0){sceKernelReleaseDirectMemory(offset,n);ptr=NULL;break;}
        dp.textures[i].ptr=ptr;dp.textures[i].offset=offset;dp.textures[i].size=n;dp.texture_bytes+=n;break;
    }
    pthread_mutex_unlock(&dp.mem_mutex);return ptr;
}
static void texture_free(void *obj,void *ptr) {
    (void)obj;if(!ptr)return;
    pthread_mutex_lock(&dp.mem_mutex);
    for(unsigned i=0;i<32;++i)if(dp.textures[i].ptr==ptr){
        sceKernelMunmap(ptr,dp.textures[i].size);sceKernelReleaseDirectMemory(dp.textures[i].offset,dp.textures[i].size);
        dp.texture_bytes-=dp.textures[i].size;memset(&dp.textures[i],0,sizeof(dp.textures[i]));break;
    }
    pthread_mutex_unlock(&dp.mem_mutex);
}
static void player_event(void *obj,uint32_t event,int32_t source,void *data) {
    (void)obj;(void)source;(void)data;
    pthread_mutex_lock(&dp.mutex);dp.event=(int)event;
    if(event==0x30 || event==0x40){dp.error=-2003;++dp.errors;dp.commands=STOP;}
    pthread_mutex_unlock(&dp.mutex);
}
static int resolve(void) {
    if(sceSysmoduleLoadModule(ORBIS_SYSMODULE_AV_PLAYER)<0){/* It may already be loaded. */}
    OrbisKernelModule modules[256];size_t count=0;
    if(sceKernelGetModuleList(modules,sizeof(modules),&count)<0)return -1;
    if(count>256)count=256;
#define SYM(member,name) do {void *p=NULL;for(size_t i=0;i<count && !p;++i)sceKernelDlsym(modules[i],name,&p);if(!p)return -1;memcpy(&api.member,&p,sizeof(p));}while(0)
    SYM(init,"sceAvPlayerInit");SYM(close,"sceAvPlayerClose");SYM(add,"sceAvPlayerAddSourceEx");
    SYM(video,"sceAvPlayerGetVideoDataEx");SYM(audio,"sceAvPlayerGetAudioData");SYM(active,"sceAvPlayerIsActive");
    SYM(pause,"sceAvPlayerPause");SYM(resume,"sceAvPlayerResume");SYM(stop,"sceAvPlayerStop");SYM(seek,"sceAvPlayerJumpToTime");
    SYM(time,"sceAvPlayerCurrentTime");SYM(count,"sceAvPlayerStreamCount");SYM(stream,"sceAvPlayerGetStreamInfo");SYM(enable,"sceAvPlayerEnableStream");
#undef SYM
    return 0;
}
static int convert_video(AP_FrameEx *f) {
    AP_VideoEx *v=&f->details.video;
    unsigned w=v->width,h=v->height,pitch=v->pitch?v->pitch:w;
    if(!f->data || !w || !h || w>3840 || h>2160 || pitch<w || pitch>8192 ||
       (v->luma && v->luma!=8) || (v->chroma && v->chroma!=8) || v->left+v->right>=w || v->top+v->bottom>=h)return -1;
    unsigned cw=w-v->left-v->right,ch=h-v->top-v->bottom;
    double aspect=isfinite(v->aspect) && v->aspect>0.1 && v->aspect<10?v->aspect:cw/(double)ch;
    int ow=LAB_WIDTH,oh=(int)(ow/aspect+0.5);if(oh>LAB_HEIGHT){oh=LAB_HEIGHT;ow=(int)(oh*aspect+0.5);}
    if(ow<1 || oh<1)return -1;
    dp.sws=sws_getCachedContext(dp.sws,(int)cw,(int)ch,AV_PIX_FMT_NV12,ow,oh,AV_PIX_FMT_BGRA,SWS_BILINEAR,NULL,NULL,NULL);
    if(!dp.sws)return -1;
    const int *coeff=sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(dp.sws,coeff,v->full_range,coeff,1,0,1<<16,1<<16);
    const uint8_t *in[4]={(uint8_t *)f->data+v->top*pitch+v->left,
        (uint8_t *)f->data+pitch*h+(v->top/2)*pitch+(v->left&~1u),NULL,NULL};
    int stride[4]={(int)pitch,(int)pitch,0,0};
    pthread_mutex_lock(&dp.image_mutex);
    if(!dp.image)dp.image=malloc(LAB_WIDTH*LAB_HEIGHT*4);
    if(!dp.image){pthread_mutex_unlock(&dp.image_mutex);return -1;}
    memset(dp.image,0,LAB_WIDTH*LAB_HEIGHT*4);
    uint8_t *out[4]={(uint8_t *)(dp.image+((LAB_HEIGHT-oh)/2)*LAB_WIDTH+(LAB_WIDTH-ow)/2),NULL,NULL,NULL};
    int outs[4]={LAB_WIDTH*4,0,0,0};
    int rc=sws_scale(dp.sws,in,stride,0,(int)ch,out,outs);
    if(rc>0){dp.image_ready=1;++dp.image_revision;}
    pthread_mutex_unlock(&dp.image_mutex);return rc>0?0:-1;
}
static void set_error(int error){pthread_mutex_lock(&dp.mutex);dp.error=error;++dp.errors;pthread_mutex_unlock(&dp.mutex);}
static void clear_image(void){pthread_mutex_lock(&dp.image_mutex);dp.image_ready=0;++dp.image_revision;pthread_mutex_unlock(&dp.image_mutex);}
static void *worker(void *unused) {
    (void)unused;void *player=NULL;SwrContext *swr=NULL;unsigned sample_rate=0,channels=0;int pending_start=0;double initial=0;int fractional=0,scanned=0;
    uint64_t opened_at=0;int seen=0,playing=0;char url[8193]={0};
    for(;;){
        pthread_mutex_lock(&dp.mutex);int running=dp.running;unsigned commands=dp.commands;dp.commands=0;
        char next_url[8193];if(commands&PLAY)memcpy(next_url,dp.url,sizeof(next_url));
        double start=dp.start,seek=dp.seek;int fraction=dp.fraction,rate=dp.rate;
        pthread_mutex_unlock(&dp.mutex);
        if(!running || (commands&(PLAY|STOP))){
            if(player){api.stop(player);api.close(player);player=NULL;}swr_free(&swr);sample_rate=channels=0;
            lab_audio_direct_mode(0);clear_image();seen=playing=0;
            pthread_mutex_lock(&dp.mutex);dp.active=dp.ready=dp.paused=0;dp.position=dp.duration=0;pthread_mutex_unlock(&dp.mutex);
            if(!running)break;
        }
        if(commands&PLAY){
            memcpy(url,next_url,sizeof(url));
            pthread_mutex_lock(&dp.mutex);dp.error=0;pthread_mutex_unlock(&dp.mutex);
            lab_session_trace("direct video","native init");
            AP_Init init={0};init.memory=(AP_Memory){NULL,heap_alloc,heap_free,texture_alloc,texture_free};
            init.event.callback=player_event;init.priority=700;init.buffers=3;init.auto_start=true;init.language="en";
            player=api.init(&init);
            if(!player){set_error(-2001);continue;}
            AP_Source source={0};source.uri.name=url;source.uri.length=(uint32_t)strlen(url);
            source.type=strstr(url,".m3u8")?8:0;
            lab_audio_direct_mode(1);lab_session_trace("direct video","native source start");
            int rc=api.add(player,0,&source);
            if(rc<0){set_error(rc);api.close(player);player=NULL;lab_audio_direct_mode(0);continue;}
            pending_start=start>0;initial=start;fractional=fraction;scanned=0;opened_at=sceKernelGetProcessTime();
            pthread_mutex_lock(&dp.mutex);dp.active=1;++dp.revision;pthread_mutex_unlock(&dp.mutex);
        }
        if(player){
            if(commands&SEEK){lab_audio_direct_mode(1);if(api.seek(player,(uint64_t)(seek*1000))<0)set_error(-2004);swr_free(&swr);sample_rate=channels=0;clear_image();}
            if(commands&RATE){int rc=rate?api.resume(player):api.pause(player);if(rc<0)set_error(rc);else{playing=rate;lab_audio_direct_mode(1);pthread_mutex_lock(&dp.mutex);dp.paused=!rate;pthread_mutex_unlock(&dp.mutex);}}
            int active=api.active(player);if(active)seen=1;
            if(active && !scanned){
                int count=api.count(player);if(count>0 && count<64){
                    int video_enabled=0,audio_enabled=0;double duration=0;
                    for(int i=0;i<count;++i){AP_Stream stream={0};if(api.stream(player,(uint32_t)i,&stream)<0)continue;
                        if((stream.type==0 && !video_enabled) || (stream.type==1 && !audio_enabled)){
                            int rc=api.enable(player,(uint32_t)i);if(rc>=0){if(stream.type==0)video_enabled=1;else audio_enabled=1;}
                        }
                        if(stream.duration/1000.0>duration)duration=stream.duration/1000.0;
                    }
                    pthread_mutex_lock(&dp.mutex);dp.duration=duration;dp.ready=1;pthread_mutex_unlock(&dp.mutex);scanned=1;playing=1;
                }
            }
            if(pending_start && scanned){
                pthread_mutex_lock(&dp.mutex);double duration=dp.duration;pthread_mutex_unlock(&dp.mutex);
                if(!fractional || duration>0){double target=fractional?initial*duration:initial;int rc=api.seek(player,(uint64_t)(target*1000));if(rc<0)set_error(rc);pending_start=0;}
            }
            /* Video is paced by AvPlayer. Audio pulls are bounded by FIFO space;
             * never pull the next native buffer until the previous one is copied. */
            if(active && playing && lab_audio_direct_space()>=8192){
                AP_Frame f={0};if(api.audio(player,&f)){
                    AP_Audio *a=&f.details.audio;
                    if(f.data && a->rate>=8000 && a->rate<=192000 && a->channels>=1 && a->channels<=8 && a->size<=32768 && a->size%(a->channels*2)==0){
                        if(sample_rate!=a->rate || channels!=a->channels){
                            swr_free(&swr);AVChannelLayout in={0},out=AV_CHANNEL_LAYOUT_STEREO;av_channel_layout_default(&in,a->channels);
                            int rc=swr_alloc_set_opts2(&swr,&out,AV_SAMPLE_FMT_S16,48000,&in,AV_SAMPLE_FMT_S16,(int)a->rate,0,NULL);av_channel_layout_uninit(&in);
                            if(rc<0 || swr_init(swr)<0){swr_free(&swr);set_error(-2005);}sample_rate=a->rate;channels=a->channels;
                        }
                        if(swr){int16_t pcm[32768];uint8_t *out=(uint8_t *)pcm;const uint8_t *in=f.data;
                            int n=swr_convert(swr,&out,16384,&in,(int)(a->size/(a->channels*2)));
                            if(n>0){lab_audio_direct_pcm(pcm,(unsigned)n);pthread_mutex_lock(&dp.mutex);dp.audio_frames+=(unsigned)n;pthread_mutex_unlock(&dp.mutex);}
                        }
                    }else set_error(-2006);
                }
            }
            if(active && playing){AP_FrameEx frame={0};if(api.video(player,&frame)){
                int rc=convert_video(&frame);pthread_mutex_lock(&dp.mutex);if(rc==0)++dp.video_frames;else ++dp.errors;pthread_mutex_unlock(&dp.mutex);
            }}
            double position=api.time(player)/1000.0;
            pthread_mutex_lock(&dp.mutex);dp.position=position;pthread_mutex_unlock(&dp.mutex);
            if((seen && !active) || (!seen && sceKernelGetProcessTime()-opened_at>30000000)){
                if(!seen)set_error(-2002);
                api.close(player);player=NULL;lab_audio_direct_mode(0);clear_image();
                pthread_mutex_lock(&dp.mutex);dp.active=dp.ready=0;pthread_mutex_unlock(&dp.mutex);
            }
        }
        sceKernelUsleep(2000);
    }
    sws_freeContext(dp.sws);dp.sws=NULL;return NULL;
}
int direct_start(void) {
    if(dp.started)return dp.available;
    lab_session_trace("direct video","module probe");dp.available=resolve()==0;
    if(!dp.available)return 0;
    dp.running=1;if(lab_airplay_thread_create(&dp.worker,worker,NULL)){dp.running=dp.available=0;return 0;}
    dp.started=1;return 1;
}
static void play(const char *url,float start,int fraction) {
    if(!dp.available || !url || !isfinite(start) || start<0 || (fraction && start>1))return;
    size_t n=strnlen(url,8193);if(n>8192 || (strncmp(url,"http://",7) && strncmp(url,"https://",8)))return;
    for(size_t i=0;i<n;++i)if((unsigned char)url[i]<32)return;
    pthread_mutex_lock(&dp.mutex);memcpy(dp.url,url,n+1);dp.start=start;dp.fraction=fraction;dp.commands=PLAY;pthread_mutex_unlock(&dp.mutex);
}
void direct_play(void *cls,const char *url,float seconds){(void)cls;play(url,seconds,0);}
void direct_play_fraction(void *cls,const char *url,float fraction){(void)cls;play(url,fraction,1);}
void direct_seek(void *cls,float seconds){(void)cls;if(!isfinite(seconds) || seconds<0 || seconds>7*24*3600)return;pthread_mutex_lock(&dp.mutex);dp.seek=seconds;dp.commands|=SEEK;pthread_mutex_unlock(&dp.mutex);}
void direct_rate(void *cls,float rate){(void)cls;if(!isfinite(rate))return;pthread_mutex_lock(&dp.mutex);dp.rate=rate>0;dp.commands|=RATE;pthread_mutex_unlock(&dp.mutex);}
void direct_stop(void *cls){(void)cls;pthread_mutex_lock(&dp.mutex);dp.commands=STOP;pthread_mutex_unlock(&dp.mutex);}
void direct_info(void *cls,playback_info_t *info){(void)cls;pthread_mutex_lock(&dp.mutex);info->duration=dp.duration;info->position=dp.position;info->rate=dp.active && !dp.paused?1:0;info->ready_to_play=dp.ready;info->playback_buffer_empty=!dp.ready;info->playback_buffer_full=false;info->playback_likely_to_keep_up=dp.ready;info->seek_start=0;info->seek_duration=dp.duration;pthread_mutex_unlock(&dp.mutex);}
float direct_remove(void *cls){playback_info_t info={0};direct_info(cls,&info);direct_stop(cls);return (float)info.position;}
static struct {uint32_t *ptr;unsigned long revision;} direct_targets[2];
void direct_invalidate(void){direct_targets[0].revision=direct_targets[1].revision=0;}
int direct_draw(uint32_t *pixels){
    pthread_mutex_lock(&dp.image_mutex);int ready=dp.image_ready;
    if(ready){
        int i=direct_targets[0].ptr==pixels?0:direct_targets[1].ptr==pixels?1:direct_targets[0].ptr?1:0;
        if(direct_targets[i].ptr!=pixels || direct_targets[i].revision!=dp.image_revision){memcpy(pixels,dp.image,LAB_WIDTH*LAB_HEIGHT*4);direct_targets[i].ptr=pixels;direct_targets[i].revision=dp.image_revision;}
    }else{direct_targets[0].revision=direct_targets[1].revision=0;}
    pthread_mutex_unlock(&dp.image_mutex);return ready;
}

void direct_poll(LabState *s){pthread_mutex_lock(&dp.mutex);s->direct_available=dp.available;s->direct_active=dp.active;s->direct_error=dp.error;s->direct_event=dp.event;s->direct_video_frames=dp.video_frames;s->direct_audio_frames=dp.audio_frames;s->direct_errors=dp.errors;s->direct_position=dp.position;s->direct_duration=dp.duration;pthread_mutex_unlock(&dp.mutex);}
void direct_shutdown(void){if(!dp.started)return;pthread_mutex_lock(&dp.mutex);dp.running=0;pthread_mutex_unlock(&dp.mutex);pthread_join(dp.worker,NULL);free(dp.image);dp.image=NULL;dp.started=0;}
