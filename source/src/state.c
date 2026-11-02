/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lab.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

void lab_init(LabState *s, const char *platform) {
    memset(s, 0, sizeof(*s));
    snprintf(s->platform, sizeof(s->platform), "%s", platform);
    snprintf(s->firmware, sizeof(s->firmware), "unknown");
    snprintf(s->ip, sizeof(s->ip), "not available");
    s->video_rc = s->audio_rc = s->pad_rc = s->net_rc = -1;
    s->decoder_module = -1;
    s->video_profile=s->video_pending_profile=1;
    s->hen_probe_rc = s->hen_acquire_rc = s->hen_restore_rc = -1;
}

void lab_log(LabState *s, const char *format, ...) {
    char line[256];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    size_t n = strlen(line);
    if (s->log_len + n + 2 >= sizeof(s->log)) {
        /* Discard only complete oldest lines. */
        size_t drop = 0;
        while (s->log_len - drop + n + 2 >= sizeof(s->log)) {
            char *end = strchr(s->log + drop, '\n');
            if (!end) { drop = s->log_len; break; }
            drop = (size_t)(end - s->log) + 1;
        }
        memmove(s->log, s->log + drop, s->log_len - drop);
        s->log_len -= drop;
    }
    memcpy(s->log + s->log_len, line, n);
    s->log_len += n;
    s->log[s->log_len++] = '\n';
    s->log[s->log_len] = 0;
    printf("%s\n", line);
}

static size_t json_string(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; *in; ++in) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            if (n + 2 >= cap) break;
            out[n++] = '\\'; out[n++] = (char)c;
        } else if (c < 32) {
            if (n + 6 >= cap) break;
            snprintf(out + n, cap - n, "\\u%04x", c); n += 6;
        } else {
            if (n + 1 >= cap) break;
            out[n++] = (char)c;
        }
    }
    out[n] = 0;
    return n;
}

