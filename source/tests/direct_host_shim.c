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
static struct {int used;char url[8193],range[100];char *headers;size_t header_size;unsigned char *data;size_t size,position;long status;} requests[64];
static pthread_mutex_t net_mutex=PTHREAD_MUTEX_INITIALIZER;
static atomic_ulong pcm_count;
static unsigned queued;static uint64_t last_tick;
static pthread_mutex_t audio_mutex=PTHREAD_MUTEX_INITIALIZER;
static uint64_t clock_us(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
int lab_airplay_thread_create(pthread_t *t,void *(*f)(void *),void *a){return pthread_create(t,NULL,f,a);}
void lab_session_trace(const char *a,const char *b){(void)a;(void)b;}
void lab_log(LabState *s,const char *f,...){(void)s;(void)f;}
void lab_audio_direct_mode(int mode){(void)mode;pthread_mutex_lock(&audio_mutex);queued=0;last_tick=clock_us();pthread_mutex_unlock(&audio_mutex);}
unsigned lab_audio_direct_space(void){pthread_mutex_lock(&audio_mutex);uint64_t now=clock_us();unsigned drained=(unsigned)((now-last_tick)*48000/1000000);if(drained){queued=drained>=queued?0:queued-drained;last_tick=now;}unsigned result=queued<=1024?48000-queued:0;pthread_mutex_unlock(&audio_mutex);return result;}
void lab_audio_direct_pcm(const int16_t *pcm,unsigned frames){assert(pcm && frames<16384);pthread_mutex_lock(&audio_mutex);queued+=frames;pthread_mutex_unlock(&audio_mutex);atomic_fetch_add(&pcm_count,frames);}
static int create_template(int c,const char *ua,int version,int proxy){assert(c==1 && ua && version==2 && proxy==1);return 1;}
static int create_connection(int t,const char *url,int keep){assert(t==1 && url && keep);return 2;}
static int create_request(int c,int method,const char *url,uint64_t n){assert(c==2 && method==0 && !n);pthread_mutex_lock(&net_mutex);int id=-1;for(int i=0;i<64;++i)if(!requests[i].used){id=i;requests[i].used=1;snprintf(requests[i].url,sizeof(requests[i].url),"%s",url);break;}pthread_mutex_unlock(&net_mutex);assert(id>=0);return id;}
static int add_header(int id,const char *key,const char *value,int mode){assert(!strcmp(key,"Range") && mode==0 && !strncmp(value,"bytes=",6));snprintf(requests[id].range,sizeof(requests[id].range),"%s",value+6);return 0;}
static size_t body(void *data,size_t a,size_t b,void *user){int i=*(int *)user;size_t n=a*b;void *p=realloc(requests[i].data,requests[i].size+n);assert(p);requests[i].data=p;memcpy(requests[i].data+requests[i].size,data,n);requests[i].size+=n;return n;}
static size_t headers(void *data,size_t a,size_t b,void *user){int i=*(int *)user;size_t n=a*b;void *p=realloc(requests[i].headers,requests[i].header_size+n+1);assert(p);requests[i].headers=p;memcpy(requests[i].headers+requests[i].header_size,data,n);requests[i].header_size+=n;requests[i].headers[requests[i].header_size]=0;return n;}
static int send_request(int id,const void *data,size_t n){assert(!data && !n);CURL *c=curl_easy_init();assert(c);curl_easy_setopt(c,CURLOPT_URL,requests[id].url);curl_easy_setopt(c,CURLOPT_RANGE,requests[id].range);curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);curl_easy_setopt(c,CURLOPT_NOPROXY,"*");curl_easy_setopt(c,CURLOPT_TIMEOUT,5L);curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,body);curl_easy_setopt(c,CURLOPT_WRITEDATA,&id);curl_easy_setopt(c,CURLOPT_HEADERFUNCTION,headers);curl_easy_setopt(c,CURLOPT_HEADERDATA,&id);CURLcode rc=curl_easy_perform(c);curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&requests[id].status);curl_easy_cleanup(c);return rc==CURLE_OK?0:-5;}
static int status_code(int i,int *code){*code=(int)requests[i].status;return 0;}
static int content_length(int i,int *kind,size_t *length){*kind=0;*length=requests[i].size;return 0;}
static int all_headers(int i,char **out,size_t *size){*out=requests[i].headers;*size=requests[i].header_size;return 0;}
static int read_data(int i,void *out,uint32_t n){size_t left=requests[i].size-requests[i].position;if(n>left)n=(uint32_t)left;memcpy(out,requests[i].data+requests[i].position,n);requests[i].position+=n;return (int)n;}
static int delete_request(int i){pthread_mutex_lock(&net_mutex);free(requests[i].data);free(requests[i].headers);memset(&requests[i],0,sizeof(requests[i]));pthread_mutex_unlock(&net_mutex);return 0;}
static int delete_other(int i){(void)i;return 0;}
int direct_host_http(MediaHttp *api){curl_global_init(CURL_GLOBAL_DEFAULT);*api=(MediaHttp){create_template,create_connection,create_request,add_header,send_request,status_code,content_length,all_headers,read_data,delete_request,delete_other,delete_other,1};return 0;}

void direct_test_cleanup(unsigned long minimum){for(int i=0;i<64;++i)assert(!requests[i].used);assert(pcm_count>=minimum);curl_global_cleanup();}
