/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lab.h"
#include "video_receiver.h"
#include "video_profile.h"
#include "now_playing.h"
#include "direct_player.h"
#include "raop.h"
#include "mdnsd/mdnsd.h"
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/mem.h>
#include <orbis/AudioOut.h>
#include <orbis/NetCtl.h>
#include <orbis/Sysmodule.h>
#include <fcntl.h>
#include <orbis/libkernel.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>

/* OpenOrbis Random.h still has an untyped placeholder. */
extern int lab_airplay_thread_create(pthread_t *,void *(*)(void *),void *);
static int (*console_random)(void *, size_t);
static int random_bytes(unsigned char *out, int n) {
    if (n < 0) return 0;
    while (n) {
        int take = n > 64 ? 64 : n;
        if (!console_random || console_random(out, (size_t)take) != 0) return 0;
        out += take; n -= take;
    }
    return 1;
}
static int random_seed(const void *b, int n) { (void)b; (void)n; return 1; }
static int random_add(const void *b, int n, double e) { (void)b; (void)n; (void)e; return 1; }
static int random_status(void) { unsigned char probe; return random_bytes(&probe, 1); }
static RAND_METHOD os_random = {random_seed, random_bytes, NULL, random_add, random_bytes, random_status};

#define FIFO_SAMPLES (48000 * 2)
static struct {
    pthread_mutex_t mutex;
    pthread_t worker;
    int direct_mode;
    int running, worker_started, codec_type, ready, output_error, output_started;
    unsigned long packets, decoded, dropped, errors, event_id;
    char message[220];
    int16_t fifo[FIFO_SAMPLES];
    unsigned read, count;
    double gain;
    AVCodecContext *decoder;
    SwrContext *resampler;
    unsigned char hw[6];
    char device[20];
    unsigned short port;
    raop_t *raop;
    dnssd_t *dns;
} receiver = {.mutex=PTHREAD_MUTEX_INITIALIZER, .gain=0.25};

