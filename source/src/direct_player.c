/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "direct_player.h"
#include "media_io.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/time.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#ifdef LAB_PS4
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#else
#include <unistd.h>
extern int direct_host_http(MediaHttp *);
#endif
extern int lab_airplay_thread_create(pthread_t *,void *(*)(void *),void *);
static struct {
 MediaHttp io;int (*template_create)(int,const char *,int,int),(*redirect)(int,int),(*connect_timeout)(int,uint32_t),(*resolve_timeout)(int,uint32_t),(*recv_timeout)(int,uint32_t),(*send_timeout)(int,uint32_t);
 int (*pool)(const char *,int,int);void (*pool_free)(int);
 int (*ssl_init)(size_t),(*ssl_term)(int),(*http_init)(int,int,size_t),(*http_term)(int);
} api;
static struct {
 pthread_mutex_t mutex,image_mutex;pthread_t worker;int started,running,available,active,ready,paused,error,event;
 unsigned commands;int io_last_error;char io_detail[512];char protocol_log[2048];char url[8193],stage[32],message[2048];double start,seek,position,duration;int fraction,rate;
 unsigned long video_frames,audio_frames,errors,revision,play_requests,rejected,reverse_requests,actions,info_requests;
 uint32_t *image;int image_ready;unsigned long image_revision;unsigned long io_reads,io_bytes;int io_error;
} d={.mutex=PTHREAD_MUTEX_INITIALIZER,.image_mutex=PTHREAD_MUTEX_INITIALIZER};
enum {PLAY=1,SEEK=2,RATE=4,STOP=8};
static uint64_t now_us(void){
#ifdef LAB_PS4
 return sceKernelGetProcessTime();
#else
 return (uint64_t)av_gettime_relative();
#endif
}
static void sleep_us(unsigned n){
#ifdef LAB_PS4
 sceKernelUsleep(n);
#else
 usleep(n);
#endif
}
#ifdef LAB_PS4
static int template_create(int context,const char *agent,int version,int proxy){int id=api.template_create(context,agent,version,proxy);if(id>=0){api.redirect(id,1);api.connect_timeout(id,5000000);api.resolve_timeout(id,5000000);api.send_timeout(id,5000000);api.recv_timeout(id,5000000);}return id;}
#endif
static int resolve(void){
#ifdef LAB_PS4
 sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
 OrbisKernelModule modules[256];size_t count=0;if(sceKernelGetModuleList(modules,sizeof(modules),&count)<0)return -1;if(count>256)count=256;
#define SYM(member,name) do{void *p=NULL;for(size_t i=0;i<count && !p;++i)sceKernelDlsym(modules[i],name,&p);if(!p)return -1;memcpy(&api.member,&p,sizeof(p));}while(0)
 SYM(pool,"sceNetPoolCreate");SYM(pool_free,"sceNetPoolDestroy");SYM(ssl_init,"sceSslInit");SYM(ssl_term,"sceSslTerm");SYM(http_init,"sceHttpInit");SYM(http_term,"sceHttpTerm");
 SYM(io.template_create,"sceHttpCreateTemplate");SYM(io.connection_create,"sceHttpCreateConnectionWithURL");SYM(io.request_create,"sceHttpCreateRequestWithURL");SYM(io.header_add,"sceHttpAddRequestHeader");
 SYM(redirect,"sceHttpSetAutoRedirect");SYM(connect_timeout,"sceHttpSetConnectTimeOut");SYM(resolve_timeout,"sceHttpSetResolveTimeOut");SYM(send_timeout,"sceHttpSetSendTimeOut");SYM(recv_timeout,"sceHttpSetRecvTimeOut");
 SYM(io.send,"sceHttpSendRequest");SYM(io.status,"sceHttpGetStatusCode");SYM(io.length,"sceHttpGetResponseContentLength");SYM(io.headers,"sceHttpGetAllResponseHeaders");SYM(io.read,"sceHttpReadData");
 SYM(io.request_delete,"sceHttpDeleteRequest");SYM(io.connection_delete,"sceHttpDeleteConnection");SYM(io.template_delete,"sceHttpDeleteTemplate");
#undef SYM
 api.io.verified_tls=1;api.template_create=api.io.template_create;api.io.template_create=template_create;
 return 0;
#else
 return direct_host_http(&api.io);
#endif
}
static void stage(const char *value){pthread_mutex_lock(&d.mutex);snprintf(d.stage,sizeof(d.stage),"%s",value);pthread_mutex_unlock(&d.mutex);lab_session_trace("direct stage",value);}
static void error(int rc){char detail[128];av_strerror(rc,detail,sizeof(detail));pthread_mutex_lock(&d.mutex);d.error=rc;++d.errors;if(d.io_last_error && d.io_detail[0])snprintf(d.message,sizeof(d.message),"FFmpeg rc=0x%08x; IO rc=0x%08x: %s",(unsigned)rc,(unsigned)d.io_last_error,d.io_detail);else snprintf(d.message,sizeof(d.message),"FFmpeg: %s (0x%08x)",detail,(unsigned)rc);pthread_mutex_unlock(&d.mutex);}
static int interrupted(void *unused){(void)unused;pthread_mutex_lock(&d.mutex);int stop=!d.running || (d.commands&(PLAY|STOP));pthread_mutex_unlock(&d.mutex);return stop;}
static int control_pending(void){pthread_mutex_lock(&d.mutex);int pending=!d.running || d.commands;pthread_mutex_unlock(&d.mutex);return pending;}
static void clear_image(void){pthread_mutex_lock(&d.image_mutex);d.image_ready=0;++d.image_revision;pthread_mutex_unlock(&d.image_mutex);}

