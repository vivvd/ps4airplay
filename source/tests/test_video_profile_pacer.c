/* Deterministic timestamp tests plus actual atomic profile persistence. */
#include "video_profile.h"
#include "video_pacer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    LabVideoPacer p={0};
    assert(lab_video_pacer_due(&p,1000000,10000000,50000)==10050000);
    /* Arrival jitter must not change the cadence encoded in source PTS. */
    assert(lab_video_pacer_due(&p,1033333,10020000,50000)==10083333);
    assert(lab_video_pacer_due(&p,1066666,10021000,50000)==10116666);
    assert(p.resets==0);
    assert(lab_video_pacer_due(&p,500000,10030000,50000)==10080000 && p.resets==1);
    assert(lab_video_pacer_due(&p,50000000,10040000,50000)==10090000 && p.resets==2);
    assert(lab_video_pacer_due(&p,-1,10050000,50000)==10050000);
    assert(lab_video_pacer_due(&p,50033333,12000000,50000)==12050000 && p.resets==3);
    LabVideoPacer early={0};
    assert(lab_video_pacer_due(&early,0,1000,50000)==51000);
    assert(lab_video_pacer_due(&early,300000,2000,50000)==52000 && early.resets==1);
    LabVideoPacer fast={0};
    uint64_t initial=lab_video_pacer_due(&fast,0,1000,33000);
    assert(initial==34000);
    for(int i=1;i<100;++i){
        uint64_t pts=(uint64_t)i*1000000/60;
        assert(lab_video_pacer_due(&fast,(int64_t)pts,1000+pts,33000)==initial+pts);
    }
    remove(LAB_VIDEO_PROFILE_PATH);
    LabState s;lab_init(&s,"profile-test");lab_video_profile_load(&s);
    assert(s.video_profile==1 && s.video_pending_profile==1);
    assert(lab_video_profile(s.video_profile)->width==1920);
    assert(lab_video_profile_save(&s,2)==0);
    assert(s.video_profile==1 && s.video_pending_profile==2); /* no live mutation */
    lab_video_profile_load(&s);assert(s.video_profile==2 && lab_video_profile(2)->fps==60);
    assert(lab_video_profile_save(&s,0)==0);lab_video_profile_load(&s);
    assert(s.video_profile==0 && lab_video_profile(0)->height==720);
    assert(lab_video_profile_save(&s,-1)==-1 && lab_video_profile_save(&s,3)==-1);
    assert(lab_video_profile_id("../../bad")==-1);
    FILE *f=fopen(LAB_VIDEO_PROFILE_PATH,"wb");assert(f);fputs("invalid profile\n",f);fclose(f);
    lab_video_profile_load(&s);assert(s.video_profile==1);
    assert(lab_video_profile_save(&s,1)==0);
    f=fopen(LAB_VIDEO_PROFILE_PATH,"rb");assert(f);
    char value[24]={0};assert(fread(value,1,sizeof(value)-1,f)==8);fclose(f);
    assert(!strcmp(value,"1080p30\n"));
    remove(LAB_VIDEO_PROFILE_PATH);
    puts("PTS jitter, discontinuity, 60 fps cadence, profile persistence and invalid-input tests passed");
    return 0;
}