static void event(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(receiver.message, sizeof(receiver.message), fmt, ap);
    va_end(ap); ++receiver.event_id;
}
static void log_callback(void *cls, int level, const char *message) {
    (void)cls;
    if (level > 4) return;
    if(strstr(message,"http://") || strstr(message,"https://") || strstr(message,"x-apple-"))return;
    pthread_mutex_lock(&receiver.mutex); event("AirPlay: %.185s", message); pthread_mutex_unlock(&receiver.mutex);
}
static void decoder_close(void) {
    avcodec_free_context(&receiver.decoder);
    swr_free(&receiver.resampler);
    receiver.codec_type = 0;
    receiver.read = receiver.count = receiver.ready = 0;
}
static void format(void *cls, unsigned char *ct, unsigned short *spf,
                   bool *screen, bool *media, uint64_t *fmt) {
    (void)cls; (void)spf; (void)screen; (void)media; (void)fmt;
    lab_session_trace("audio format","decoder init begin");
    pthread_mutex_lock(&receiver.mutex);
    decoder_close();
    enum AVCodecID id = *ct == 2 ? AV_CODEC_ID_ALAC : *ct == 1 ? AV_CODEC_ID_PCM_S16LE : AV_CODEC_ID_AAC;
    const AVCodec *codec = avcodec_find_decoder(id);
    receiver.decoder = codec ? avcodec_alloc_context3(codec) : NULL;
    const char *hex = *ct == 2 ? "00000024616c616300000000000001600010280a0e0200ff00000000000000000000ac44" :
                      *ct == 8 ? "f8e85000" : *ct == 1 ? "" : "1210";
    int rc = -1;
    if (receiver.decoder) {
        AVCodecContext *c = receiver.decoder;
        c->sample_rate = 44100; c->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        c->thread_count = 1;
        c->extradata_size = (int)strlen(hex) / 2;
        c->extradata = av_mallocz(c->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (c->extradata) {
            for (int i = 0; i < c->extradata_size; ++i) {
                unsigned v = 0; sscanf(hex + i * 2, "%2x", &v); c->extradata[i] = v;
            }
            rc = avcodec_open2(c, codec, NULL);
        }
    }
    if (rc < 0) { ++receiver.errors; decoder_close(); }
    else receiver.codec_type = *ct;
    char stage[80];snprintf(stage,sizeof(stage),"ct=%u rc=%d",*ct,rc);
    lab_session_trace("audio format done",stage);
    event("Audio format ct=%u; decoder=%d", *ct, rc);
    pthread_mutex_unlock(&receiver.mutex);
}
static void process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data) {
    (void)cls; (void)ntp;
    if (!data || data->data_len <= 0 || data->data_len > 1024 * 1024) return;
    pthread_mutex_lock(&receiver.mutex); ++receiver.packets;
    if(receiver.direct_mode){pthread_mutex_unlock(&receiver.mutex);return;}
    if(receiver.packets==1)lab_session_trace("audio packet","first decoder input");
    if (!receiver.decoder) { ++receiver.errors; pthread_mutex_unlock(&receiver.mutex); return; }
    AVPacket *packet = av_packet_alloc(); AVFrame *frame = av_frame_alloc();
    if (!packet || !frame || av_new_packet(packet, data->data_len) < 0) goto done;
    memcpy(packet->data, data->data, data->data_len);
    int rc = avcodec_send_packet(receiver.decoder, packet);
    if (rc < 0) { ++receiver.errors; goto done; }
    while ((rc = avcodec_receive_frame(receiver.decoder, frame)) >= 0) {
        if (!receiver.resampler) {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            rc = swr_alloc_set_opts2(&receiver.resampler, &stereo, AV_SAMPLE_FMT_S16, 48000,
                 &frame->ch_layout, frame->format, frame->sample_rate, 0, NULL);
            if (rc < 0 || swr_init(receiver.resampler) < 0) { swr_free(&receiver.resampler); ++receiver.errors; break; }
        }
        int16_t pcm[16384]; uint8_t *out = (uint8_t *)pcm;
        int frames = swr_convert(receiver.resampler, &out, 8192,
                               (const uint8_t **)frame->extended_data, frame->nb_samples);
        if (frames < 0) { ++receiver.errors; break; }
        np_audio((unsigned)frame->sample_rate);
        unsigned samples = (unsigned)frames * 2;
        if (receiver.count + samples > FIFO_SAMPLES) {
            unsigned drop = receiver.count + samples - FIFO_SAMPLES;
            receiver.read = (receiver.read + drop) % FIFO_SAMPLES;
            receiver.count -= drop; ++receiver.dropped;
        }
        for (unsigned i = 0; i < samples; ++i)
            receiver.fifo[(receiver.read + receiver.count + i) % FIFO_SAMPLES] = pcm[i];
        if(!receiver.decoded)lab_session_trace("audio decode","first PCM frame");
        receiver.count += samples; receiver.decoded += (unsigned long)frames;
        if (receiver.count >= 9600) receiver.ready = 1;
        av_frame_unref(frame);
    }
done:
    av_packet_free(&packet); av_frame_free(&frame);
    pthread_mutex_unlock(&receiver.mutex);
}
static void flush(void *cls) {
    (void)cls; pthread_mutex_lock(&receiver.mutex);
    receiver.count = receiver.read = receiver.ready = 0;
    swr_free(&receiver.resampler);
    if (receiver.decoder) avcodec_flush_buffers(receiver.decoder);
    pthread_mutex_unlock(&receiver.mutex);
}
static void volume(void *cls, float db) {
    (void)cls; pthread_mutex_lock(&receiver.mutex);
    receiver.gain = isfinite(db) ? db <= -144 ? 0 : pow(10, fmin(0, db) / 20) : 0;
    pthread_mutex_unlock(&receiver.mutex);
}
static double initial_volume(void *cls) { (void)cls; return -12.0; }
void lab_audio_direct_mode(int active) {
    pthread_mutex_lock(&receiver.mutex);receiver.direct_mode=active;
    receiver.read=receiver.count=receiver.ready=0;pthread_mutex_unlock(&receiver.mutex);
}
unsigned lab_audio_direct_space(void) {
    pthread_mutex_lock(&receiver.mutex);unsigned n=receiver.count<=2048?FIFO_SAMPLES-receiver.count:0;
    pthread_mutex_unlock(&receiver.mutex);return n/2;
}
void lab_audio_direct_pcm(const int16_t *pcm,unsigned frames) {
    if(!pcm || frames>FIFO_SAMPLES/2)return;
    pthread_mutex_lock(&receiver.mutex);unsigned samples=frames*2;
    if(receiver.direct_mode && receiver.count+samples<=FIFO_SAMPLES){
        for(unsigned i=0;i<samples;++i)receiver.fifo[(receiver.read+receiver.count+i)%FIFO_SAMPLES]=pcm[i];
        receiver.count+=samples;if(receiver.count>=512)receiver.ready=1;
    }
    pthread_mutex_unlock(&receiver.mutex);
}
static void metadata(void *cls,const void *data,int size){(void)cls;np_metadata(data,size);}
static void coverart(void *cls,const void *data,int size){(void)cls;np_coverart(data,size);}
static void progress(void *cls,uint32_t *start,uint32_t *current,uint32_t *end){(void)cls;if(start && current && end)np_progress(*start,*current,*end);}
static void music_stop(void *cls){(void)cls;np_stop();}
static void mirror_running(void *cls, bool running) { (void)cls; lab_video_stream_state(running); }
static void noop(void *cls) { (void)cls; }
static void reset(void *cls, int why) { (void)why; flush(cls); }
static void video_reset(void *cls, reset_type_t type) { (void)type; (void)cls; lab_video_reset(); }
static void video_flush(void *cls) { (void)cls; lab_video_reset(); }
static void video(void *cls, raop_ntp_t *ntp, video_decode_struct *data) {
    (void)cls;(void)ntp;
    if(data)lab_video_submit(data->data,data->data_len,data->is_h265,data->ntp_time_remote/1000);
}
static int video_size(void *cls, video_codec_t type, unsigned short a, unsigned short b,
                      unsigned short c, unsigned short d, unsigned short e, unsigned short f) {
    (void)cls;(void)c;(void)d;
    return lab_video_configure(type==VIDEO_CODEC_H264,e?e:a,f?f:b);
}
static void *audio_worker(void *unused) {
    (void)unused;
    sceAudioOutInit();
    int handle = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
                                0, 256, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (handle < 0) {
        pthread_mutex_lock(&receiver.mutex);receiver.output_error=handle;
        event("AirPlay AudioOutOpen failed: %08x", (unsigned)handle);pthread_mutex_unlock(&receiver.mutex);return NULL;
    }
    pthread_mutex_lock(&receiver.mutex);receiver.output_started=1;pthread_mutex_unlock(&receiver.mutex);
    int last_rc=0;
    int16_t pcm[512];
    for (;;) {
        int rc = sceAudioOutOutput(handle, NULL);
        if (rc < 0) { last_rc=rc; break; }
        pthread_mutex_lock(&receiver.mutex);
        int running = receiver.running;
        memset(pcm, 0, sizeof(pcm));
        if (receiver.ready && receiver.count >= 512) {
            for (int i = 0; i < 512; ++i) pcm[i] = (int16_t)(receiver.fifo[(receiver.read + i) % FIFO_SAMPLES] * receiver.gain);
            receiver.read = (receiver.read + 512) % FIFO_SAMPLES; receiver.count -= 512;
        } else if (receiver.ready) receiver.ready = 0;
        pthread_mutex_unlock(&receiver.mutex);
        if (!running) break;
        last_rc=sceAudioOutOutput(handle, pcm);
        if (last_rc < 0) break;
    }
    sceAudioOutOutput(handle, NULL); sceAudioOutClose(handle);
    if(last_rc<0){pthread_mutex_lock(&receiver.mutex);receiver.output_error=last_rc;event("Music output failed: %08x",(unsigned)last_rc);pthread_mutex_unlock(&receiver.mutex);}
    return NULL;
}
static int startup_phase=0;
static void checkpoint(LabState *s,const char *message) {
    mkdir("/data/ps4-receiver",0700);
    int fd=sceKernelOpen("/data/ps4-receiver/startup.log",O_WRONLY|O_CREAT|O_APPEND,0600);
    if(fd>=0){sceKernelWrite(fd,message,strlen(message));sceKernelWrite(fd,"\n",1);sceKernelClose(fd);}
    lab_log(s,"%s",message);
}
void lab_airplay_restore_log(LabState *s) {
    FILE *f=fopen("/data/ps4-receiver/startup.log","rb");
    if(!f)return;
    char data[4097];fseek(f,0,SEEK_END);long size=ftell(f);
    if(size>4096)fseek(f,size-4096,SEEK_SET);else rewind(f);
    size_t n=fread(data,1,sizeof(data)-1,f);data[n]=0;fclose(f);
    lab_log(s,"Previous startup checkpoints:");
    char *save=NULL;for(char *line=strtok_r(data,"\n",&save);line;line=strtok_r(NULL,"\n",&save))lab_log(s,"%s",line);
}
int lab_airplay_start(LabState *s) {
    unsigned char *hw=receiver.hw;char *device=receiver.device;
    if(startup_phase>=6)return 0;
    if(startup_phase<0){lab_log(s,"Restart app before another startup attempt");return -1;}
    int phase=startup_phase+1;
    char message[100];snprintf(message,sizeof(message),"START step %d",phase);
    checkpoint(s,message);
    s->airplay_step=phase;
    /* A failing phase must not be skipped by a second browser click. */
    startup_phase=-1;
    switch(phase) {
    case 1: {
    int signal_rc=lab_airplay_signals_init();
    lab_log(s,"AirPlay SIGPIPE guard: %d",signal_rc);
    if(signal_rc!=0)return -1;
    checkpoint(s,"HEN component: probing GoldHEN SDK");
    if(lab_hen_open(s)!=0){checkpoint(s,"HEN component unavailable; startup stopped");return -1;}
    checkpoint(s,"HEN component: process privileges acquired");
    lab_video_profile_load(s);
    int load_rc=sceSysmoduleLoadModule(ORBIS_SYSMODULE_RANDOM);
    lab_log(s,"Random sysmodule load: %08x",(unsigned)load_rc);
    OrbisKernelModule modules[256]={0};size_t count=0;
    int list_rc=sceKernelGetModuleList(modules,sizeof(modules),&count);
    if(list_rc<0){lab_log(s,"Module list failed: %08x",(unsigned)list_rc);return -1;}
    if(count>256)count=256;
    for(size_t i=0;i<count && !console_random;++i){
        void *ptr=NULL;
        if(sceKernelDlsym(modules[i],"sceRandomGetRandomNumber",&ptr)==0 && ptr)
            memcpy(&console_random,&ptr,sizeof(console_random));
    }
    if(!console_random){lab_log(s,"Console RNG symbol unavailable");return -1;}
    if (RAND_set_rand_method(&os_random) != 1 || !random_status()) { lab_log(s,"OS RNG unavailable; AirPlay disabled");return -1; }
        break;
    }
    case 2: {
    EVP_PKEY_CTX *testctx=EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519,NULL);
    EVP_PKEY *testkey=NULL;
    int crypto_ok=testctx && EVP_PKEY_keygen_init(testctx)>0 && EVP_PKEY_keygen(testctx,&testkey)>0;
    EVP_PKEY_free(testkey);EVP_PKEY_CTX_free(testctx);
    if(!crypto_ok){lab_log(s,"Crypto self-test failed; AirPlay disabled");return -1;}
    lab_log(s,"Crypto Ed25519 keygen self-test passed using console RNG");
        break;
    }
    case 3: {
    OrbisNetCtlInfo info; memset(&info,0,sizeof(info));
    int mac_rc=sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_ETHER_ADDR,&info);
    if(mac_rc != 0){lab_log(s,"AirPlay MAC query failed: %08x",(unsigned)mac_rc);return -1;}
    memcpy(hw,info.ether_addr,6);
    snprintf(device,sizeof(receiver.device),"%02X:%02X:%02X:%02X:%02X:%02X",hw[0],hw[1],hw[2],hw[3],hw[4],hw[5]);
    mkdir("/data/ps4-receiver",0700);
        break;
    }
    case 4: {
    if(np_start()!=0){lab_log(s,"Cover worker unavailable");return -1;}
    int direct_available=direct_start();
    lab_log(s,"Direct FFmpeg backend available: %d",direct_available);
    if(lab_video_start(s)!=0){lab_log(s,"Video worker initialization failed");return -1;}
    raop_callbacks_t cb;memset(&cb,0,sizeof(cb));
    cb.audio_process=process;cb.audio_get_format=format;cb.audio_flush=flush;
    cb.audio_set_metadata=metadata;cb.audio_set_coverart=coverart;cb.audio_set_progress=progress;
    cb.audio_stop_coverart_rendering=music_stop;
    cb.on_protocol_trace=direct_protocol_trace;cb.on_video_protocol=direct_protocol;cb.on_video_play=direct_play;cb.on_video_play_fraction=direct_play_fraction;cb.on_video_scrub=direct_seek;
    cb.on_video_rate=direct_rate;cb.on_video_stop=direct_stop;cb.on_video_acquire_playback_info=direct_info;cb.on_video_playlist_remove=direct_remove;
    cb.audio_set_volume=volume;cb.audio_set_client_volume=initial_volume;
    cb.conn_init=noop;cb.conn_destroy=flush;cb.conn_reset=reset;cb.conn_feedback=noop;
    cb.mirror_video_running=mirror_running;cb.video_process=video;cb.video_pause=noop;cb.video_resume=noop;cb.video_flush=video_flush;
    cb.video_reset=video_reset;cb.video_set_codec_and_size=video_size;
    int error=0;
    receiver.dns=dnssd_init("PS4 AirPlay",11,(const char *)hw,6,0,&error);
    if (!receiver.dns || error) {lab_log(s,"mDNS init failed: %d",error);return -1;}
    for(int bit=0;bit<=8;++bit)dnssd_set_airplay_features(receiver.dns,bit,0);
    dnssd_set_airplay_features(receiver.dns,2,1); /* FairPlay transport */
    dnssd_set_airplay_features(receiver.dns,7,1); /* screen mirroring */
    dnssd_set_airplay_features(receiver.dns,42,0); /* H264 only, no HEVC */
    dnssd_set_airplay_features(receiver.dns,0,direct_available);
    dnssd_set_airplay_features(receiver.dns,4,direct_available);
    dnssd_set_airplay_features(receiver.dns,15,1);
    dnssd_set_airplay_features(receiver.dns,16,1);
    dnssd_set_airplay_features(receiver.dns,17,1);
    receiver.raop=raop_init(&cb);
    if(!receiver.raop){lab_log(s,"AirPlay core allocation failed");return -1;}
    raop_set_log_callback(receiver.raop,log_callback,NULL);raop_set_log_level(receiver.raop,4);
    if(raop_init2(receiver.raop,0,device,"/data/ps4-receiver/airplay-key.pem")){lab_log(s,"AirPlay key/session initialization failed");return -1;}
    const LabVideoProfile *profile=lab_video_profile(s->video_profile);
    raop_set_plist(receiver.raop,"width",(int)profile->width);
    raop_set_plist(receiver.raop,"height",(int)profile->height);
    raop_set_plist(receiver.raop,"maxFPS",(int)profile->fps);
    raop_set_plist(receiver.raop,"refreshRate",60);
    raop_set_plist(receiver.raop,"hls",direct_available);
    raop_set_lang(receiver.raop,NULL,NULL,"ru:en");
    raop_set_dnssd(receiver.raop,receiver.dns);
        break;
    }
    case 5: {
    receiver.port=7000;
    if(raop_start_httpd(receiver.raop,&receiver.port)<0){lab_log(s,"AirPlay protocol listener failed");return -1;}
    raop_set_port(receiver.raop,receiver.port);
    receiver.running=1;
    if(lab_airplay_thread_create(&receiver.worker,audio_worker,NULL)){receiver.running=0;lab_log(s,"Music worker thread creation failed");return -1;}
    receiver.worker_started=1;
        break;
    }
    case 6: {
    int a=dnssd_register_raop(receiver.dns,receiver.port),b=dnssd_register_airplay(receiver.dns,receiver.port);
    lab_log(s,"AirPlay protocol listening on %u; mDNS RAOP=%d AirPlay=%d",receiver.port,a,b);
    lab_log(s,"mDNS stage: %s",mdnsd_last_stage());
    if(a || b)return -1;
        break;
    }
    }
    startup_phase=phase;
    snprintf(message,sizeof(message),"DONE step %d",phase);checkpoint(s,message);
    return phase==6?0:1;
}

