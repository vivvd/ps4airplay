/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "lab.h"
#include "raop.h"
int direct_start(void);
void direct_play(void *cls,const char *url,float seconds);
void direct_play_fraction(void *cls,const char *url,float fraction);
void direct_seek(void *cls,float seconds);
void direct_rate(void *cls,float rate);
void direct_stop(void *cls);
void direct_info(void *cls,playback_info_t *info);
float direct_remove(void *cls);
void direct_poll(LabState *s);
int direct_draw(uint32_t *pixels);
void direct_invalidate(void);
void direct_shutdown(void);
void lab_audio_direct_mode(int active);
unsigned lab_audio_direct_space(void);
void lab_audio_direct_pcm(const int16_t *pcm,unsigned frames);

void direct_protocol(void *cls,int event);
void direct_protocol_trace(void *cls,const char *label);

void direct_test(int mode);
