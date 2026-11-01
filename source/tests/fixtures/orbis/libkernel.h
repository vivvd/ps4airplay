#pragma once
#include <stdint.h>
#include <stddef.h>
int32_t sceKernelOpen(const char *,int32_t,uint16_t);
int32_t sceKernelClose(int32_t);
size_t sceKernelRead(int32_t,void *,size_t);
size_t sceKernelWrite(int32_t,const void *,size_t);
int32_t sceKernelRename(const char *,const char *);
int32_t sceKernelUnlink(const char *);
int32_t sceKernelMkdir(const char *,uint16_t);
