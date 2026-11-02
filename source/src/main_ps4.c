/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lab.h"
#include "video_receiver.h"
#include "now_playing.h"
#include "player_ui.h"
#include "direct_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/AudioOut.h>
#include <orbis/Pad.h>
#include <orbis/NetCtl.h>
#include <orbis/Net.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>

typedef struct {
    int video, registered, queue_ready;
    OrbisKernelEqueue queue;
    off_t offset;
    size_t size;
    void *memory;
    uint32_t *buffers[2];
} Display;

static int display_open(Display *d) {
    memset(d, 0, sizeof(*d)); d->video = -1;
    int rc;
    d->video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, NULL);
    if (d->video < 0) return d->video;
    rc = sceKernelCreateEqueue(&d->queue, "receiver lab flip");
    if (rc < 0) return rc;
    d->queue_ready = 1;
    rc = sceVideoOutAddFlipEvent(d->queue, d->video, NULL);
    if (rc < 0) return rc;
    size_t stride = ((LAB_WIDTH * LAB_HEIGHT * 4) + 0xffff) & ~(size_t)0xffff;
    d->size = (stride*2+0x1fffff)&~(size_t)0x1fffff;
    rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), d->size, 0x200000, 3, &d->offset);
    if (rc < 0) { d->size = 0; return rc; }
    rc = sceKernelMapDirectMemory(&d->memory, d->size, 0x33, 0, d->offset, 0x200000);
    if (rc < 0) return rc;
    d->buffers[0] = d->memory;
    /* Buffer bases aligned to 64 KiB, including the second buffer. */
    d->buffers[1] = (uint32_t *)((char *)d->memory + stride);
    OrbisVideoOutBufferAttribute attr;
    memset(&attr, 0, sizeof(attr));
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, LAB_WIDTH, LAB_HEIGHT, LAB_WIDTH);
    rc = sceVideoOutRegisterBuffers(d->video, 0, (void *const *)d->buffers, 2, &attr);
    if (rc < 0) return rc;
    d->registered = 1;
    return sceVideoOutSetFlipRate(d->video, 0);
}
static void display_close(Display *d) {
    if (d->video >= 0) {
        if (d->registered) sceVideoOutUnregisterBuffers(d->video, 0);
        sceVideoOutClose(d->video);
    }
    if (d->queue_ready) sceKernelDeleteEqueue(d->queue);
    if (d->memory) sceKernelMunmap(d->memory, d->size);
    if (d->size) sceKernelReleaseDirectMemory(d->offset, d->size);
}
static int display_flip(Display *d, unsigned long frame, int index) {
    int rc = sceVideoOutSubmitFlip(d->video, index, ORBIS_VIDEO_OUT_FLIP_VSYNC, (int64_t)frame);
    if (rc < 0) return rc;
    /* A missed event must not hang the app forever. */
    for (int i = 0; i < 120; ++i) {
        OrbisVideoOutFlipStatus status;
        memset(&status, 0, sizeof(status));
        rc = sceVideoOutGetFlipStatus(d->video, &status);
        if (rc < 0) return rc;
        if (status.flipArg == (int64_t)frame) return 0;
        sceKernelUsleep(1000);
    }
    return -110;
}
static void decoder_probe(LabState *s) {
    /* Resolve only. Never call decoder functions with unverified signatures. */
    int handle = (int)sceKernelLoadStartModule("/system/common/lib/libSceVideodec2.sprx", 0, NULL, 0, NULL, NULL);
    s->decoder_module = handle;
    lab_log(s, "Videodec2 module: 0x%08x (presence probe only)", (unsigned)handle);
    if (handle < 0) return;
    const char *names[] = {"sceVideodec2CreateDecoder", "sceVideodec2Decode", "sceVideodec2DeleteDecoder"};
    for (int i = 0; i < 3; ++i) {
        void *ptr = NULL;
        int rc = sceKernelDlsym(handle, names[i], &ptr);
        if (rc == 0 && ptr) ++s->decoder_exports;
        lab_log(s, "%s resolve: 0x%08x", names[i], (unsigned)rc);
    }
}
static void tone(LabState *s) {
    s->pending_tone = 0;
    int rc = sceAudioOutInit();
    lab_log(s, "AudioOutInit: 0x%08x", (unsigned)rc);
    /* Already-initialized is allowed: Open() is the definitive check. */
    int handle = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM,
        ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (handle < 0) { s->audio_rc = handle; lab_log(s, "AudioOutOpen failed: 0x%08x", (unsigned)handle); return; }
    int16_t pcm[512];
    for (unsigned frame = 0; frame < 48128; frame += 256) {
        rc = sceAudioOutOutput(handle, NULL);
        if (rc < 0) break;
        lab_tone_block(pcm, frame, 256);
        rc = sceAudioOutOutput(handle, pcm);
        if (rc < 0) break;
    }
    int drain_rc = sceAudioOutOutput(handle, NULL);
    if (rc >= 0 && drain_rc < 0) rc = drain_rc;
    int close_rc = sceAudioOutClose(handle);
    if (rc >= 0 && close_rc < 0) rc = close_rc;
    s->audio_rc = rc < 0 ? rc : 0;
    ++s->tone_runs;
    lab_log(s, "Tone output finished: 0x%08x; audibility needs human confirmation", (unsigned)s->audio_rc);
}
static void network_info(LabState *s) {
    OrbisNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    int rc = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info);
    if (rc == 0 && info.ip_address[0]) snprintf(s->ip, sizeof(s->ip), "%.15s", info.ip_address);
    lab_log(s, "NetCtl IP query: 0x%08x; IP: %s", (unsigned)rc, s->ip);
}
int main(void) {
    LabState s; Display display;
    lab_init(&s, "ps4");
    setvbuf(stdout, NULL, _IONBF, 0);
    lab_log(&s, "PS4 AirPlay %s; automatic staged startup", LAB_VERSION);
    OrbisKernelSwVersion fw;
    memset(&fw, 0, sizeof(fw)); fw.Size = sizeof(fw);
    int rc = sceKernelGetSystemSwVersion(&fw);
    if (rc == 0) snprintf(s.firmware, sizeof(s.firmware), "%.27s / %08x", fw.VersionString, fw.Version);
    lab_log(&s, "Firmware: %s (query 0x%08x)", s.firmware, (unsigned)rc);
    OrbisUserServiceInitializeParams params;
    memset(&params, 0, sizeof(params)); params.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    rc = sceUserServiceInitialize(&params);
    lab_log(&s, "UserServiceInit: 0x%08x", (unsigned)rc);
    int user = -1, pad = -1;
    int user_rc = sceUserServiceGetInitialUser(&user);
    rc = scePadInit();
    if (rc == 0 && user_rc == 0) pad = scePadOpen(user, 0, 0, NULL);
    s.pad_rc = pad >= 0 ? 0 : (rc < 0 ? rc : user_rc < 0 ? user_rc : pad);
    lab_log(&s, "Pad: 0x%08x", (unsigned)s.pad_rc);
    rc = sceNetInit();
    lab_log(&s, "NetInit: 0x%08x", (unsigned)rc);
    rc = sceNetCtlInit();
    lab_log(&s, "NetCtlInit: 0x%08x", (unsigned)rc);
    network_info(&s);
    if (lab_server_start(&s, "0.0.0.0", LAB_PORT)) lab_log(&s, "HTTP startup failed: %d", s.net_rc);
    decoder_probe(&s);
    s.video_rc = display_open(&display);
    lab_log(&s, "VideoOut init: 0x%08x", (unsigned)s.video_rc);
    lab_airplay_restore_log(&s);
    lab_session_trace_restore(&s);
    lab_log(&s,s.recovery_mode ? "AirPlay recovery: startup paused; download report" : "AirPlay automatic staged startup scheduled");
    s.pending_airplay_step=!s.recovery_mode;
    unsigned previous_buttons = 0;
    int running = 1;
    while (running) {
        lab_airplay_poll(&s);
        lab_video_poll(&s);
        np_poll(&s);direct_poll(&s);
        lab_server_poll(&s);
        if(s.pending_direct_test){int mode=s.pending_direct_test;s.pending_direct_test=0;if(s.direct_available)direct_test(mode);else lab_log(&s,"Direct video module unavailable");}
        if(s.pending_airplay_step && s.frames>=30){
            s.pending_airplay_step=0;int startup_rc=-1;
            if(!s.recovery_mode || lab_session_trace_begin()==0){
                s.recovery_mode=0;startup_rc=lab_airplay_start(&s);
            }else lab_log(&s,"Cannot resume: session checkpoint file unavailable");
            s.airplay_started=startup_rc==0;
            if(startup_rc==1)s.pending_airplay_step=1;
        }
        if (pad >= 0) {
            OrbisPadData data; memset(&data, 0, sizeof(data));
            int read_rc = scePadReadState(pad, &data);
            if (read_rc == 0 && data.connected) {
                unsigned pressed = data.buttons & ~previous_buttons;
                previous_buttons = data.buttons;
                if (pressed & ORBIS_PAD_BUTTON_OPTIONS) {s.settings_open=!s.settings_open;s.settings_item=0;}
                else if(s.settings_open){
                    if(pressed & ORBIS_PAD_BUTTON_UP)s.settings_item=(s.settings_item+2)%3;
                    if(pressed & ORBIS_PAD_BUTTON_DOWN)s.settings_item=(s.settings_item+1)%3;
                    if(pressed & ORBIS_PAD_BUTTON_CIRCLE)s.settings_open=0;
                    if(pressed & ORBIS_PAD_BUTTON_CROSS){
                        if(s.settings_item==0)s.language=!s.language;
                        else if(s.settings_item==1)s.settings_open=0;
                        else running=0;
                    }
                }else{
                if (pressed & ORBIS_PAD_BUTTON_CROSS) {playback_info_t info={0};direct_info(NULL,&info);if(s.direct_active)direct_rate(NULL,info.rate>0?0:1);}
                if ((pressed & ORBIS_PAD_BUTTON_TRIANGLE) && s.show_status) s.pattern = 1;
                if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {s.pattern=0;direct_stop(NULL);}
                if (pressed & ORBIS_PAD_BUTTON_SQUARE) {s.pattern=0;s.show_status=!s.show_status;}
                }
            } else previous_buttons = 0;
        }
        if (s.pending_tone) tone(&s);
        if (s.video_rc == 0) {
            int index = (int)(s.frames % 2);
            if(s.settings_open){player_ui_draw(&s,display.buffers[index]);direct_invalidate();lab_video_invalidate_output();}
            else if(s.show_status || s.pattern){lab_render(&s,display.buffers[index]);direct_invalidate();lab_video_invalidate_output();player_ui_invalidate();}
            else if(direct_draw(display.buffers[index])){lab_video_invalidate_output();player_ui_invalidate();}
            else if(!s.direct_active && lab_video_draw(display.buffers[index]))player_ui_invalidate();
            else {player_ui_draw(&s,display.buffers[index]);lab_video_invalidate_output();}
            int flip_rc = display_flip(&display, s.frames + 1, index);
            if (flip_rc < 0) { s.video_rc = flip_rc; lab_log(&s, "VideoOut flip failed: 0x%08x", (unsigned)flip_rc); }
        } else sceKernelUsleep(16000);
        ++s.frames;
        if (s.frames % 600 == 0) network_info(&s);
    }
    lab_session_trace("shutdown","begin");
    lab_airplay_stop();
    lab_session_trace("shutdown","AirPlay stopped");
    lab_server_stop();
    player_ui_shutdown();
    if (pad >= 0) scePadClose(pad);
    display_close(&display);
    lab_hen_close(&s);
    lab_session_trace("session","clean stop");
    /* Ask the shell to close this title instead of returning through the CRT. */
    sceSystemServiceLoadExec("exit",NULL);
    return 0;
}
