/* Host end-to-end direct player: real MP4/HLS demux, H264/AAC decoders, curl IO. */
#include "direct_player.h"
#include "media_io.h"
#include <curl/curl.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
void direct_test_cleanup(unsigned long minimum);
static LabState snapshot(void){LabState s={0};direct_poll(&s);return s;}
#define WAIT(condition) do{int success=0;for(int i=0;i<5000;++i){if(condition){success=1;break;}usleep(2000);}if(!success){LabState s=snapshot();fprintf(stderr,"Timeout stage=%s error=%d %s frames=%lu audio=%lu\n",s.direct_stage,s.direct_error,s.direct_native_log,s.direct_video_frames,s.direct_audio_frames);}assert(success);}while(0)
int main(int argc,char **argv){assert(argc==2);const char *test_ca=getenv("PS4_TEST_CA");if(test_ca){FILE *f=fopen(test_ca,"rb");assert(f);char pem[10000];size_t size=fread(pem,1,sizeof(pem),f);fclose(f);assert(size && !tls_http_test_ca(pem,size));}assert(direct_start());if(getenv("PS4_EXPECT_TLS_FAIL")){char failure_url[1000];snprintf(failure_url,sizeof(failure_url),"%s/av-test.mp4?secret=synthetic",argv[1]);direct_play(NULL,failure_url,0);WAIT(snapshot().direct_errors>0);assert(strstr(snapshot().direct_native_log,"tls-verify"));assert(!strstr(snapshot().direct_native_log,"secret"));direct_shutdown();direct_test_cleanup(0);puts("Direct TLS refusal: useful verification error without URL query/path leakage passed");return 0;}for(int i=0;i<200;++i)direct_protocol_trace(NULL,"HTTP GET /server-info -> 200");direct_protocol_trace(NULL,"HTTP POST /play -> 200");assert(strlen(snapshot().direct_protocol_log)<2048);assert(strstr(snapshot().direct_protocol_log,"HTTP POST /play -> 200"));char url[1000];snprintf(url,sizeof(url),"%s/av-test.mp4",argv[1]);direct_play(NULL,url,0);WAIT(snapshot().direct_video_frames>=15 && snapshot().direct_audio_frames>=4096);assert(snapshot().direct_errors==0);
 direct_rate(NULL,0);WAIT(snapshot().direct_active && !strcmp(snapshot().direct_stage,"paused"));double paused=snapshot().direct_position;usleep(100000);assert(fabs(snapshot().direct_position-paused)<0.1);
 direct_seek(NULL,6);WAIT(snapshot().direct_position>=6);direct_rate(NULL,1);WAIT(!strcmp(snapshot().direct_stage,"playing"));unsigned long before=snapshot().direct_video_frames;WAIT(snapshot().direct_video_frames>before+10);
 uint32_t *pixels=malloc(LAB_WIDTH*LAB_HEIGHT*4);assert(pixels);assert(direct_draw(pixels));assert(pixels[LAB_WIDTH*500+900]!=0);free(pixels);direct_stop(NULL);WAIT(!snapshot().direct_active);
 unsigned long videos=snapshot().direct_video_frames,audios=snapshot().direct_audio_frames;
 snprintf(url,sizeof(url),"%s/av-master.m3u8",argv[1]);direct_play(NULL,url,0);WAIT(snapshot().direct_video_frames>=videos+15 && snapshot().direct_audio_frames>=audios+4096);assert(snapshot().direct_errors==0);direct_stop(NULL);WAIT(!snapshot().direct_active);snprintf(url,sizeof(url),"%s/av-single.m3u8",argv[1]);videos=snapshot().direct_video_frames;audios=snapshot().direct_audio_frames;direct_play(NULL,url,0);WAIT(snapshot().direct_video_frames>=videos+15 && snapshot().direct_audio_frames>=audios+4096);assert(snapshot().direct_errors==0);direct_stop(NULL);WAIT(!snapshot().direct_active);direct_shutdown();
 direct_test_cleanup(8193);puts("Direct FFmpeg: real HTTP MP4 and master/media HLS, fMP4 byte ranges, H264+AAC, PCM, RGB frame, pause/resume, seek, stop, IO cleanup passed");return 0;}