typedef struct {MediaFile file;uint64_t offset,base,length;AVIOContext *context;} Reader;
typedef struct {
 AVFormatContext *format;Reader *main_reader;AVCodecContext *video,*audio;AVPacket *packet;AVFrame *frame;
 int video_index,audio_index,http,source_rate,source_format,source_channels;struct SwsContext *sws;SwrContext *swr;
 int64_t clock_base,pause_started;double origin,target;unsigned media_frames;
} Player;
static void remember_io(Reader *r){
 pthread_mutex_lock(&r->file.mutex);pthread_mutex_lock(&d.mutex);
 d.io_error=r->file.error;d.io_reads=(unsigned long)r->file.reads;d.io_bytes=(unsigned long)r->file.bytes;
 if(r->file.error<0)d.io_last_error=r->file.error;
 if(r->file.error<0 || !d.io_last_error)snprintf(d.io_detail,sizeof(d.io_detail),"phase=%s origin=%s status=%d tls_verify=%ld tls_error=0x%lx",r->file.diagnostics.phase,r->file.diagnostics.origin,r->file.diagnostics.status,r->file.diagnostics.verify,r->file.diagnostics.ssl_error);
 pthread_mutex_unlock(&d.mutex);pthread_mutex_unlock(&r->file.mutex);
}
static int io_read(void *object,uint8_t *buffer,int size){Reader *r=object;if(interrupted(NULL))return AVERROR_EXIT;if(r->offset>=r->length)return AVERROR_EOF;if((uint64_t)size>r->length-r->offset)size=(int)(r->length-r->offset);int n=media_file_read(&r->file,buffer,r->base+r->offset,(uint32_t)size);if(n>0)r->offset+=(unsigned)n;
 remember_io(r);
 if(n<0 && interrupted(NULL))return AVERROR_EXIT;
 return n==0?AVERROR_EOF:n;
}
static int64_t io_seek(void *object,int64_t offset,int whence){Reader *r=object;uint64_t size=r->length;if(whence==AVSEEK_SIZE)return size>(uint64_t)INT64_MAX?AVERROR(ENOSYS):(int64_t)size;whence&=~AVSEEK_FORCE;
 int64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?(int64_t)r->offset:whence==SEEK_END?(int64_t)size:-1;
 if(base<0 || (offset>0 && base>INT64_MAX-offset) || (offset<0 && offset < -base))return AVERROR(EINVAL);
 int64_t next=base+offset;if(next<0 || (uint64_t)next>size)return AVERROR(EINVAL);r->offset=(uint64_t)next;return next;
}
static void close_reader(Reader *r){if(!r)return;media_file_destroy(&r->file);if(r->context){av_freep(&r->context->buffer);avio_context_free(&r->context);}free(r);}
static int open_reader(int http,const char *url,Reader **output){
 if(!url || (strncmp(url,"http://",7) && strncmp(url,"https://",8) && strcmp(url,"/app0/airplay-test.mp4")))return AVERROR(EACCES);
 Reader *r=calloc(1,sizeof(*r));if(!r)return AVERROR(ENOMEM);media_file_init(&r->file,&api.io,http);int rc=media_file_open(&r->file,url);
 if(rc<0){remember_io(r);close_reader(r);return interrupted(NULL)?AVERROR_EXIT:rc;}
 r->length=media_file_size(&r->file);
 unsigned char *buffer=av_malloc(32768);if(!buffer){close_reader(r);return AVERROR(ENOMEM);}
 r->context=avio_alloc_context(buffer,32768,0,r,io_read,NULL,io_seek);if(!r->context){av_free(buffer);close_reader(r);return AVERROR(ENOMEM);}r->context->seekable=r->length<=INT64_MAX?AVIO_SEEKABLE_NORMAL:0;*output=r;return 0;
}
static int format_io_open(AVFormatContext *format,AVIOContext **context,const char *url,int flags,AVDictionary **options){
 Player *p=format->opaque;if(flags&AVIO_FLAG_WRITE)return AVERROR(EACCES);Reader *r=NULL;int rc=open_reader(p->http,url,&r);if(rc<0)return rc;
 if(options && *options){
  AVDictionaryEntry *start=av_dict_get(*options,"offset",NULL,0),*finish=av_dict_get(*options,"end_offset",NULL,0);uint64_t size=r->length;
  if(start){char *end=NULL;unsigned long long n=strtoull(start->value,&end,10);if(!end || *end || n>size){close_reader(r);return AVERROR(EINVAL);}r->base=n;r->length=size-r->base;}
  if(finish){char *end=NULL;unsigned long long n=strtoull(finish->value,&end,10);if(!end || *end || n<r->base || n>size){close_reader(r);return AVERROR(EINVAL);}r->length=n-r->base;}
 }
 *context=r->context;return 0;
}
static int format_io_close(AVFormatContext *format,AVIOContext *context){(void)format;if(context)close_reader(context->opaque);return 0;}
static void player_close(Player *p){
 avformat_close_input(&p->format);close_reader(p->main_reader);p->main_reader=NULL;avcodec_free_context(&p->video);avcodec_free_context(&p->audio);av_packet_free(&p->packet);av_frame_free(&p->frame);sws_freeContext(p->sws);p->sws=NULL;swr_free(&p->swr);p->source_rate=0;
 lab_audio_direct_mode(0);clear_image();pthread_mutex_lock(&d.mutex);d.active=d.ready=d.paused=0;pthread_mutex_unlock(&d.mutex);
}
static int decoder_open(AVStream *stream,AVCodecContext **context){const AVCodec *codec=avcodec_find_decoder(stream->codecpar->codec_id);if(!codec)return AVERROR_DECODER_NOT_FOUND;AVCodecContext *c=avcodec_alloc_context3(codec);if(!c)return AVERROR(ENOMEM);int rc=avcodec_parameters_to_context(c,stream->codecpar);if(rc>=0){c->thread_count=1;rc=avcodec_open2(c,codec,NULL);}if(rc<0){avcodec_free_context(&c);return rc;}*context=c;return 0;}
static int seek_player(Player *p,double seconds){
 int64_t timestamp=(int64_t)((seconds+p->origin)*AV_TIME_BASE);int rc=avformat_seek_file(p->format,-1,INT64_MIN,timestamp,INT64_MAX,AVSEEK_FLAG_BACKWARD);if(rc<0)return rc;
 if(p->video)avcodec_flush_buffers(p->video);if(p->audio)avcodec_flush_buffers(p->audio);swr_free(&p->swr);p->source_rate=0;p->target=seconds;p->clock_base=(int64_t)now_us()-(int64_t)(seconds*1000000);p->pause_started=(int64_t)now_us();lab_audio_direct_mode(1);clear_image();pthread_mutex_lock(&d.mutex);d.position=seconds;pthread_mutex_unlock(&d.mutex);return 0;
}
static int player_open(Player *p,const char *url,double start,int fraction){
 stage("opening");int rc=open_reader(p->http,url,&p->main_reader);if(rc<0)return rc;p->format=avformat_alloc_context();if(!p->format)return AVERROR(ENOMEM);
 p->format->opaque=p;p->format->pb=p->main_reader->context;p->format->flags|=AVFMT_FLAG_CUSTOM_IO;p->format->io_open=format_io_open;p->format->io_close2=format_io_close;p->format->interrupt_callback=(AVIOInterruptCB){interrupted,NULL};p->format->probesize=1024*1024;p->format->max_analyze_duration=2000000;
 AVDictionary *options=NULL;av_dict_set(&options,"http_persistent","0",0);av_dict_set(&options,"http_multiple","0",0);av_dict_set(&options,"allowed_extensions","ALL",0);
 rc=avformat_open_input(&p->format,url,NULL,&options);av_dict_free(&options);if(rc<0)return rc;stage("probing");rc=avformat_find_stream_info(p->format,NULL);if(rc<0)return rc;
 p->origin=p->format->start_time!=AV_NOPTS_VALUE?p->format->start_time/(double)AV_TIME_BASE:0;p->video_index=p->audio_index=-1;int64_t best=-1;
 for(unsigned i=0;i<p->format->nb_streams;++i){AVCodecParameters *c=p->format->streams[i]->codecpar;if(c->codec_type!=AVMEDIA_TYPE_VIDEO || c->codec_id!=AV_CODEC_ID_H264 || c->width<1 || c->height<1 || c->width>1920 || c->height>1920 || (int64_t)c->width*c->height>1920*1080)continue;double fps=av_q2d(p->format->streams[i]->avg_frame_rate);if(fps>30.1 && (int64_t)c->width*c->height>1280*720)continue;int64_t score=(int64_t)c->width*c->height;if(score>best){best=score;p->video_index=(int)i;}}
 if(p->video_index<0)return AVERROR_DECODER_NOT_FOUND;
 const AVCodec *audio_codec=NULL;p->audio_index=av_find_best_stream(p->format,AVMEDIA_TYPE_AUDIO,-1,p->video_index,&audio_codec,0);
 for(unsigned i=0;i<p->format->nb_streams;++i)p->format->streams[i]->discard=(int)i==p->video_index || (int)i==p->audio_index?AVDISCARD_DEFAULT:AVDISCARD_ALL;
 stage("decoder");rc=decoder_open(p->format->streams[p->video_index],&p->video);if(rc<0)return rc;
 if(p->audio_index>=0){rc=decoder_open(p->format->streams[p->audio_index],&p->audio);if(rc<0)return rc;}
 p->packet=av_packet_alloc();p->frame=av_frame_alloc();if(!p->packet || !p->frame)return AVERROR(ENOMEM);
 double duration=p->format->duration>0?p->format->duration/(double)AV_TIME_BASE:0;
 pthread_mutex_lock(&d.mutex);d.duration=duration;d.position=0;d.ready=d.active=1;d.event=3;pthread_mutex_unlock(&d.mutex);
 p->clock_base=(int64_t)now_us();p->target=0;p->media_frames=0;
 if(start>0){double target=fraction?duration*start:start;if(target>0){rc=seek_player(p,target);if(rc<0)return rc;}}
 lab_audio_direct_mode(1);stage("playing");return 0;
}
static double frame_position(Player *p,AVFrame *frame,int stream){int64_t pts=frame->best_effort_timestamp;if(pts==AV_NOPTS_VALUE)return d.position;return pts*av_q2d(p->format->streams[stream]->time_base)-p->origin;}
static int render_frame(Player *p,AVFrame *frame,double position){
 if(position+0.03<p->target)return 0;
 int64_t target=p->clock_base+(int64_t)(position*1000000);uint64_t now=now_us();
 if(target<(int64_t)now-500000){p->clock_base=(int64_t)now-(int64_t)(position*1000000);target=(int64_t)now;}
 while(target>(int64_t)now_us()){if(control_pending())return 1;sleep_us(2000);}
 int width=frame->width,height=frame->height;if(width<1 || height<1 || width>1920 || height>1920 || (int64_t)width*height>1920*1080)return AVERROR_INVALIDDATA;
 double aspect=width/(double)height;if(frame->sample_aspect_ratio.num>0 && frame->sample_aspect_ratio.den>0)aspect*=av_q2d(frame->sample_aspect_ratio);
 if(!isfinite(aspect) || aspect<0.1 || aspect>10)return AVERROR_INVALIDDATA;
 int ow=LAB_WIDTH,oh=(int)(ow/aspect+0.5);if(oh>LAB_HEIGHT){oh=LAB_HEIGHT;ow=(int)(oh*aspect+0.5);}
 p->sws=sws_getCachedContext(p->sws,width,height,frame->format,ow,oh,AV_PIX_FMT_BGRA,SWS_BILINEAR,NULL,NULL,NULL);if(!p->sws)return AVERROR(ENOMEM);
 const int *coeff=sws_getCoefficients(frame->colorspace==AVCOL_SPC_BT709?SWS_CS_ITU709:SWS_CS_ITU601);sws_setColorspaceDetails(p->sws,coeff,frame->color_range==AVCOL_RANGE_JPEG,coeff,1,0,1<<16,1<<16);
 pthread_mutex_lock(&d.image_mutex);if(!d.image)d.image=malloc(LAB_WIDTH*LAB_HEIGHT*4);if(!d.image){pthread_mutex_unlock(&d.image_mutex);return AVERROR(ENOMEM);}memset(d.image,0,LAB_WIDTH*LAB_HEIGHT*4);
 uint8_t *out[4]={(uint8_t *)(d.image+((LAB_HEIGHT-oh)/2)*LAB_WIDTH+(LAB_WIDTH-ow)/2),NULL,NULL,NULL};int strides[4]={LAB_WIDTH*4,0,0,0};int rc=sws_scale(p->sws,(const uint8_t *const *)frame->data,frame->linesize,0,height,out,strides);if(rc>0){d.image_ready=1;++d.image_revision;}pthread_mutex_unlock(&d.image_mutex);
 if(rc<=0)return AVERROR_INVALIDDATA;pthread_mutex_lock(&d.mutex);++d.video_frames;d.position=position<0?0:position;pthread_mutex_unlock(&d.mutex);++p->media_frames;return 0;
}
static int audio_frame(Player *p,AVFrame *frame,double position){
 if(position+frame->nb_samples/(double)frame->sample_rate<p->target)return 0;
 int rate=frame->sample_rate,channels=frame->ch_layout.nb_channels;if(rate<8000 || rate>192000 || channels<1 || channels>8)return AVERROR_INVALIDDATA;
 if(!p->swr || p->source_rate!=rate || p->source_channels!=channels || p->source_format!=frame->format){swr_free(&p->swr);AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;int rc=swr_alloc_set_opts2(&p->swr,&stereo,AV_SAMPLE_FMT_S16,48000,&frame->ch_layout,frame->format,rate,0,NULL);if(rc<0 || (rc=swr_init(p->swr))<0)return rc;p->source_rate=rate;p->source_channels=channels;p->source_format=frame->format;}
 int16_t samples[32768];uint8_t *out=(uint8_t *)samples;int frames=swr_convert(p->swr,&out,16384,(const uint8_t **)frame->extended_data,frame->nb_samples);if(frames<0)return frames;
 while(!lab_audio_direct_space()){if(control_pending())return 1;sleep_us(2000);}
 if(frames>0){lab_audio_direct_pcm(samples,(unsigned)frames);pthread_mutex_lock(&d.mutex);d.audio_frames+=(unsigned)frames;pthread_mutex_unlock(&d.mutex);}return 0;
}
static int decode_packet(Player *p,AVCodecContext *decoder,AVPacket *packet,int video){int rc=avcodec_send_packet(decoder,packet);if(rc<0 && rc!=AVERROR_EOF)return rc;
 while((rc=avcodec_receive_frame(decoder,p->frame))>=0){double position=frame_position(p,p->frame,video?p->video_index:p->audio_index);int result=video?render_frame(p,p->frame,position):audio_frame(p,p->frame,position);av_frame_unref(p->frame);if(result)return result;}
 return rc==AVERROR(EAGAIN) || rc==AVERROR_EOF?0:rc;
}
static void *worker(void *unused){(void)unused;Player p={0};int net=-1,ssl=-1,http=-1;
 for(;;){
  pthread_mutex_lock(&d.mutex);int running=d.running;unsigned commands=d.commands;d.commands=0;char next[8193];if(commands&PLAY)memcpy(next,d.url,sizeof(next));double start=d.start,seek=d.seek;int fraction=d.fraction,rate=d.rate;pthread_mutex_unlock(&d.mutex);
  if(!running || (commands&(PLAY|STOP))){player_close(&p);if(!running)break;}
  if(commands&PLAY){
   pthread_mutex_lock(&d.mutex);d.error=d.event=d.io_error=d.io_last_error=0;d.message[0]=d.io_detail[0]=0;d.io_reads=d.io_bytes=0;d.duration=d.position=0;d.active=1;pthread_mutex_unlock(&d.mutex);
#ifdef LAB_PS4
   if(http<0){stage("net-pool");net=api.pool("airplay-video",4*1024*1024,0);if(net<0){error(net);player_close(&p);continue;}stage("ssl-init");ssl=api.ssl_init(4*1024*1024);if(ssl<0){error(ssl);api.pool_free(net);net=-1;player_close(&p);continue;}tls_http_net_pool(net);stage("http-init");http=api.http_init(net,ssl,4*1024*1024);if(http<0){error(http);api.ssl_term(ssl);api.pool_free(net);ssl=net=-1;player_close(&p);continue;}}
#else
   http=1;
#endif
   p.http=http;int rc=player_open(&p,next,start,fraction);if(rc<0){if(rc!=AVERROR_EXIT)error(rc);player_close(&p);continue;}
  }
  if(p.format){
   if(commands&SEEK){stage("seeking");int rc=seek_player(&p,seek);if(rc<0)error(rc);stage("playing");}
   if(commands&RATE){uint64_t now=now_us();pthread_mutex_lock(&d.mutex);if(!rate && !d.paused){d.paused=1;p.pause_started=(int64_t)now;}else if(rate && d.paused){d.paused=0;p.clock_base+=(int64_t)now-p.pause_started;}pthread_mutex_unlock(&d.mutex);lab_audio_direct_mode(1);stage(rate?"playing":"paused");}
   pthread_mutex_lock(&d.mutex);int paused=d.paused;pthread_mutex_unlock(&d.mutex);if(paused){sleep_us(2000);continue;}
   int rc=av_read_frame(p.format,p.packet);
   if(rc>=0){int result=0;if(p.packet->stream_index==p.video_index)result=decode_packet(&p,p.video,p.packet,1);else if(p.audio && p.packet->stream_index==p.audio_index)result=decode_packet(&p,p.audio,p.packet,0);av_packet_unref(p.packet);if(result<0){error(result);player_close(&p);}}
   else if(rc==AVERROR_EOF){decode_packet(&p,p.video,NULL,1);if(p.audio)decode_packet(&p,p.audio,NULL,0);if(!control_pending()){stage("finished");player_close(&p);pthread_mutex_lock(&d.mutex);d.event=1;pthread_mutex_unlock(&d.mutex);}}
   else if(rc!=AVERROR_EXIT && rc!=AVERROR(EAGAIN)){error(rc);player_close(&p);}
  }else sleep_us(2000);
 }
#ifdef LAB_PS4
 tls_http_shutdown();if(http>=0)api.http_term(http);if(ssl>=0)api.ssl_term(ssl);if(net>=0)api.pool_free(net);
#else
 tls_http_shutdown();(void)net;(void)ssl;
#endif
 return NULL;
}
int direct_start(void){tls_http_interrupt(interrupted,NULL);if(d.started)return d.available;d.available=resolve()==0;if(!d.available)return 0;av_log_set_level(AV_LOG_ERROR);d.running=1;if(lab_airplay_thread_create(&d.worker,worker,NULL)){d.running=d.available=0;return 0;}d.started=1;return 1;}
static void play(const char *url,float start,int fraction){if(!d.available || !url || !isfinite(start) || start<0 || (fraction && start>1))return;size_t n=strnlen(url,8193);if(n>8192 || (strncmp(url,"http://",7) && strncmp(url,"https://",8)))return;for(size_t i=0;i<n;++i)if((unsigned char)url[i]<32)return;pthread_mutex_lock(&d.mutex);memcpy(d.url,url,n+1);d.start=start;d.fraction=fraction;d.commands=PLAY;pthread_mutex_unlock(&d.mutex);}
void direct_play(void *cls,const char *url,float seconds){(void)cls;play(url,seconds,0);}
void direct_play_fraction(void *cls,const char *url,float fraction){(void)cls;play(url,fraction,1);}
void direct_seek(void *cls,float seconds){(void)cls;if(!isfinite(seconds) || seconds<0 || seconds>604800)return;pthread_mutex_lock(&d.mutex);d.seek=seconds;d.commands|=SEEK;pthread_mutex_unlock(&d.mutex);}
void direct_rate(void *cls,float rate){(void)cls;if(!isfinite(rate))return;pthread_mutex_lock(&d.mutex);d.rate=rate>0;d.commands|=RATE;pthread_mutex_unlock(&d.mutex);}
void direct_stop(void *cls){(void)cls;pthread_mutex_lock(&d.mutex);d.commands=STOP;pthread_mutex_unlock(&d.mutex);}
void direct_test(int mode){if(mode<1 || mode>4 || !d.available)return;pthread_mutex_lock(&d.mutex);snprintf(d.url,sizeof(d.url),"%s",mode==1 || mode==4?"/app0/airplay-test.mp4":"http://127.0.0.1:9080/test-video.mp4");d.start=0;d.fraction=0;d.commands=PLAY;pthread_mutex_unlock(&d.mutex);}
void direct_info(void *cls,playback_info_t *info){(void)cls;pthread_mutex_lock(&d.mutex);info->duration=d.duration;info->position=d.position;info->rate=d.active && !d.paused?1:0;info->ready_to_play=d.ready;info->playback_buffer_empty=!d.ready;info->playback_buffer_full=false;info->playback_likely_to_keep_up=d.ready;info->seek_start=0;info->seek_duration=d.duration;pthread_mutex_unlock(&d.mutex);}
float direct_remove(void *cls){playback_info_t info={0};direct_info(cls,&info);direct_stop(cls);return (float)info.position;}
void direct_protocol_trace(void *cls,const char *label){
 (void)cls;pthread_mutex_lock(&d.mutex);size_t n=strnlen(label,112),len=strlen(d.protocol_log);
 while(len+n+2>=sizeof(d.protocol_log)){char *end=strchr(d.protocol_log,'\n');size_t drop=end?(size_t)(end-d.protocol_log)+1:len;memmove(d.protocol_log,d.protocol_log+drop,len-drop+1);len-=drop;}
 memcpy(d.protocol_log+len,label,n);d.protocol_log[len+n]='\n';d.protocol_log[len+n+1]=0;pthread_mutex_unlock(&d.mutex);
}
void direct_protocol(void *cls,int event){(void)cls;pthread_mutex_lock(&d.mutex);if(event==1)++d.play_requests;else if(event==2)++d.rejected;else if(event==3)++d.reverse_requests;else if(event==4)++d.actions;else if(event==5)++d.info_requests;pthread_mutex_unlock(&d.mutex);}
static struct {uint32_t *ptr;unsigned long revision;} targets[2];
void direct_invalidate(void){targets[0].revision=targets[1].revision=0;}
int direct_draw(uint32_t *pixels){pthread_mutex_lock(&d.image_mutex);int ready=d.image_ready;if(ready){int i=targets[0].ptr==pixels?0:targets[1].ptr==pixels?1:targets[0].ptr?1:0;if(targets[i].ptr!=pixels || targets[i].revision!=d.image_revision){memcpy(pixels,d.image,LAB_WIDTH*LAB_HEIGHT*4);targets[i].ptr=pixels;targets[i].revision=d.image_revision;}}else direct_invalidate();pthread_mutex_unlock(&d.image_mutex);return ready;}
void direct_poll(LabState *s){pthread_mutex_lock(&d.mutex);snprintf(s->direct_protocol_log,sizeof(s->direct_protocol_log),"%s",d.protocol_log);s->direct_available=d.available;s->direct_active=d.active;s->direct_error=d.error;s->direct_event=d.event;s->direct_video_frames=d.video_frames;s->direct_audio_frames=d.audio_frames;s->direct_errors=d.errors;s->direct_position=d.position;s->direct_duration=d.duration;s->direct_io_error=d.io_error;s->direct_io_reads=d.io_reads;s->direct_io_bytes=d.io_bytes;s->direct_play_requests=d.play_requests;s->direct_play_rejected=d.rejected;s->direct_reverse_requests=d.reverse_requests;s->direct_action_requests=d.actions;s->direct_info_requests=d.info_requests;snprintf(s->direct_stage,sizeof(s->direct_stage),"%s",d.stage);snprintf(s->direct_native_log,sizeof(s->direct_native_log),"%s",d.message);static unsigned long last_errors;static char last_stage[32];if(last_errors!=d.errors || strcmp(last_stage,d.stage)){lab_log(s,"Direct FFmpeg: stage=%s rc=0x%08x",d.stage,(unsigned)d.error);last_errors=d.errors;snprintf(last_stage,sizeof(last_stage),"%s",d.stage);}pthread_mutex_unlock(&d.mutex);}
void direct_shutdown(void){if(!d.started)return;pthread_mutex_lock(&d.mutex);d.running=0;pthread_mutex_unlock(&d.mutex);pthread_join(d.worker,NULL);tls_http_interrupt(NULL,NULL);free(d.image);d.image=NULL;d.started=0;}
