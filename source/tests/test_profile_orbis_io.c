/* Run the LAB_PS4 persistence branch with a fake Orbis file API.
 * Covers native raw errors and short I/O; does not emulate a PS4 filesystem. */
#include "video_profile.h"
#include <orbis/libkernel.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

enum {OK,OPEN_FAIL,WRITE_FAIL,ZERO_WRITE,CLOSE_FAIL,RENAME_FAIL,READ_FAIL,SHORT_IO};
static int fault,open_calls,write_calls,rename_calls;
#define NATIVE_ERROR ((int32_t)0x80020005u)
int32_t sceKernelOpen(const char *path,int32_t flags,uint16_t mode) {
    ++open_calls;
    if(fault==OPEN_FAIL && strstr(path,".tmp"))return (int32_t)0x8002000du;
    int fd=open(path,flags,mode);return fd<0?-errno:fd;
}
int32_t sceKernelClose(int32_t fd) {
    int rc=close(fd);
    return fault==CLOSE_FAIL?NATIVE_ERROR:rc<0?-errno:0;
}
size_t sceKernelRead(int32_t fd,void *out,size_t size) {
    if(fault==READ_FAIL)return (size_t)(int64_t)NATIVE_ERROR;
    if(fault==SHORT_IO && size>2)size=2;
    ssize_t n=read(fd,out,size);return n<0?(size_t)(int64_t)(-errno):(size_t)n;
}
size_t sceKernelWrite(int32_t fd,const void *data,size_t size) {
    ++write_calls;
    if(fault==WRITE_FAIL)return (size_t)(uint32_t)NATIVE_ERROR; /* zero extended */
    if(fault==ZERO_WRITE)return 0;
    if(fault==SHORT_IO && size>3)size=3;
    ssize_t n=write(fd,data,size);return n<0?(size_t)(int64_t)(-errno):(size_t)n;
}
int32_t sceKernelRename(const char *a,const char *b) {
    ++rename_calls;if(fault==RENAME_FAIL)return NATIVE_ERROR;
    return rename(a,b)<0?-errno:0;
}
int32_t sceKernelUnlink(const char *path) {return unlink(path)<0?-errno:0;}
int32_t sceKernelMkdir(const char *path,uint16_t mode) {
    assert(!strcmp(path,"/data/ps4-receiver") && mode==0700);return 0;
}
int main(void) {
    remove(LAB_VIDEO_PROFILE_PATH);remove(LAB_VIDEO_PROFILE_PATH ".tmp");
    LabState s;lab_init(&s,"orbis-io-mock");
    assert(lab_video_profile_save(&s,0)==0);
    assert(s.video_profile==1 && s.video_pending_profile==0);
    assert(!strcmp(s.video_profile_save_stage,"saved") && s.video_profile_save_rc==0);
    const char *stages[]={"","open_tmp","write_tmp","write_tmp","close_tmp","rename","verify"};
    for(int kind=OPEN_FAIL;kind<=READ_FAIL;++kind){
        fault=OK;assert(lab_video_profile_save(&s,0)==0);
        fault=kind;assert(lab_video_profile_save(&s,2)==-1);
        assert(!strcmp(s.video_profile_save_stage,stages[kind]));
        assert(s.video_profile_save_rc<0 && s.video_pending_profile==0 && s.video_profile==1);
        char report[42000];lab_report(&s,report,sizeof(report));
        assert(strstr(report,"\"video_profile_save_stage\":"));
        fault=OK;
        LabState loaded;lab_init(&loaded,"load-test");lab_video_profile_load(&loaded);
        assert(loaded.video_profile==(kind==READ_FAIL?2:0));
        FILE *tmp=fopen(LAB_VIDEO_PROFILE_PATH ".tmp","rb");assert(!tmp);
    }
    fault=SHORT_IO;int previous=write_calls;
    assert(lab_video_profile_save(&s,2)==0 && write_calls-previous==3);
    LabState loaded;lab_init(&loaded,"load-test");lab_video_profile_load(&loaded);
    assert(loaded.video_profile==2 && s.video_profile_save_rc==0);
    assert(open_calls>0 && rename_calls>0);
    fault=OK;remove(LAB_VIDEO_PROFILE_PATH);
    puts("Orbis persistence branch: atomic commit, readback, raw errors, short I/O and truthful pending state passed");
    return 0;
}
