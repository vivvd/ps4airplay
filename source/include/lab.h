#ifndef RECEIVER_LAB_H
#define RECEIVER_LAB_H
#include <stdint.h>
#include <stddef.h>
#define LAB_VERSION "1.12-RC13"
#define LAB_WIDTH 1920
#define LAB_HEIGHT 1080
#define LAB_LOG_SIZE 6144
#define LAB_PORT 9080
typedef struct {
    char platform[24], firmware[48], ip[48];
    char network_stage[32];
    unsigned long connections;
    int video_rc, audio_rc, pad_rc, net_rc, decoder_module;
    int airplay_started, airplay_step, pending_airplay_step;
    int show_status,pending_direct_test;
    int language, settings_open, settings_item; /* 0 English, 1 Russian */
    int video_profile,video_pending_profile;
    int video_profile_save_rc;
    char video_profile_save_stage[24];
    unsigned long video_copy_us,video_pacing_us,video_pacer_resets;
    unsigned long video_packets,video_decoded,video_errors,video_dropped,video_presented;
    unsigned long video_converted,video_render_skipped,video_backpressure,video_decode_us,video_convert_us,video_copies;
    unsigned long video_packet_age_ms,video_frame_age_ms;
    int video_stream_active,video_cpu_flags;
    unsigned video_width,video_height,video_queue_bytes;
    unsigned long metadata_packets,cover_images,metadata_errors;
    unsigned direct_allocation_size,direct_allocation_alignment;int direct_allocation_error;
    int direct_stream_module,direct_io_error;unsigned long direct_io_reads,direct_io_bytes;
    char direct_stage[32],direct_native_log[2048],direct_protocol_log[2048];
    unsigned long direct_play_requests,direct_play_rejected,direct_reverse_requests,direct_action_requests,direct_info_requests;
    int direct_available,direct_active,direct_error,direct_event;
    unsigned long direct_video_frames,direct_audio_frames,direct_errors;
    double direct_position,direct_duration;
    int recovery_mode;
    int hen_sdk_version, hen_probe_rc, hen_acquire_rc, hen_restore_rc, hen_active;
    unsigned long audio_packets, decoded_frames, audio_errors;
    int decoder_exports, tone_runs, pattern, pending_tone;
    unsigned long frames, requests;
    char log[LAB_LOG_SIZE];
    size_t log_len;
} LabState;
void lab_init(LabState *s, const char *platform);
void lab_log(LabState *s, const char *format, ...);
size_t lab_report(const LabState *s, char *out, size_t cap);
void lab_render(const LabState *s, uint32_t *pixels);
void lab_tone_block(int16_t *samples, unsigned first_frame, unsigned frames);
int lab_server_start(LabState *s, const char *address, unsigned port);
void lab_server_poll(LabState *s);
void lab_server_stop(void);
void lab_session_trace(const char *phase,const char *detail);
void lab_session_trace_request(const char *method,const char *url);
int lab_session_trace_begin(void);
int lab_airplay_signals_init(void);
void lab_session_trace_restore(LabState *s);
int lab_hen_open(LabState *s);
int lab_hen_close(LabState *s);
void lab_airplay_restore_log(LabState *s);
int lab_airplay_start(LabState *s);
void lab_airplay_poll(LabState *s);
void lab_airplay_stop(void);
#endif
