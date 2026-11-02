/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "media_io.h"
#include "../generated/test_video.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#ifdef LAB_PS4
#include <orbis/libkernel.h>
#endif
static void close_request(MediaFile *f){
 if(f->request>=0){f->api->request_delete(f->request);f->request=-1;}
}
static void close_all(MediaFile *f){
 if(f->secure){tls_http_close(f->secure);f->secure=NULL;}
 if(f->fd>=0){
#ifdef LAB_PS4
 sceKernelClose(f->fd);
#else
 close(f->fd);
#endif
 f->fd=-1;}
 if(f->api){close_request(f);if(f->connection>=0)f->api->connection_delete(f->connection);if(f->template_id>=0)f->api->template_delete(f->template_id);}
 f->request=f->connection=f->template_id=-1;f->position=f->length=0;
}
static void *io_worker(void *object);
void media_file_init(MediaFile *f,const MediaHttp *api,int context){
 memset(f,0,sizeof(*f));pthread_mutex_init(&f->mutex,NULL);f->api=api;f->context=context;f->fd=f->request=f->connection=f->template_id=-1;
 pthread_mutex_init(&f->command_mutex,NULL);pthread_cond_init(&f->wake,NULL);pthread_cond_init(&f->done,NULL);f->running=1;
 pthread_attr_t attr;pthread_attr_init(&attr);int rc=pthread_attr_setstacksize(&attr,2*1024*1024);if(!rc)rc=pthread_create(&f->worker,&attr,io_worker,f);pthread_attr_destroy(&attr);f->worker_started=rc==0;if(rc)f->error=-rc;
}
static int range_total(MediaFile *f,uint64_t expected){
 char *headers=NULL;size_t size=0;
 if(f->api->headers(f->request,&headers,&size)<0 || !headers || size>65536)return -1;
 for(size_t i=0;i+14<size;++i)if(!strncasecmp(headers+i,"Content-Range:",14)){
  char line[160];size_t n=0;while(i+n<size && n<sizeof(line)-1 && headers[i+n]!='\r' && headers[i+n]!='\n'){line[n]=headers[i+n];++n;}line[n]=0;
  unsigned long long start,end,total;
  if(sscanf(line+14," bytes %llu-%llu/%llu",&start,&end,&total)!=3 || start!=expected || end<start || total<=end)return -1;
  f->length=total;return 0;
 }
 return -1;
}
static void io_phase(MediaFile *f,const char *phase){snprintf(f->diagnostics.phase,sizeof(f->diagnostics.phase),"%s",phase);}
static int secure_at(MediaFile *f,uint64_t position){
 tls_http_close(f->secure);f->secure=NULL;
 int rc=tls_http_open(&f->secure,f->url,position,&f->diagnostics);
 if(rc<0)return rc;f->length=tls_http_size(f->secure);f->position=position;return 0;
}
static int request_at(MediaFile *f,uint64_t position){
 io_phase(f,"http-request");close_request(f);int rc=f->api->request_create(f->connection,0,f->url,0);if(rc<0)return rc;f->request=rc;
 char value[80];snprintf(value,sizeof(value),"bytes=%llu-",(unsigned long long)position);
 if((rc=f->api->header_add(f->request,"Range",value,0))<0)return rc;io_phase(f,"http-send");if((rc=f->api->send(f->request,NULL,0))<0)return rc;
 io_phase(f,"http-status");int status=0;if((rc=f->api->status(f->request,&status))<0)return rc;f->diagnostics.status=status;
 if(status==206){if(range_total(f,position)<0)return -EINVAL;}
 else if(status==200 && position==0){int kind=0;size_t length=0;rc=f->api->length(f->request,&kind,&length);if(rc<0 && (uint32_t)rc!=0x80431071u && (uint32_t)rc!=0x80431072u)return rc;if(rc<0 || kind!=0)f->length=UINT64_MAX;else{if(!length)return -EINVAL;f->length=length;}}
 else return -EIO;
 f->position=position;return 0;
}
static int io_open(void *object,const char *name){
 MediaFile *f=object;if(!f || !name || strnlen(name,8193)>8192)return -EINVAL;
 pthread_mutex_lock(&f->mutex);close_all(f);f->error=0;f->reads=f->bytes=0;snprintf(f->url,sizeof(f->url),"%s",name);memset(&f->diagnostics,0,sizeof(f->diagnostics));media_url_origin(name,f->diagnostics.origin,sizeof(f->diagnostics.origin));
 f->is_embedded=!strcmp(name,"/app0/airplay-test.mp4");
 if(f->is_embedded){f->length=sizeof(airplay_test_video);pthread_mutex_unlock(&f->mutex);return 0;}
 f->is_http=!strncmp(name,"http://",7) || !strncmp(name,"https://",8);
 int rc=0;
 if(f->is_http){
  if(!f->api || f->context<0){rc=-EINVAL;goto done;}
  if(f->api->verified_tls && !strncmp(name,"https://",8)){rc=secure_at(f,0);goto done;}
  io_phase(f,"http-template");rc=f->api->template_create(f->context,"PS4AirPlay/1.03",2,1);if(rc<0)goto done;f->template_id=rc;
  io_phase(f,"http-connect");rc=f->api->connection_create(f->template_id,name,1);if(rc<0)goto done;f->connection=rc;
  rc=request_at(f,0);
 }else{
  io_phase(f,"file-open");
#ifdef LAB_PS4
  f->fd=sceKernelOpen(name,O_RDONLY,0);if(f->fd<0){rc=f->fd;goto done;}
  off_t end=sceKernelLseek(f->fd,0,SEEK_END);
#else
  f->fd=open(name,O_RDONLY);if(f->fd<0){rc=-errno;goto done;}off_t end=lseek(f->fd,0,SEEK_END);
#endif
  if(end<=0){rc=-EIO;goto done;}f->length=(uint64_t)end;
 }
done:
 if(rc<0){f->error=rc;close_all(f);}pthread_mutex_unlock(&f->mutex);return rc<0?rc:0;
}
static int io_close(void *object){MediaFile *f=object;if(!f)return -EINVAL;pthread_mutex_lock(&f->mutex);close_all(f);pthread_mutex_unlock(&f->mutex);return 0;}
static uint64_t io_size(void *object){MediaFile *f=object;pthread_mutex_lock(&f->mutex);uint64_t size=f->length;pthread_mutex_unlock(&f->mutex);return size;}
static int io_read(void *object,uint8_t *data,uint64_t position,uint32_t length){
 MediaFile *f=object;if(!f || !data || length>32*1024*1024 || position>INT64_MAX)return -EINVAL;
 pthread_mutex_lock(&f->mutex);++f->reads;int rc=0;
 if(position>=f->length)goto done;
 if(length>f->length-position)length=(uint32_t)(f->length-position);
 if(f->is_embedded){memcpy(data,airplay_test_video+position,length);rc=(int)length;}
 else if(f->secure){
  if(position!=f->position && (rc=secure_at(f,position))<0)goto done;
  uint32_t total=0;
  while(total<length){int n=tls_http_read(f->secure,data+total,length-total,&f->diagnostics);if(n<0){rc=n;goto done;}if(!n)break;total+=(unsigned)n;f->position+=(unsigned)n;}
  rc=(int)total;
 }
 else if(f->is_http){
  if(position!=f->position && (rc=request_at(f,position))<0)goto done;
  io_phase(f,"http-read");uint32_t total=0;
  while(total<length){int n=f->api->read(f->request,data+total,length-total);if(n<0){rc=n;goto done;}if(!n)break;if((uint32_t)n>length-total){rc=-EIO;goto done;}total+=(uint32_t)n;f->position+=(uint32_t)n;}
  rc=(int)total;
 }else{
#ifdef LAB_PS4
  size_t n=sceKernelPread(f->fd,data,length,(off_t)position);rc=n<=length?(int)n:(int32_t)(uint32_t)n;
#else
  ssize_t n=pread(f->fd,data,length,(off_t)position);rc=n<0?-errno:(int)n;
#endif
 }
 if(rc>=0)f->bytes+=(unsigned)rc;
done:
 if(rc<0)f->error=rc;pthread_mutex_unlock(&f->mutex);return rc;
}

