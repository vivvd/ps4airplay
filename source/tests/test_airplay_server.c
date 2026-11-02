/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "raop.h"
#include "dnssd.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static volatile sig_atomic_t running=1;
static void halt(int s){(void)s;running=0;}
static void logger(void *p,int level,const char *text){(void)p;(void)level;fprintf(stderr,"%s\n",text);}
static void trace(void *p,const char *label){(void)p;printf("PROTOCOL %s\n",label);}
static void noop(void *p){(void)p;}
static void reset(void *p,int n){(void)p;(void)n;}
static void audio(void *p,raop_ntp_t *n,audio_decode_struct *d){(void)p;(void)n;(void)d;}
static void video(void *p,raop_ntp_t *n,video_decode_struct *d){(void)p;(void)n;(void)d;}
static void play(void *p,const char *url,float seconds){(void)p;printf("PLAY %.3f %s\n",seconds,url);}
static void fraction(void *p,const char *url,float f){(void)p;printf("FRACTION %.3f %s\n",f,url);}
static void seek(void *p,float s){(void)p;printf("SEEK %.3f\n",s);}
static void rate(void *p,float r){(void)p;printf("RATE %.3f\n",r);}
static void stop(void *p){(void)p;puts("STOP");}
static float remove_video(void *p){(void)p;return 12;}
static void info(void *p,playback_info_t *i){(void)p;i->duration=120;i->position=12;i->rate=1;i->ready_to_play=true;i->playback_buffer_empty=false;i->playback_buffer_full=false;i->playback_likely_to_keep_up=true;i->seek_start=0;i->seek_duration=120;}
int main(int argc,char **argv){assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);signal(SIGTERM,halt);signal(SIGPIPE,SIG_IGN);
 raop_callbacks_t cb={0};cb.on_protocol_trace=trace;cb.audio_process=audio;cb.video_process=video;cb.conn_init=noop;cb.conn_destroy=noop;cb.conn_reset=reset;
 cb.on_video_play=play;cb.on_video_play_fraction=fraction;cb.on_video_scrub=seek;cb.on_video_rate=rate;cb.on_video_stop=stop;cb.on_video_playlist_remove=remove_video;cb.on_video_acquire_playback_info=info;
 raop_t *r=raop_init(&cb);assert(r);raop_set_log_callback(r,logger,NULL);raop_set_log_level(r,3);assert(raop_init2(r,0,"02:00:00:00:00:01","build/release/test-key.pem")==0);
 unsigned char hw[6]={2,0,0,0,0,1};int error=0;dnssd_t *dns=dnssd_init("Test",4,(const char *)hw,6,0,&error);assert(dns && !error);raop_set_dnssd(r,dns);raop_set_plist(r,"hls",1);
 unsigned short port=(unsigned short)atoi(argv[1]);assert(raop_start_httpd(r,&port)>=0);raop_set_port(r,port);printf("READY %u\n",port);
 while(running)usleep(10000);raop_destroy(r);dnssd_destroy(dns);return 0;
}
