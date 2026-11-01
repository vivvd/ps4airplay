/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
typedef struct { int valid; int64_t first_pts,last_pts; uint64_t first_clock; unsigned resets; } LabVideoPacer;
uint64_t lab_video_pacer_due(LabVideoPacer *p,int64_t pts,uint64_t now,unsigned buffer_us);