void lab_airplay_poll(LabState *s) {
    if(!receiver.worker_started)return;
    static unsigned long last_event=0,last_frames=0;
    pthread_mutex_lock(&receiver.mutex);
    if(receiver.event_id!=last_event){last_event=receiver.event_id;lab_log(s,"%s",receiver.message);}
    if(receiver.decoded-last_frames>=48000){last_frames=receiver.decoded;lab_log(s,"Music: packets=%lu frames=%lu drops=%lu errors=%lu",receiver.packets,receiver.decoded,receiver.dropped,receiver.errors);}
    s->audio_packets=receiver.packets;s->decoded_frames=receiver.decoded;s->audio_errors=receiver.errors;
    if(receiver.output_started)s->audio_rc=0;
    if(receiver.output_error){s->airplay_started=0;s->audio_rc=receiver.output_error;}
    pthread_mutex_unlock(&receiver.mutex);
}
void lab_airplay_stop(void) {
    if(receiver.dns){dnssd_unregister_raop(receiver.dns);dnssd_unregister_airplay(receiver.dns);}
    /* Quiesce callbacks first; cancel/join the player while its HTTP core and
     * FCUP playlists still exist. Only then free protocol/session storage. */
    if(receiver.raop)raop_stop_httpd(receiver.raop);
    direct_shutdown();
    if(receiver.raop){raop_destroy(receiver.raop);receiver.raop=NULL;}
    pthread_mutex_lock(&receiver.mutex);receiver.running=0;pthread_mutex_unlock(&receiver.mutex);
    if(receiver.worker_started){pthread_join(receiver.worker,NULL);receiver.worker_started=0;}
    if(receiver.dns){dnssd_destroy(receiver.dns);receiver.dns=NULL;}
    decoder_close();
    np_shutdown();
    lab_video_stop();
    startup_phase=0;
}
