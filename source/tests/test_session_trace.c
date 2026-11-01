/* Host regression of durable checkpoints, redaction, rotation and thread stack. */
#include "lab.h"
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
extern int lab_airplay_thread_create(pthread_t *,void *(*)(void *),void *);
static void *worker(void *arg) {
    (void)arg;
    volatile unsigned char stack[256*1024];
    for(size_t i=0;i<sizeof(stack);i+=4096)stack[i]=42;
    for(size_t i=0;i<sizeof(stack);i+=4096)assert(stack[i]==42);
#ifdef __APPLE__
    assert(pthread_get_stacksize_np(pthread_self())>=2*1024*1024);
#endif
    for(int i=0;i<40;++i)lab_session_trace("thread","checkpoint");
    return NULL;
}
int main(void) {
    assert(lab_airplay_signals_init()==0);
    int sockets[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    close(sockets[1]);
    assert(send(sockets[0],"x",1,0)==-1 && errno==EPIPE);
    close(sockets[0]); /* process must survive writing to a closed peer */
    remove(LAB_TRACE_PATH);
    LabState s;lab_init(&s,"trace-test");lab_session_trace_restore(&s);
    lab_session_trace_request("POST","/pair-verify");
    lab_session_trace_request("secret-method","/unknown?private-key=SECRET");
    pthread_t a,b;
    assert(lab_airplay_thread_create(&a,worker,NULL)==0);
    assert(lab_airplay_thread_create(&b,worker,NULL)==0);
    pthread_join(a,NULL);pthread_join(b,NULL);
    FILE *f=fopen(LAB_TRACE_PATH,"rb");assert(f);
    char content[12000];size_t n=fread(content,1,sizeof(content)-1,f);content[n]=0;fclose(f);
    assert(strstr(content,"/pair-verify"));assert(!strstr(content,"SECRET"));
    assert(!strstr(content,"secret-method"));
    struct stat st;assert(stat(LAB_TRACE_PATH,&st)==0);assert((st.st_mode&0777)==0600);
    lab_session_trace_restore(&s);
    assert(strstr(s.log,"Previous AirPlay session checkpoints:"));
    assert(strstr(s.log,"thread: checkpoint"));
    assert(s.recovery_mode==1);
    assert(stat(LAB_TRACE_PATH,&st)==0 && st.st_size>0);
    off_t retained=st.st_size;
    lab_session_trace("should not append","recovery is read only");
    assert(stat(LAB_TRACE_PATH,&st)==0 && st.st_size==retained);
    assert(lab_session_trace_begin()==0);
    assert(stat(LAB_TRACE_PATH,&st)==0 && st.st_size>0 && st.st_size<80);
    lab_session_trace("session","clean stop");
    lab_session_trace_restore(&s);assert(!s.recovery_mode);
    assert(stat(LAB_TRACE_PATH,&st)==0 && st.st_size>0 && st.st_size<80);
    for(int i=0;i<10000;++i)lab_session_trace("limit","bounded log");
    assert(stat(LAB_TRACE_PATH,&st)==0 && st.st_size<65800);
    puts("Session persistence, redaction, rotation, cap and 2 MiB stack tests passed");
    f=fopen(LAB_TRACE_PATH,"wb");assert(f);
    fputs("old release interrupted checkpoint\n",f);fclose(f);
    lab_session_trace_restore(&s);assert(!s.recovery_mode);
    assert(strstr(s.log,"old release interrupted checkpoint"));
    remove(LAB_TRACE_PATH);return 0;
}
