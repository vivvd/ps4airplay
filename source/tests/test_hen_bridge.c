/* Exercise the real HEN bridge with injected kernel responses, never syscalls. */
#include "lab.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static long version=0x100, acquire, restore;
static int calls[4];
static void *saved;
static void previous_handler(int sig) { (void)sig; }
long lab_hen_command(unsigned long command, void *data) {
    assert(command==0 || command==2 || command==3);
    ++calls[command];
    if(command==0){assert(data==NULL);return version;}
    if(command==2){
        assert(data);saved=data;
        for(unsigned i=0;i<72;++i)assert(((unsigned char *)data)[i]==0);
        memset(data,0x5a,72);return acquire;
    }
    assert(data==saved);
    for(unsigned i=0;i<72;++i)assert(((unsigned char *)data)[i]==0x5a);
    return restore;
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"negative-version") || !strcmp(argv[1],"negative-version-denied"))version=-0x100;
    if(!strcmp(argv[1],"negative-version-denied"))acquire=-EPERM;
    if(!strcmp(argv[1],"unsupported"))version=-ENOSYS;
    if(!strcmp(argv[1],"unknown-abi"))version=0x200;
    if(!strcmp(argv[1],"denied"))acquire=-EPERM;
    if(!strcmp(argv[1],"restore-failed"))restore=-EACCES;
    signal(12,previous_handler);
    LabState s;lab_init(&s,"hen-test");
    int rc=lab_hen_open(&s);
    assert(signal(12,previous_handler)==previous_handler);
    int known_version=version==0x100 || version==-0x100;
    if(version==-0x100)assert(s.hen_sdk_version==0x100 && s.hen_probe_rc==-256);
    if(!known_version || acquire){
        assert(rc==-1 && !s.hen_active);
        assert(lab_hen_open(&s)==-1);assert(calls[0]==1);
        assert(calls[2]==known_version);
        assert(lab_hen_close(&s)==0 && calls[3]==0);
    }else{
        assert(rc==0 && s.hen_active);
        assert(lab_hen_open(&s)==0 && calls[2]==1);
        assert(lab_hen_close(&s)==(restore ? -1 : 0));
        assert(s.hen_active==(restore!=0));
        assert(s.hen_restore_rc==restore);
        if(restore){restore=0;assert(lab_hen_close(&s)==0);}
        assert(lab_hen_close(&s)==0);
        assert(calls[3]==(!strcmp(argv[1],"restore-failed") ? 2 : 1));
        assert(lab_hen_open(&s)==-1 && calls[2]==1);
    }
    char json[48000];lab_report(&s,json,sizeof(json));
    assert(strstr(json,"\"hen_active\":"));
    puts("HEN bridge lifecycle passed");
    return 0;
}
