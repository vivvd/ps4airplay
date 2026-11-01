/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "video_profile.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef LAB_PS4
#include <orbis/libkernel.h>
#endif
#ifndef LAB_VIDEO_PROFILE_PATH
#ifdef LAB_PS4
#define LAB_VIDEO_PROFILE_PATH "/data/ps4-receiver/video-profile.txt"
#else
#define LAB_VIDEO_PROFILE_PATH "build/host-video-profile.txt"
#endif
#endif
static const LabVideoProfile profiles[]={
    {"720p30",1280,720,30,50000},
    {"1080p30",1920,1080,30,50000},
    {"720p60",1280,720,60,33000}
};
const LabVideoProfile *lab_video_profile(int mode) {
    return &profiles[mode>=0 && mode<3?mode:1];
}
int lab_video_profile_id(const char *id) {
    for(int i=0;i<3;++i)if(id && !strcmp(id,profiles[i].id))return i;
    return -1;
}
/* Orbis kernel APIs return native error codes, not POSIX -1/errno. In
 * particular the SDK declares read/write as size_t, so reject any result
 * larger than the requested size and retain its signed low 32-bit code. */
static int profile_open(const char *path,int flags) {
#ifdef LAB_PS4
    return sceKernelOpen(path,flags,0600);
#else
    int rc=open(path,flags,0600);return rc<0?-errno:rc;
#endif
}
static int profile_close(int fd) {
#ifdef LAB_PS4
    return sceKernelClose(fd);
#else
    return close(fd)<0?-errno:0;
#endif
}
static int64_t profile_read(int fd,void *data,size_t size) {
#ifdef LAB_PS4
    size_t rc=sceKernelRead(fd,data,size);
    if(rc<=size)return (int64_t)rc;
    int32_t error=(int32_t)(uint32_t)rc;return error<0?error:-EIO;
#else
    ssize_t rc=read(fd,data,size);return rc<0?-errno:rc;
#endif
}
static int64_t profile_write(int fd,const void *data,size_t size) {
#ifdef LAB_PS4
    size_t rc=sceKernelWrite(fd,data,size);
    if(rc<=size)return (int64_t)rc;
    int32_t error=(int32_t)(uint32_t)rc;return error<0?error:-EIO;
#else
    ssize_t rc=write(fd,data,size);return rc<0?-errno:rc;
#endif
}
static int profile_rename(const char *from,const char *to) {
#ifdef LAB_PS4
    return sceKernelRename(from,to);
#else
    return rename(from,to)<0?-errno:0;
#endif
}
static void profile_unlink(const char *path) {
#ifdef LAB_PS4
    sceKernelUnlink(path);
#else
    unlink(path);
#endif
}
static int read_profile(int *error) {
    *error=0;
    int fd=profile_open(LAB_VIDEO_PROFILE_PATH,O_RDONLY);
    if(fd<0){*error=fd;return -1;}
    char value[32];size_t size=0;
    while(size<sizeof(value)-1){
        int64_t n=profile_read(fd,value+size,sizeof(value)-1-size);
        if(n<0){*error=(int)n;break;}
        if(!n)break;
        size+=(size_t)n;
    }
    int close_rc=profile_close(fd);
    if(!*error && close_rc)*error=close_rc;
    if(*error)return -1;
    if(size==sizeof(value)-1 || memchr(value,0,size)){*error=-EINVAL;return -1;}
    value[size]=0;
    while(size && (value[size-1]=='\r' || value[size-1]=='\n'))value[--size]=0;
    int mode=lab_video_profile_id(value);
    if(mode<0)*error=-EINVAL;
    return mode;
}
void lab_video_profile_load(LabState *s) {
    int error=0,mode=read_profile(&error);
    if(mode<0){
        mode=1;
        lab_log(s,"Video profile unavailable rc=0x%08x; using 1080p30",(unsigned)error);
    }
    s->video_profile=s->video_pending_profile=mode;
    lab_log(s,"Video profile: %s; change applies after app restart",profiles[mode].id);
}
static int save_failure(LabState *s,const char *stage,int rc) {
    snprintf(s->video_profile_save_stage,sizeof(s->video_profile_save_stage),"%s",stage);
    s->video_profile_save_rc=rc?rc:-EIO;
    lab_log(s,"Video profile save failed: stage=%s rc=0x%08x",stage,(unsigned)s->video_profile_save_rc);
    return -1;
}
int lab_video_profile_save(LabState *s,int mode) {
    if(mode<0 || mode>=3)return save_failure(s,"invalid_mode",-EINVAL);
#ifdef LAB_PS4
    sceKernelMkdir("/data/ps4-receiver",0700);
#endif
    const char temporary[]=LAB_VIDEO_PROFILE_PATH ".tmp";
    int fd=profile_open(temporary,O_WRONLY|O_CREAT|O_TRUNC);
    if(fd<0)return save_failure(s,"open_tmp",fd);
    char value[24];int n=snprintf(value,sizeof(value),"%s\n",profiles[mode].id);
    size_t done=0;int error=0;
    while(done<(size_t)n){
        int64_t written=profile_write(fd,value+done,(size_t)n-done);
        if(written<=0){error=written?(int)written:-EIO;break;}
        done+=(size_t)written;
    }
    int close_rc=profile_close(fd);
    if(error || close_rc){
        profile_unlink(temporary);
        return save_failure(s,error?"write_tmp":"close_tmp",error?error:close_rc);
    }
    int rename_rc=profile_rename(temporary,LAB_VIDEO_PROFILE_PATH);
    if(rename_rc){profile_unlink(temporary);return save_failure(s,"rename",rename_rc);}
    int verified=read_profile(&error);
    if(verified!=mode)return save_failure(s,"verify",error?error:-EIO);
    s->video_pending_profile=mode;
    s->video_profile_save_rc=0;
    snprintf(s->video_profile_save_stage,sizeof(s->video_profile_save_stage),"saved");
    lab_log(s,"Video profile saved and verified: %s; restart app with Options to apply",profiles[mode].id);
    return 0;
}
