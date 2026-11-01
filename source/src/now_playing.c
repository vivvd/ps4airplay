/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "now_playing.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#ifdef LAB_PS4
#include <orbis/libkernel.h>
#endif
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_MAX_DIMENSIONS 2048
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
extern int lab_airplay_thread_create(pthread_t *,void *(*)(void *),void *);
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    pthread_t worker;
    int running,started;
    NowPlaying value;
    unsigned char *pending;
    int pending_size;
    unsigned long cover_serial,metadata_count,cover_count,errors;
    uint32_t *art;
    unsigned rate;
    uint32_t start,current,end;
    uint64_t progress_time,last_audio;
} np={.mutex=PTHREAD_MUTEX_INITIALIZER,.wake=PTHREAD_COND_INITIALIZER,.rate=44100};
static uint64_t clock_us(void) {
#ifdef LAB_PS4
    return sceKernelGetProcessTime();
#else
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
#endif
}
static uint32_t be32(const unsigned char *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void text_copy(char *out,size_t capacity,const unsigned char *in,size_t size) {
    size_t n=size<capacity-1?size:capacity-1;
    if(n<size)while(n && (in[n]&0xc0)==0x80)--n;
    for(size_t i=0;i<n;++i)out[i]=in[i]<32?' ':in[i];
    out[n]=0;
}
static int parse_dmap(NowPlaying *out,const unsigned char *data,size_t size,unsigned depth,int *fields) {
    if(depth>8)return -1;
    for(size_t i=0;i<size;){
        if(size-i<8)return -1;
        const unsigned char *tag=data+i;size_t length=be32(tag+4);i+=8;
        if(length>size-i)return -1;
        const unsigned char *value=data+i;
        if(!memcmp(tag,"minm",4)){text_copy(out->title,sizeof(out->title),value,length);++*fields;}
        else if(!memcmp(tag,"asar",4)){text_copy(out->artist,sizeof(out->artist),value,length);++*fields;}
        else if(!memcmp(tag,"asal",4)){text_copy(out->album,sizeof(out->album),value,length);++*fields;}
        else if(!memcmp(tag,"asaa",4) && !out->artist[0]){text_copy(out->artist,sizeof(out->artist),value,length);++*fields;}
        else if(!memcmp(tag,"astm",4) && length==4){out->duration=be32(value)/1000.0;++*fields;}
        else if(!memcmp(tag,"mlit",4) || !memcmp(tag,"mlcl",4) || !memcmp(tag,"mdcl",4)){
            if(parse_dmap(out,value,length,depth+1,fields)<0)return -1;
        }
        i+=length;
    }
    return 0;
}
void np_metadata(const void *data,int size) {
    if(!data || size<=0 || size>65536)return;
    NowPlaying parsed={0};int fields=0;
    int valid=parse_dmap(&parsed,data,(size_t)size,0,&fields)==0;
    pthread_mutex_lock(&np.mutex);
    if(!valid){++np.errors;pthread_mutex_unlock(&np.mutex);return;}
    if(fields){
        /* A DMAP update may omit artwork; preserve the previous album cover
         * until a new cover arrives (important for adjacent tracks on one album). */
        if(parsed.title[0])memcpy(np.value.title,parsed.title,sizeof(parsed.title));
        if(parsed.artist[0])memcpy(np.value.artist,parsed.artist,sizeof(parsed.artist));
        if(parsed.album[0])memcpy(np.value.album,parsed.album,sizeof(parsed.album));
        if(parsed.duration>0)np.value.duration=parsed.duration;
        np.value.active=1;++np.value.revision;++np.metadata_count;
    }
    pthread_mutex_unlock(&np.mutex);
}
void np_coverart(const void *data,int size) {
    if(!data || size<=0 || size>4*1024*1024)return;
    unsigned char *copy=malloc((size_t)size);if(!copy)return;
    memcpy(copy,data,(size_t)size);
    pthread_mutex_lock(&np.mutex);
    free(np.pending);np.pending=copy;np.pending_size=size;++np.cover_serial;
    pthread_cond_signal(&np.wake);pthread_mutex_unlock(&np.mutex);
}
static uint32_t *decode_art(const unsigned char *data,int size) {
    int width=0,height=0,channels=0;
    if(!stbi_info_from_memory(data,size,&width,&height,&channels) || width<1 || height<1 ||
       width>2048 || height>2048 || (int64_t)width*height>2048*2048)return NULL;
    unsigned char *rgba=stbi_load_from_memory(data,size,&width,&height,&channels,4);
    if(!rgba)return NULL;
    uint32_t *out=malloc(NP_ART_SIZE*NP_ART_SIZE*4);
    if(out){
        int side=width<height?width:height,left=(width-side)/2,top=(height-side)/2;
        for(int y=0;y<NP_ART_SIZE;++y)for(int x=0;x<NP_ART_SIZE;++x){
            float fx=left+(x+0.5f)*side/NP_ART_SIZE-0.5f;
            float fy=top+(y+0.5f)*side/NP_ART_SIZE-0.5f;
            int ix=(int)floorf(fx),iy=(int)floorf(fy);float ax=fx-ix,ay=fy-iy;
            if(ix<0){ix=0;ax=0;}if(iy<0){iy=0;ay=0;}
            int nx=ix+1<width?ix+1:ix,ny=iy+1<height?iy+1:iy;
            const unsigned char *a=rgba+(iy*width+ix)*4,*b=rgba+(iy*width+nx)*4;
            const unsigned char *c=rgba+(ny*width+ix)*4,*d=rgba+(ny*width+nx)*4;
            uint32_t pixel=0xff000000;
            for(int k=0;k<3;++k){
                float v=(1-ay)*((1-ax)*a[k]+ax*b[k])+ay*((1-ax)*c[k]+ax*d[k]);
                pixel|=(uint32_t)(v+0.5f)<<(16-8*k);
            }
            out[y*NP_ART_SIZE+x]=pixel;
        }
    }
    stbi_image_free(rgba);return out;
}
static void *cover_worker(void *unused) {
    (void)unused;
    for(;;){
        pthread_mutex_lock(&np.mutex);
        while(np.running && !np.pending)pthread_cond_wait(&np.wake,&np.mutex);
        if(!np.running){pthread_mutex_unlock(&np.mutex);break;}
        unsigned char *data=np.pending;int size=np.pending_size;unsigned long serial=np.cover_serial;
        np.pending=NULL;pthread_mutex_unlock(&np.mutex);
        uint32_t *art=decode_art(data,size);free(data);
        pthread_mutex_lock(&np.mutex);
        if(serial==np.cover_serial){
            if(art){free(np.art);np.art=art;art=NULL;np.value.has_art=1;++np.value.art_revision;++np.value.revision;++np.cover_count;}
            else ++np.errors;
        }
        pthread_mutex_unlock(&np.mutex);free(art);
    }
    return NULL;
}
int np_start(void) {
    if(np.started)return 0;
    np.running=1;
    if(lab_airplay_thread_create(&np.worker,cover_worker,NULL)){np.running=0;return -1;}
    np.started=1;return 0;
}
void np_progress(uint32_t start,uint32_t current,uint32_t end) {
    pthread_mutex_lock(&np.mutex);
    np.start=start;np.current=current;np.end=end;np.progress_time=clock_us();
    pthread_mutex_unlock(&np.mutex);
}
void np_audio(unsigned rate) {
    pthread_mutex_lock(&np.mutex);
    if(rate>=8000 && rate<=192000)np.rate=rate;
    np.last_audio=clock_us();np.value.active=1;
    pthread_mutex_unlock(&np.mutex);
}
void np_stop(void) {pthread_mutex_lock(&np.mutex);np.value.active=0;++np.value.revision;pthread_mutex_unlock(&np.mutex);}
void np_snapshot(NowPlaying *out,uint32_t *art,unsigned long *art_revision) {
    pthread_mutex_lock(&np.mutex);*out=np.value;
    uint64_t now=clock_us();out->playing=np.last_audio && now-np.last_audio<1500000;
    if(np.progress_time){
        uint32_t length=np.end-np.start,offset=np.current-np.start;
        if(length && length/(double)np.rate<24*3600)out->duration=length/(double)np.rate;
        double elapsed=0;
        if(out->playing)elapsed=(now-np.progress_time)/1000000.0;
        else if(np.last_audio>np.progress_time)elapsed=(np.last_audio-np.progress_time)/1000000.0;
        out->position=offset/(double)np.rate+elapsed;
        if(out->duration>0 && out->position>out->duration)out->position=out->duration;
    }
    if(art && art_revision && *art_revision!=np.value.art_revision && np.art){
        memcpy(art,np.art,NP_ART_SIZE*NP_ART_SIZE*4);*art_revision=np.value.art_revision;
    }
    pthread_mutex_unlock(&np.mutex);
}
void np_poll(LabState *s) {
    pthread_mutex_lock(&np.mutex);
    s->metadata_packets=np.metadata_count;s->cover_images=np.cover_count;s->metadata_errors=np.errors;
    pthread_mutex_unlock(&np.mutex);
}
void np_shutdown(void) {
    if(!np.started)return;
    pthread_mutex_lock(&np.mutex);np.running=0;pthread_cond_signal(&np.wake);pthread_mutex_unlock(&np.mutex);
    pthread_join(np.worker,NULL);free(np.pending);free(np.art);np.pending=NULL;np.art=NULL;np.started=0;
}
