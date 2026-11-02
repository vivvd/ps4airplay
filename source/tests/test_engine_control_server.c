/* Real FFmpeg engine behind the real AirPlay HTTP parser/handlers. */
#include "direct_player.h"
#include "dnssd.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
static volatile sig_atomic_t running=1;
static void halt(int s){(void)s;running=0;}
static void noop(void *p){(void)p;}
static void reset(void *p,int n){(void)p;(void)n;}
static void audio(void *p,raop_ntp_t *n,audio_decode_struct *d){(void)p;(void)n;(void)d;}
static void video(void *p,raop_ntp_t *n,video_decode_struct *d){(void)p;(void)n;(void)d;}
void direct_test_cleanup(unsigned long minimum);
int main(int argc,char **argv){assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);signal(SIGTERM,halt);signal(SIGPIPE,SIG_IGN);assert(direct_start());
 raop_callbacks_t cb={0};cb.audio_process=audio;cb.video_process=video;cb.conn_init=noop;cb.conn_destroy=noop;cb.conn_reset=reset;
 cb.on_video_play=direct_play;cb.on_video_play_fraction=direct_play_fraction;cb.on_video_scrub=direct_seek;cb.on_video_rate=direct_rate;cb.on_video_stop=direct_stop;cb.on_video_acquire_playback_info=direct_info;cb.on_video_playlist_remove=direct_remove;
 raop_t *r=raop_init(&cb);assert(r);raop_set_log_level(r,0);assert(!raop_init2(r,0,"02:00:00:00:00:01","build/release/test-engine-key.pem"));
 unsigned char hw[6]={2,0,0,0,0,1};int error=0;dnssd_t *dns=dnssd_init("Test",4,(const char *)hw,6,0,&error);assert(dns && !error);raop_set_dnssd(r,dns);raop_set_plist(r,"hls",1);
 unsigned short port=(unsigned short)atoi(argv[1]);assert(raop_start_httpd(r,&port)>=0);printf("READY %u\n",port);
 while(running)usleep(10000);raop_stop_httpd(r);direct_shutdown();raop_destroy(r);dnssd_destroy(dns);direct_test_cleanup(0);return 0;
}
