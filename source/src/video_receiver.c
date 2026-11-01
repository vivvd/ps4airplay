/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "video_receiver.h"
#include "video_profile.h"
#include "video_pacer.h"
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libavutil/cpu.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#ifdef LAB_PS4
#include <orbis/libkernel.h>
#endif

#define VIDEO_QUEUE 6
#define VIDEO_BYTES (8u*1024*1024)
#define VIDEO_PACKET_MAX (4*1024*1024)
#define VIDEO_MAX_PIXELS (1920*1080)
#define PARAMETER_MAX 32768
extern int lab_airplay_thread_create(pthread_t *,void *(*)(void *),void *);
static struct {
    pthread_mutex_t mutex;
    pthread_mutex_t image_mutex;
    pthread_cond_t wake;
    pthread_t worker;
    int running,started,configured,need_idr,have_frame,stream_active,cpu_flags;
    unsigned generation,head,count;
    size_t bytes;
    AVPacket *queue[VIDEO_QUEUE];
    unsigned epochs[VIDEO_QUEUE];
    unsigned char sps[PARAMETER_MAX],pps[PARAMETER_MAX];
    size_t sps_size,pps_size;
    uint32_t *latest,*scratch;
    unsigned width,height;
    unsigned long packets,decoded,errors,dropped,presented,last_presented;
    unsigned long converted,render_skipped,backpressure,decode_us,convert_us,copies,latest_serial;
    unsigned long copy_us,pacing_us,pacer_resets;
    unsigned frame_interval_us,buffer_us;
    uint64_t last_packet_us,last_frame_us;
    uint32_t *destinations[2];
    unsigned long destination_serials[2];
    unsigned next_destination;
} v={.mutex=PTHREAD_MUTEX_INITIALIZER,.image_mutex=PTHREAD_MUTEX_INITIALIZER,.wake=PTHREAD_COND_INITIALIZER};
typedef struct {
    struct SwsContext *context;
    int width,height,format,out_width,out_height,range,clear;
    enum AVColorSpace colorspace;
} VideoConverter;

static uint64_t now_us(void) {
#ifdef LAB_PS4
    return sceKernelGetProcessTime();
#else
    struct timespec value;clock_gettime(CLOCK_MONOTONIC,&value);
    return (uint64_t)value.tv_sec*1000000+(uint64_t)value.tv_nsec/1000;
#endif
}

static void clear_queue(void) {
    while(v.count){av_packet_free(&v.queue[v.head]);v.head=(v.head+1)%VIDEO_QUEUE;--v.count;++v.dropped;}
    v.bytes=0;
}
static size_t start_code(const unsigned char *data,size_t size,size_t from,size_t *prefix) {
    for(size_t i=from;i+3<=size;++i){
        if(data[i] || data[i+1])continue;
        if(data[i+2]==1){*prefix=3;return i;}
        if(i+4<=size && data[i+2]==0 && data[i+3]==1){*prefix=4;return i;}
    }
    return size;
}
/* Called under the queue lock. Cache SPS/PPS so dropping a backed-up GOP can
 * resume at an IDR without keeping arbitrary old reference frames or memory. */
