#ifndef SAILFISH_TAILWIND_RECORDER_H
#define SAILFISH_TAILWIND_RECORDER_H

#include <wayland-client.h>
#include "wayland-lipstick-recorder-client-protocol.h"

#include <droidmedia/droidmedia.h>
#include <droidmedia/droidmediacodec.h>
#include <droidmedia/droidmediaconstants.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>

#define CAPTURE_BUFFER_COUNT 6
#define CAPTURE_QUEUE_CAP 6
#define DEFAULT_BITRATE 24000000

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CaptureBuffer {
    struct wl_buffer *wl_buffer;
    uint8_t *data;
    size_t size;
    int state; /* 0 free, 1 in flight, 2 queued to worker */
    int32_t transform;
    uint64_t seq;
    uint32_t capture_time_ms;
} CaptureBuffer;

typedef struct CaptureQueue {
    CaptureBuffer *items[CAPTURE_QUEUE_CAP];
    int head;
    int tail;
    int count;
    int stop;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} CaptureQueue;

typedef struct PtsNode {
    int64_t pts_us;
    struct PtsNode *next;
} PtsNode;

typedef struct EncoderState {
    DroidMediaCodec *codec;
    FILE *out;
    AVFormatContext *mux;
    AVStream *stream;
    PtsNode *pts_head;
    PtsNode *pts_tail;
    size_t pts_count;
    int64_t last_pts_us;
    int64_t nominal_frame_us;
    pthread_t loop_thread;
    volatile int loop_running;
    volatile int loop_started;
    long long loop_calls;
    long long encoded_bytes;
    int encoded_packets;
    int codec_config_packets;
    int errors;
    int eos;
    int chosen_format;
    pthread_mutex_t mutex;
} EncoderState;

typedef struct App {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct wl_output *output;
    struct lipstick_recorder_manager *manager;
    struct lipstick_recorder *recorder;

    int width;
    int height;
    int stride;
    int target_fps;
    int seconds;
    int bitrate;
    const char *out_path;

    int running;
    int failed;
    int worker_started;
    pthread_t worker_thread;
    CaptureQueue queue;
    EncoderState enc;
    CaptureBuffer buffers[CAPTURE_BUFFER_COUNT];

    uint64_t frames_captured;
    uint64_t frames_sent_encoder;
    uint64_t frames_dropped_queue;
    uint64_t frames_dropped_no_buffer;
    uint64_t frames_converted;
    uint64_t cancelled;
    uint64_t in_flight;
    uint64_t seq_next;

    uint32_t last_lipstick_time;
    uint32_t first_lipstick_time;
    int have_first_lipstick_time;
    int64_t first_capture_monotonic_us;
    uint32_t min_delta;
    uint32_t max_delta;
    uint64_t sum_delta;
    uint64_t delta_count;

    double start_seconds;
    double last_progress;
} App;

/* Time helpers */
double now_seconds(void);
int64_t now_monotonic_us(void);

/* Capture pipeline */
int create_capture_buffer(App *app, CaptureBuffer *buf);
void request_one(App *app);
void *worker_thread(void *arg);

/* Encoder */
int encoder_start(App *app);
void encoder_stop(App *app);

/* App lifecycle */
int parse_int(int argc, char **argv, int idx, int def, int min, int max);
void stop_worker(App *app);
void print_result(App *app);
void cleanup(App *app);

/* Wayland listeners (installed by the application) */
extern const struct lipstick_recorder_listener recorder_listener;
extern const struct wl_output_listener output_listener;
extern const struct wl_registry_listener registry_listener;

#ifdef __cplusplus
}
#endif

#endif /* SAILFISH_TAILWIND_RECORDER_H */
