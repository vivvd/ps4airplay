/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "tls_http.h"
typedef struct {
 int (*template_create)(int,const char *,int,int),(*connection_create)(int,const char *,int),(*request_create)(int,int,const char *,uint64_t);
 int (*header_add)(int,const char *,const char *,int),(*send)(int,const void *,size_t),(*status)(int,int *),(*length)(int,int *,size_t *),(*headers)(int,char **,size_t *),(*read)(int,void *,uint32_t);
 int (*request_delete)(int),(*connection_delete)(int),(*template_delete)(int);
 int verified_tls;
} MediaHttp;
typedef struct {
 pthread_mutex_t command_mutex;pthread_cond_t wake,done;pthread_t worker;int worker_started,running,busy,command;
 const char *command_name;uint8_t *command_data;uint64_t command_position,result;uint32_t command_length;
 TlsHttp *secure;TlsDiagnostics diagnostics;
 pthread_mutex_t mutex;const MediaHttp *api;int context,fd,template_id,connection,request,error,is_http,is_embedded;
 uint64_t position,length,reads,bytes;char url[8193];
} MediaFile;
void media_file_init(MediaFile *,const MediaHttp *,int);
void media_file_destroy(MediaFile *);
int media_file_open(void *,const char *);
int media_file_close(void *);
int media_file_read(void *,uint8_t *,uint64_t,uint32_t);
uint64_t media_file_size(void *);
