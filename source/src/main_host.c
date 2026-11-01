/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lab.h"
#include "video_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }
int main(int argc, char **argv) {
    LabState s;
    lab_init(&s, "host-simulation");
    lab_video_profile_load(&s);
    snprintf(s.firmware, sizeof(s.firmware), "HOST / PS4 API untested");
    snprintf(s.ip, sizeof(s.ip), "127.0.0.1");
    unsigned port = 19080;
    const char *snapshot = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--snapshot") && i + 1 < argc) snapshot = argv[++i];
        else if (!strcmp(argv[i], "--pattern")) s.pattern = 1;
        else { fprintf(stderr, "Usage: %s [--port N] [--snapshot file.ppm] [--pattern]\n", argv[0]); return 2; }
    }
    lab_log(&s, "Host adapter: PS4 video, audio and decoder APIs are NOT exercised");
    if (snapshot) {
        uint32_t *pixels = calloc(LAB_WIDTH * LAB_HEIGHT, 4);
        if (!pixels) return 1;
        lab_render(&s, pixels);
        FILE *f = fopen(snapshot, "wb");
        if (!f) { free(pixels); return 1; }
        fprintf(f, "P6\n%d %d\n255\n", LAB_WIDTH, LAB_HEIGHT);
        for (size_t i = 0; i < LAB_WIDTH * LAB_HEIGHT; ++i) {
            unsigned char rgb[3] = { pixels[i] >> 16, pixels[i] >> 8, pixels[i] };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f); free(pixels); return 0;
    }
    signal(SIGTERM, stop); signal(SIGINT, stop); signal(SIGPIPE, SIG_IGN);
    if (lab_server_start(&s, "127.0.0.1", port)) return 1;
    while (running) {
        lab_server_poll(&s);
        if(s.pending_airplay_step){s.pending_airplay_step=0;lab_log(&s,"Host: AirPlay step not executed; PS4 hardware required");}
        if (s.pending_tone) {
            s.pending_tone = 0;
            lab_log(&s, "Host: tone command handled; no PS4 audio output performed");
        }
        ++s.frames;
        usleep(5000);
    }
    lab_server_stop(); return 0;
}
