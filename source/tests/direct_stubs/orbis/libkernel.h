#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
typedef int OrbisKernelModule;
uint64_t sceKernelGetProcessTime(void);
void sceKernelUsleep(unsigned usec);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(off_t,off_t,size_t,size_t,int,off_t *);
int sceKernelMapDirectMemory(void **,size_t,int,int,off_t,size_t);
int sceKernelMunmap(void *,size_t);
int sceKernelReleaseDirectMemory(off_t,size_t);
int sceKernelGetModuleList(int *,size_t,size_t *);
int sceKernelDlsym(int,const char *,void **);
