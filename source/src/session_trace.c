/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lab.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef LAB_PS4
#include <orbis/libkernel.h>
#define trace_open sceKernelOpen
#define trace_write sceKernelWrite
#define trace_close sceKernelClose
#else
#define trace_open open
#define trace_write write
#define trace_close close
#endif
#ifndef LAB_TRACE_PATH
#define LAB_TRACE_PATH "/data/ps4-receiver/session.log"
#endif
static pthread_mutex_t trace_mutex=PTHREAD_MUTEX_INITIALIZER;
static size_t written;
static int enabled;

void lab_session_trace(const char *phase,const char *detail) {
    pthread_mutex_lock(&trace_mutex);
    if(enabled && written<65536){
        char line[240];
        int n=snprintf(line,sizeof(line),"%.48s: %.160s\n",phase?phase:"?",detail?detail:"");
        if(n>0 && n<(int)sizeof(line)){
            int fd=trace_open(LAB_TRACE_PATH,O_WRONLY|O_APPEND,0600);
            if(fd>=0){
                size_t offset=0;
                while(offset<(size_t)n){
                    ssize_t rc=trace_write(fd,line+offset,(size_t)n-offset);
                    if(rc<=0)break;
                    offset+=(size_t)rc;
                }
                written+=offset;trace_close(fd);
            }
        }
    }
    pthread_mutex_unlock(&trace_mutex);
}

void lab_session_trace_request(const char *method,const char *url) {
    /* Only fixed protocol labels: never headers, bodies, keys, PINs or URLs
     * supplied by a client. Unknown requests get a generic label. */
    const char *methods[]={"GET","POST","OPTIONS","SETUP","RECORD","FLUSH","TEARDOWN","GET_PARAMETER","SET_PARAMETER"};
    const char *routes[]={"/info","/pair-pin-start","/pair-setup-pin","/pair-setup","/pair-verify","/fp-setup","/feedback","/audioMode"};
    const char *m="other",*r="other";
    for(size_t i=0;i<sizeof(methods)/sizeof(*methods);++i)if(method && !strcmp(method,methods[i]))m=methods[i];
    for(size_t i=0;i<sizeof(routes)/sizeof(*routes);++i)if(url && !strcmp(url,routes[i]))r=routes[i];
    lab_session_trace("request method",m);lab_session_trace("request route",r);
}

int lab_session_trace_begin(void) {
    pthread_mutex_lock(&trace_mutex);
    int fd=trace_open(LAB_TRACE_PATH,O_WRONLY|O_CREAT|O_TRUNC,0600);
    enabled=fd>=0;written=0;
    if(fd>=0){
        const char marker[]="session version: " LAB_VERSION "\n";
        ssize_t n=trace_write(fd,marker,sizeof(marker)-1);
        enabled=n==(ssize_t)(sizeof(marker)-1);
        if(n>0)written=(size_t)n;
        trace_close(fd);
    }
    pthread_mutex_unlock(&trace_mutex);
    return enabled ? 0 : -1;
}

void lab_session_trace_restore(LabState *s) {
#ifdef LAB_PS4
    mkdir("/data/ps4-receiver",0700);
#endif
    pthread_mutex_lock(&trace_mutex);
    int interrupted=0;
    FILE *f=fopen(LAB_TRACE_PATH,"rb");
    if(f){
        const char marker[]="session version: " LAB_VERSION "\n";
        char head[sizeof(marker)]={0};
        size_t head_size=fread(head,1,sizeof(marker)-1,f);
        int same_version=head_size==sizeof(marker)-1 && !memcmp(head,marker,sizeof(marker)-1);
        char data[4097];fseek(f,0,SEEK_END);long size=ftell(f);
        if(size>4096)fseek(f,size-4096,SEEK_SET);else rewind(f);
        size_t n=fread(data,1,sizeof(data)-1,f);data[n]=0;fclose(f);
        if(n){
            const char *clean="session: clean stop\n";
            size_t clean_size=strlen(clean);
            interrupted=same_version && (n<clean_size || memcmp(data+n-clean_size,clean,clean_size)!=0);
            lab_log(s,"Previous AirPlay session checkpoints:");
            char *save=NULL;
            for(char *line=strtok_r(data,"\n",&save);line;line=strtok_r(NULL,"\n",&save))lab_log(s,"%s",line);
        }
    }
    enabled=0;
    pthread_mutex_unlock(&trace_mutex);
    s->recovery_mode=interrupted;
    if(interrupted){
        lab_log(s,"Interrupted AirPlay session: automatic startup paused; download report first");
        return;
    }
    lab_session_trace_begin();
    lab_log(s,"Session checkpoints: %s",enabled?"enabled":"file unavailable");
}