size_t lab_report(const LabState *s, char *out, size_t cap) {
    if (!cap) return 0;
    char log_json[LAB_LOG_SIZE * 6], fw[300], ip[300], platform[150];
    char protocol_json[12400];json_string(s->direct_protocol_log,protocol_json,sizeof(protocol_json));
    char native_json[12400];json_string(s->direct_native_log,native_json,sizeof(native_json));
    json_string(s->log, log_json, sizeof(log_json));
    json_string(s->firmware, fw, sizeof(fw));
    json_string(s->ip, ip, sizeof(ip));
    json_string(s->platform, platform, sizeof(platform));
    int n = snprintf(out, cap,
        "{\"app\":\"PS4 AirPlay\",\"version\":\"%s\","
        "\"platform\":\"%s\",\"firmware\":\"%s\",\"ip\":\"%s\","
        "\"language\":\"%s\",\"airplay_ready\":%s,\"h264_decode_tested\":%s,"
        "\"video_profile\":%d,\"video_pending_profile\":%d,"
        "\"video_profile_save_rc\":%d,\"video_profile_save_stage\":\"%s\", "
        "\"video_output_width\":1920,\"video_output_height\":1080,"
        "\"video_copy_us\":%lu,\"video_pacing_us\":%lu,\"video_pacer_resets\":%lu,"
        "\"video_packets\":%lu,\"video_decoded\":%lu,\"video_errors\":%lu,"
        "\"video_dropped\":%lu,\"video_presented\":%lu,"
        "\"video_converted\":%lu,\"video_render_skipped\":%lu,\"video_backpressure\":%lu,"
        "\"video_decode_us\":%lu,\"video_convert_us\":%lu,\"video_copies\":%lu,"
        "\"video_packet_age_ms\":%lu,\"video_frame_age_ms\":%lu,"
        "\"video_stream_active\":%s,\"video_cpu_flags\":%d,"
        "\"video_width\":%u,\"video_height\":%u,\"video_queue_bytes\":%u,"
        "\"metadata_packets\":%lu,\"cover_images\":%lu,\"metadata_errors\":%lu,"
        "\"direct_allocation_size\":%u,\"direct_allocation_alignment\":%u,\"direct_allocation_error\":%d,"
        "\"direct_stream_module\":%d,\"direct_io_error\":%d,\"direct_io_reads\":%lu,\"direct_io_bytes\":%lu,"
        "\"direct_backend\":\"ffmpeg\",\"direct_protocol_log\":\"%s\","
        "\"direct_native_log\":\"%s\", "
        "\"direct_stage\":\"%s\",\"direct_play_requests\":%lu,\"direct_play_rejected\":%lu,"
        "\"direct_reverse_requests\":%lu,\"direct_action_requests\":%lu,\"direct_info_requests\":%lu,"
        "\"direct_available\":%d,\"direct_active\":%d,\"direct_error\":%d,\"direct_event\":%d,"
        "\"direct_video_frames\":%lu,\"direct_audio_frames\":%lu,\"direct_errors\":%lu,"
        "\"direct_position\":%.3f,\"direct_duration\":%.3f,"
        "\"recovery_mode\":%s,"
        "\"hen_sdk_version\":%d,\"hen_probe_rc\":%d,\"hen_acquire_rc\":%d,"
        "\"hen_restore_rc\":%d,\"hen_active\":%s,"
        "\"airplay_step\":%d,\"audio_packets\":%lu,\"decoded_frames\":%lu,\"audio_errors\":%lu,"
        "\"video_rc\":%d,\"audio_rc\":%d,\"pad_rc\":%d,\"net_rc\":%d,"
        "\"decoder_module\":%d,\"decoder_exports\":%d,"
        "\"tone_runs\":%d,\"pattern\":%s,\"frames\":%lu,\"requests\":%lu,"
        "\"log\":\"%s\"}\n",
        LAB_VERSION, platform, fw, ip, s->language ? "ru" : "en", s->airplay_started ? "true" : "false",
        s->video_decoded ? "true" : "false",
        s->video_profile,s->video_pending_profile,s->video_profile_save_rc,s->video_profile_save_stage,s->video_copy_us,s->video_pacing_us,s->video_pacer_resets,
        s->video_packets,s->video_decoded,s->video_errors,s->video_dropped,s->video_presented,
        s->video_converted,s->video_render_skipped,s->video_backpressure,
        s->video_decode_us,s->video_convert_us,s->video_copies,s->video_packet_age_ms,s->video_frame_age_ms,
        s->video_stream_active ? "true" : "false",s->video_cpu_flags,
        s->video_width,s->video_height,s->video_queue_bytes,
        s->metadata_packets,s->cover_images,s->metadata_errors,
        s->direct_allocation_size,s->direct_allocation_alignment,s->direct_allocation_error,
        s->direct_stream_module,s->direct_io_error,s->direct_io_reads,s->direct_io_bytes,
        protocol_json,native_json,s->direct_stage,s->direct_play_requests,s->direct_play_rejected,s->direct_reverse_requests,s->direct_action_requests,s->direct_info_requests,
        s->direct_available,s->direct_active,s->direct_error,s->direct_event,
        s->direct_video_frames,s->direct_audio_frames,s->direct_errors,s->direct_position,s->direct_duration,
        s->recovery_mode ? "true" : "false",
        s->hen_sdk_version, s->hen_probe_rc, s->hen_acquire_rc, s->hen_restore_rc,
        s->hen_active ? "true" : "false",
        s->airplay_step,s->audio_packets,s->decoded_frames,s->audio_errors,s->video_rc, s->audio_rc, s->pad_rc,
        s->net_rc, s->decoder_module, s->decoder_exports, s->tone_runs,
        s->pattern ? "true" : "false", s->frames, s->requests, log_json);
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

void lab_tone_block(int16_t *samples, unsigned first, unsigned frames) {
    /* 1 s, 48 kHz, S16 stereo. L then R; peak ~-30 dBFS, soft ramps. */
    for (unsigned i = 0; i < frames; ++i) {
        unsigned t = first + i, p = t % 24000;
        double ramp = p < 480 ? p / 480.0 : p > 23519 ? (23999 - p) / 480.0 : 1.0;
        int16_t v = t < 48000 ? (int16_t)(1000.0 * ramp * sin(6.283185307179586 * 440.0 * t / 48000.0)) : 0;
        samples[i * 2] = t < 24000 ? v : 0;
        samples[i * 2 + 1] = t >= 24000 ? v : 0;
    }
}
