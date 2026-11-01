/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "video_pacer.h"
uint64_t lab_video_pacer_due(LabVideoPacer *p,int64_t pts,uint64_t now,unsigned buffer_us) {
    if(pts<0)return now;
    int reset=!p->valid || pts<p->last_pts || pts-p->last_pts>500000;
    uint64_t due=now;
    if(!reset){
        due=p->first_clock+(uint64_t)(pts-p->first_pts)+buffer_us;
        if((due>now && due-now>250000) || (now>due && now-due>250000))reset=1;
    }
    if(reset){
        if(p->valid)++p->resets;
        p->valid=1;p->first_pts=pts;p->first_clock=now;
        due=now+buffer_us;
    }
    p->last_pts=pts;
    return due;
}
