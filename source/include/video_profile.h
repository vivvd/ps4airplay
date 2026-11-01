/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "lab.h"
typedef struct { const char *id; unsigned width,height,fps,buffer_us; } LabVideoProfile;
const LabVideoProfile *lab_video_profile(int mode);
int lab_video_profile_id(const char *id);
void lab_video_profile_load(LabState *s);
int lab_video_profile_save(LabState *s,int mode);