static int inspect(const unsigned char *data,size_t size,int *idr,int *picture) {
    size_t prefix=0,pos=start_code(data,size,0,&prefix);
    if(pos==size)return -1;
    while(pos<size){
        size_t body=pos+prefix,next_prefix=0;
        size_t next=start_code(data,size,body,&next_prefix);
        if(body>=next)return -1;
        unsigned type=data[body]&31;
        if(type==5)*idr=1;
        if(type>=1 && type<=5)*picture=1;
        if(type==7 || type==8){
            size_t length=next-body;
            if(length>PARAMETER_MAX-4)return -1;
            unsigned char *out=type==7?v.sps:v.pps;
            out[0]=out[1]=out[2]=0;out[3]=1;
            memcpy(out+4,data+body,length);
            if(type==7)v.sps_size=length+4;else v.pps_size=length+4;
        }
        pos=next;prefix=next_prefix;
    }
    return 0;
}
int lab_video_configure(int h264,unsigned width,unsigned height) {
    lab_video_reset();
    pthread_mutex_lock(&v.mutex);
    /* These are client viewport/source hints, not decoded SPS dimensions.
     * Do not reject a downscaled stream because the phone has a large screen.
     * The decoder's max_pixels and publish() enforce the actual frame budget. */
    int okay=v.running && h264 && width && height && width<=8192 && height<=8192;
    if(okay){v.configured=1;v.width=width;v.height=height;}else ++v.errors;
    pthread_mutex_unlock(&v.mutex);
    char detail[100];snprintf(detail,sizeof(detail),"h264=%d size=%ux%u accepted=%d",h264,width,height,okay);
    lab_session_trace("video configure",detail);
    return okay?0:-1;
}
void lab_video_submit(const unsigned char *data,int size,int hevc,uint64_t pts) {
    if(!data || size<=0)return;
    pthread_mutex_lock(&v.mutex);
    if(!v.running || !v.configured){pthread_mutex_unlock(&v.mutex);return;}
    ++v.packets;
    v.last_packet_us=now_us();
    if(hevc || size>VIDEO_PACKET_MAX){++v.errors;pthread_mutex_unlock(&v.mutex);return;}
    int idr=0,picture=0;
    if(inspect(data,(size_t)size,&idr,&picture)<0){++v.errors;pthread_mutex_unlock(&v.mutex);return;}
    if(!picture){pthread_mutex_unlock(&v.mutex);return;}
    if(v.need_idr && !idr){++v.dropped;pthread_mutex_unlock(&v.mutex);return;}
    unsigned epoch=v.generation;
    /* Preserve reference pictures. A full queue applies bounded TCP
     * backpressure, rather than discarding a GOP and freezing until an IDR. */
    int waited=0;
    while(v.running && v.configured && epoch==v.generation &&
          (v.count==VIDEO_QUEUE || v.bytes+(size_t)size+v.sps_size+v.pps_size>VIDEO_BYTES)){
        if(!waited){++v.backpressure;waited=1;}
        pthread_cond_wait(&v.wake,&v.mutex);
    }
    if(!v.running || !v.configured || epoch!=v.generation){++v.dropped;pthread_mutex_unlock(&v.mutex);return;}
    size_t prefix=idr?v.sps_size+v.pps_size:0;
    AVPacket *packet=av_packet_alloc();
    if(!packet || av_new_packet(packet,(int)(prefix+(size_t)size))<0){
        av_packet_free(&packet);++v.errors;pthread_mutex_unlock(&v.mutex);return;
    }
    if(prefix){memcpy(packet->data,v.sps,v.sps_size);memcpy(packet->data+v.sps_size,v.pps,v.pps_size);}
    memcpy(packet->data+prefix,data,(size_t)size);
    packet->pts=(int64_t)pts;
    unsigned slot=(v.head+v.count)%VIDEO_QUEUE;
    v.queue[slot]=packet;v.epochs[slot]=v.generation;++v.count;v.bytes+=(size_t)packet->size;
    v.need_idr=0;
    pthread_cond_broadcast(&v.wake);pthread_mutex_unlock(&v.mutex);
}
static void record_error(void) {pthread_mutex_lock(&v.mutex);++v.errors;pthread_mutex_unlock(&v.mutex);}
static int publish(AVFrame *frame,VideoConverter *scale,unsigned epoch) {
    int w=frame->width,h=frame->height;
    if(w<=0 || h<=0 || w>1920 || h>1920 || (int64_t)w*h>VIDEO_MAX_PIXELS)return -1;
    int out_w=LAB_WIDTH,out_h=(int)((int64_t)h*LAB_WIDTH/w);
    if(out_h>LAB_HEIGHT){out_h=LAB_HEIGHT;out_w=(int)((int64_t)w*LAB_HEIGHT/h);}
    if(out_w<1 || out_h<1)return -1;
    int range=frame->color_range==AVCOL_RANGE_JPEG;
    int changed=!scale->context || scale->width!=w || scale->height!=h || scale->format!=frame->format ||
                scale->out_width!=out_w || scale->out_height!=out_h;
    scale->context=sws_getCachedContext(scale->context,w,h,frame->format,out_w,out_h,AV_PIX_FMT_BGRA,
                                       SWS_BILINEAR,NULL,NULL,NULL);
    if(!scale->context)return -1;
    if(changed || scale->colorspace!=frame->colorspace || scale->range!=range){
        const int *colors=sws_getCoefficients(frame->colorspace==AVCOL_SPC_BT709?SWS_CS_ITU709:SWS_CS_DEFAULT);
        if(sws_setColorspaceDetails(scale->context,colors,range,colors,1,0,1<<16,1<<16)<0)return -1;
    }
    if(scale->out_width!=out_w || scale->out_height!=out_h)scale->clear=2;
    scale->width=w;scale->height=h;scale->format=frame->format;
    scale->out_width=out_w;scale->out_height=out_h;scale->colorspace=frame->colorspace;scale->range=range;
    /* Each RGB buffer's black borders survive until the geometry changes. */
    if(scale->clear){for(size_t i=0;i<LAB_WIDTH*LAB_HEIGHT;++i)v.scratch[i]=0xff000000;--scale->clear;}
    int left=(LAB_WIDTH-out_w)/2,top=(LAB_HEIGHT-out_h)/2;
    uint8_t *planes[4]={(uint8_t *)(v.scratch+top*LAB_WIDTH+left),NULL,NULL,NULL};
    int lines[4]={LAB_WIDTH*4,0,0,0};
    if(sws_scale(scale->context,(const uint8_t *const *)frame->data,frame->linesize,0,h,planes,lines)!=out_h)return -1;
    /* Lock order: image -> state. A slow VideoOut copy must not hold the queue lock. */
    pthread_mutex_lock(&v.image_mutex);
    pthread_mutex_lock(&v.mutex);
    if(epoch==v.generation && v.configured && v.running){
        uint32_t *previous=v.latest;v.latest=v.scratch;v.scratch=previous;
        v.have_frame=1;v.width=(unsigned)w;v.height=(unsigned)h;
        if(!v.converted)lab_session_trace("video decode","first BGRA frame ready");
        ++v.converted;++v.latest_serial;v.last_frame_us=now_us();
    }
    pthread_mutex_unlock(&v.mutex);
    pthread_mutex_unlock(&v.image_mutex);
    return 0;
}
static int wait_for_presentation(unsigned epoch,uint64_t due) {
    for(;;){
        pthread_mutex_lock(&v.mutex);
        int valid=v.running && v.configured && epoch==v.generation;
        pthread_mutex_unlock(&v.mutex);
        if(!valid)return 0;
        uint64_t now=now_us();
        if(now>=due)return 1;
        unsigned delay=(unsigned)(due-now>2000?2000:due-now);
#ifdef LAB_PS4
        sceKernelUsleep(delay);
#else
        usleep(delay);
#endif
    }
}
static void *video_worker(void *unused) {
    (void)unused;
    AVCodecContext *decoder=NULL;
    AVFrame *frame=av_frame_alloc();
    VideoConverter scale={0};
    LabVideoPacer pacer={0};
    unsigned current=0;
    if(!frame){
        pthread_mutex_lock(&v.mutex);++v.errors;v.running=0;
        pthread_cond_broadcast(&v.wake);pthread_mutex_unlock(&v.mutex);return NULL;
    }
    for(;;){
        pthread_mutex_lock(&v.mutex);
        while(v.running && !v.count && current==v.generation)pthread_cond_wait(&v.wake,&v.mutex);
        int running=v.running;
        unsigned epoch=v.generation;
        AVPacket *packet=NULL;
        if(v.count){
            packet=v.queue[v.head];epoch=v.epochs[v.head];v.queue[v.head]=NULL;
            v.head=(v.head+1)%VIDEO_QUEUE;--v.count;v.bytes-=(size_t)packet->size;
            pthread_cond_broadcast(&v.wake);
        }
        pthread_mutex_unlock(&v.mutex);
        if(!running){av_packet_free(&packet);break;}
        if(current!=epoch){
            avcodec_free_context(&decoder);sws_freeContext(scale.context);
            memset(&scale,0,sizeof(scale));memset(&pacer,0,sizeof(pacer));current=epoch;
        }
        if(!packet)continue;
        if(!decoder){
            lab_session_trace("video decoder","H264 open begin");
            const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_H264);
            decoder=codec?avcodec_alloc_context3(codec):NULL;
            if(decoder){
                decoder->thread_count=1;decoder->thread_type=0;
                decoder->max_pixels=VIDEO_MAX_PIXELS;decoder->flags|=AV_CODEC_FLAG_LOW_DELAY;
                decoder->pkt_timebase=(AVRational){1,1000000};
                if(avcodec_open2(decoder,codec,NULL)<0)avcodec_free_context(&decoder);
            }
            lab_session_trace("video decoder",decoder?"H264 open complete":"H264 open failed");
        }
        if(!decoder){record_error();av_packet_free(&packet);continue;}
        uint64_t decode_start=now_us();
        int64_t packet_pts=packet->pts;
        int rc=avcodec_send_packet(decoder,packet);
        unsigned long decode_time=(unsigned long)(now_us()-decode_start);
        pthread_mutex_lock(&v.mutex);v.decode_us+=decode_time;pthread_mutex_unlock(&v.mutex);
        av_packet_free(&packet);
        if(rc<0){record_error();continue;}
        for(;;){
            decode_start=now_us();rc=avcodec_receive_frame(decoder,frame);
            decode_time=(unsigned long)(now_us()-decode_start);
            pthread_mutex_lock(&v.mutex);
            v.decode_us+=decode_time;
            if(rc>=0)++v.decoded;
            unsigned backlog=v.count;
            pthread_mutex_unlock(&v.mutex);
            if(rc<0)break;
            uint64_t now=now_us();
            int64_t pts=frame->pts==AV_NOPTS_VALUE?packet_pts:frame->pts;
            unsigned previous_resets=pacer.resets;
            uint64_t due=lab_video_pacer_due(&pacer,pts,now,v.buffer_us);
            int skip=backlog>0 && now>due && now-due>v.frame_interval_us;
            pthread_mutex_lock(&v.mutex);
            v.pacer_resets+=pacer.resets-previous_resets;
            if(skip)++v.render_skipped;
            pthread_mutex_unlock(&v.mutex);
            /* Queue occupancy alone does not mean a picture is late. */
            if(!skip){
                uint64_t wait_start=now_us();
                int valid=wait_for_presentation(epoch,due);
                pthread_mutex_lock(&v.mutex);v.pacing_us+=(unsigned long)(now_us()-wait_start);pthread_mutex_unlock(&v.mutex);
                uint64_t convert_start=now_us();
                if(valid && publish(frame,&scale,epoch)<0)record_error();
                pthread_mutex_lock(&v.mutex);
                v.convert_us+=(unsigned long)(now_us()-convert_start);
                pthread_mutex_unlock(&v.mutex);
            }
            av_frame_unref(frame);
        }
        if(rc!=AVERROR(EAGAIN) && rc!=AVERROR_EOF)record_error();
    }
    avcodec_free_context(&decoder);av_frame_free(&frame);sws_freeContext(scale.context);
    return NULL;
}
int lab_video_start(LabState *s) {
    if(v.started)return 0;
    size_t size=LAB_WIDTH*LAB_HEIGHT*sizeof(uint32_t);
    v.latest=av_mallocz(size);v.scratch=av_mallocz(size);
    if(!v.latest || !v.scratch){av_freep(&v.latest);av_freep(&v.scratch);return -1;}
    v.running=1;v.need_idr=1;v.generation=1;
    const LabVideoProfile *profile=lab_video_profile(s->video_profile);
    v.frame_interval_us=1000000/profile->fps;v.buffer_us=profile->buffer_us;
    v.cpu_flags=av_get_cpu_flags();
    int rc=lab_airplay_thread_create(&v.worker,video_worker,NULL);
    if(rc){v.running=0;av_freep(&v.latest);av_freep(&v.scratch);return -1;}
    v.started=1;
    lab_log(s,"Video: optimized H264 CPU flags=0x%x; bounded reference-preserving queue",v.cpu_flags);
    return 0;
}
void lab_video_reset(void) {
    pthread_mutex_lock(&v.mutex);
    clear_queue();++v.generation;v.configured=0;v.need_idr=1;
    unsigned epoch=v.generation;
    v.sps_size=v.pps_size=0;
    pthread_cond_broadcast(&v.wake);pthread_mutex_unlock(&v.mutex);
    pthread_mutex_lock(&v.image_mutex);pthread_mutex_lock(&v.mutex);
    if(epoch==v.generation){v.have_frame=0;v.destination_serials[0]=v.destination_serials[1]=0;}
    pthread_mutex_unlock(&v.mutex);pthread_mutex_unlock(&v.image_mutex);
}
void lab_video_stream_state(int active) {
    if(!active)lab_video_reset();
    pthread_mutex_lock(&v.mutex);v.stream_active=active;pthread_mutex_unlock(&v.mutex);
    lab_session_trace("mirror stream",active?"started":"ended");
}
void lab_video_poll(LabState *s) {
    pthread_mutex_lock(&v.mutex);
    s->video_packets=v.packets;s->video_decoded=v.decoded;s->video_errors=v.errors;
    s->video_dropped=v.dropped;
    s->video_width=v.width;s->video_height=v.height;s->video_queue_bytes=(unsigned)v.bytes;
    s->video_converted=v.converted;s->video_render_skipped=v.render_skipped;
    s->video_backpressure=v.backpressure;s->video_decode_us=v.decode_us;s->video_convert_us=v.convert_us;
    s->video_stream_active=v.stream_active;s->video_cpu_flags=v.cpu_flags;
    s->video_pacing_us=v.pacing_us;s->video_pacer_resets=v.pacer_resets;
    uint64_t now=now_us();
    s->video_packet_age_ms=v.last_packet_us?(unsigned long)((now-v.last_packet_us)/1000):0;
    s->video_frame_age_ms=v.last_frame_us?(unsigned long)((now-v.last_frame_us)/1000):0;
    pthread_mutex_unlock(&v.mutex);
    pthread_mutex_lock(&v.image_mutex);
    s->video_copies=v.copies;s->video_copy_us=v.copy_us;s->video_presented=v.presented;
    pthread_mutex_unlock(&v.image_mutex);
}
int lab_video_draw(uint32_t *pixels) {
    pthread_mutex_lock(&v.image_mutex);
    int have=v.have_frame;
    if(have){
        unsigned slot=2;
        for(unsigned i=0;i<2;++i)if(v.destinations[i]==pixels)slot=i;
        if(slot==2){slot=v.next_destination++%2;v.destinations[slot]=pixels;v.destination_serials[slot]=0;}
        if(v.destination_serials[slot]!=v.latest_serial){
            uint64_t copy_start=now_us();
            memcpy(pixels,v.latest,LAB_WIDTH*LAB_HEIGHT*sizeof(*pixels));
            v.copy_us+=(unsigned long)(now_us()-copy_start);
            v.destination_serials[slot]=v.latest_serial;++v.copies;
        }
        if(v.last_presented!=v.latest_serial){++v.presented;v.last_presented=v.latest_serial;}
    }
    pthread_mutex_unlock(&v.image_mutex);return have;
}
void lab_video_invalidate_output(void) {
    pthread_mutex_lock(&v.image_mutex);
    v.destination_serials[0]=v.destination_serials[1]=0;
    pthread_mutex_unlock(&v.image_mutex);
}
void lab_video_stop(void) {
    if(!v.started)return;
    pthread_mutex_lock(&v.mutex);v.running=0;pthread_cond_broadcast(&v.wake);pthread_mutex_unlock(&v.mutex);
    pthread_join(v.worker,NULL);
    pthread_mutex_lock(&v.mutex);clear_queue();v.started=0;v.configured=0;pthread_mutex_unlock(&v.mutex);
    pthread_mutex_lock(&v.image_mutex);v.have_frame=0;
    av_freep(&v.latest);av_freep(&v.scratch);pthread_mutex_unlock(&v.image_mutex);
}
