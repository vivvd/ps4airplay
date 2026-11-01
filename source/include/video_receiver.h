/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "lab.h"
int lab_video_start(LabState *s);
int lab_video_configure(int h264,unsigned width,unsigned height);
void lab_video_submit(const unsigned char *data,int size,int hevc,uint64_t pts);
void lab_video_reset(void);
void lab_video_stream_state(int active);
void lab_video_invalidate_output(void);
void lab_video_poll(LabState *s);
int lab_video_draw(uint32_t *pixels);
void lab_video_stop(void);