/* Native AvPlayer file callbacks run on small Sony thread stacks. HTTP/SSL
 * APIs require more stack; all file operations are synchronously dispatched
 * to an owned 2 MiB worker. Callback arguments remain alive until completion. */
enum {IO_OPEN=1,IO_CLOSE,IO_READ,IO_SIZE};
static void *io_worker(void *object){
 MediaFile *f=object;
 pthread_mutex_lock(&f->command_mutex);
 for(;;){
  while(f->running && !f->command)pthread_cond_wait(&f->wake,&f->command_mutex);
  if(!f->running)break;
  int command=f->command;const char *name=f->command_name;uint8_t *data=f->command_data;uint64_t position=f->command_position;uint32_t length=f->command_length;
  pthread_mutex_unlock(&f->command_mutex);
  uint64_t result=command==IO_OPEN?(uint64_t)(int64_t)io_open(f,name):command==IO_CLOSE?(uint64_t)(int64_t)io_close(f):command==IO_READ?(uint64_t)(int64_t)io_read(f,data,position,length):io_size(f);
  pthread_mutex_lock(&f->command_mutex);f->result=result;f->command=0;pthread_cond_broadcast(&f->done);
 }
 pthread_mutex_unlock(&f->command_mutex);return NULL;
}
static uint64_t dispatch(MediaFile *f,int command,const char *name,uint8_t *data,uint64_t position,uint32_t length){
 if(!f || !f->worker_started)return (uint64_t)(int64_t)-ENOMEM;
 pthread_mutex_lock(&f->command_mutex);
 while(f->busy)pthread_cond_wait(&f->done,&f->command_mutex);
 f->busy=1;f->command_name=name;f->command_data=data;f->command_position=position;f->command_length=length;f->command=command;pthread_cond_signal(&f->wake);
 while(f->command)pthread_cond_wait(&f->done,&f->command_mutex);
 uint64_t result=f->result;f->busy=0;pthread_cond_broadcast(&f->done);pthread_mutex_unlock(&f->command_mutex);return result;
}
int media_file_open(void *f,const char *name){return (int)(int64_t)dispatch(f,IO_OPEN,name,NULL,0,0);}
int media_file_close(void *f){return (int)(int64_t)dispatch(f,IO_CLOSE,NULL,NULL,0,0);}
int media_file_read(void *f,uint8_t *data,uint64_t position,uint32_t length){return (int)(int64_t)dispatch(f,IO_READ,NULL,data,position,length);}
uint64_t media_file_size(void *f){uint64_t size=dispatch(f,IO_SIZE,NULL,NULL,0,0);return size>(uint64_t)INT64_MAX && size!=UINT64_MAX?0:size;}
void media_file_destroy(MediaFile *f){
 if(f->worker_started){media_file_close(f);pthread_mutex_lock(&f->command_mutex);f->running=0;pthread_cond_signal(&f->wake);pthread_mutex_unlock(&f->command_mutex);pthread_join(f->worker,NULL);}
 pthread_cond_destroy(&f->wake);pthread_cond_destroy(&f->done);pthread_mutex_destroy(&f->command_mutex);pthread_mutex_destroy(&f->mutex);
}
