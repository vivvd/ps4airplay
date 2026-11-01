/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "lab.h"
void player_ui_draw(const LabState *state,uint32_t *pixels);
void player_ui_invalidate(void);
void player_ui_shutdown(void);
