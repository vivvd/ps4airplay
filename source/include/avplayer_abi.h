/* Public ABI layouts checked against shadPS4 avplayer.h, Copyright 2024
 * shadPS4 Emulator Project, GPL-2.0-or-later. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct {uint16_t channels;uint8_t reserved[2];uint32_t rate,size;char language[4];} AP_Audio;
typedef struct {uint32_t width,height;float aspect;char language[4];} AP_Video;
typedef union {AP_Audio audio;AP_Video video;uint8_t reserved[16];} AP_Details;
typedef struct {void *data;uint8_t reserved[4];uint64_t timestamp;AP_Details details;} AP_Frame;
typedef struct {uint32_t type;uint8_t reserved[4];AP_Details details;uint64_t duration,start;} AP_Stream;
typedef struct {uint32_t width,height;float aspect;char language[4];uint32_t framerate,left,right,top,bottom,pitch;uint8_t luma,chroma;bool full_range;uint8_t reserved[37];} AP_VideoEx;
typedef struct {void *data;uint8_t reserved[4];uint64_t timestamp;union {AP_VideoEx video;uint8_t reserved[80];} details;} AP_FrameEx;
typedef struct {void *object;void *(*alloc)(void *,uint32_t,uint32_t);void (*free)(void *,void *);void *(*texture)(void *,uint32_t,uint32_t);void (*free_texture)(void *,void *);} AP_Memory;
typedef struct {void *object;int (*open)(void *,const char *);int (*close)(void *);int (*read)(void *,uint8_t *,uint64_t,uint32_t);uint64_t (*size)(void *);} AP_File;
typedef struct {void *object;void (*callback)(void *,uint32_t,int32_t,void *);} AP_Event;
typedef struct {AP_Memory memory;AP_File file;AP_Event event;uint32_t debug,priority;int32_t buffers;bool auto_start;uint8_t reserved[3];const char *language;} AP_Init;
typedef struct {const char *name;uint32_t length;} AP_Uri;
typedef struct {AP_Uri uri;uint8_t reserved1[64];uint32_t type;uint8_t reserved2[44];} AP_Source;
_Static_assert(sizeof(AP_Init)==120 && offsetof(AP_Init,auto_start)==108 && offsetof(AP_Init,language)==112,"AvPlayer Init ABI");
_Static_assert(sizeof(AP_Source)==128 && offsetof(AP_Source,type)==80,"AvPlayer Source ABI");
_Static_assert(sizeof(AP_Frame)==40 && sizeof(AP_FrameEx)==104 && offsetof(AP_FrameEx,details)==24,"AvPlayer Frame ABI");
_Static_assert(sizeof(AP_Stream)==40 && sizeof(AP_VideoEx)==80,"AvPlayer Stream ABI");

/* sceAvPlayerPostInit supplies contexts created by the application. */
typedef struct {uint32_t type;uint8_t params[28];} AP_DecoderInit;
typedef struct {uint32_t demux_video_buffer;AP_DecoderInit video,audio;uint32_t http_context,ssl_context;uint8_t reserved[56];} AP_PostInit;
_Static_assert(sizeof(AP_PostInit)==132 && offsetof(AP_PostInit,http_context)==68,"AvPlayer PostInit ABI");

typedef struct {uint32_t priority,affinity;} AP_ThreadConfig;
typedef struct {size_t size;AP_Memory memory;AP_File file;AP_Event event;const char *language;uint32_t debug;AP_ThreadConfig audio_thread,video_thread,demux_thread,controller_thread,http_thread,file_thread;int32_t buffers;bool auto_start;uint8_t reserved[3];} AP_InitEx;
_Static_assert(sizeof(AP_InitEx)==176 && offsetof(AP_InitEx,http_thread)==148 && offsetof(AP_InitEx,auto_start)==168,"AvPlayer InitEx ABI");
