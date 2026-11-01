/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "lab.h"
#define NP_ART_SIZE 600
typedef struct {
    char title[512],artist[256],album[256];
    unsigned long revision,art_revision;
    int active,playing,has_art;
    double position,duration;
} NowPlaying;
int np_start(void);
void np_metadata(const void *data,int size);
void np_coverart(const void *data,int size);
void np_progress(uint32_t start,uint32_t current,uint32_t end);
void np_audio(unsigned sample_rate);
void np_stop(void);
void np_snapshot(NowPlaying *out,uint32_t *art,unsigned long *art_revision);
void np_poll(LabState *s);
void np_shutdown(void);
