#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include "droidrec.h"

/* Singleton instance, created once on the first droidrec_start() and
 * persisted across start/stop cycles. */
static Runner *s_runner = NULL;
static const char *s_last_error = "ok";

static void set_error(const char *msg) {
    s_last_error = msg ? msg : "unknown error";
}

/* Utility functions */

double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

int64_t now_monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return
        (int64_t)ts.tv_sec * 1000000LL +
        (int64_t)ts.tv_nsec / 1000LL;
}

static inline uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* table used in the following function */
static inline void rgba_to_yuv(uint8_t r, uint8_t g, uint8_t b, int *y, int *u, int *v) {
    /* BT.601 full-ish range, fast integer approximation. */
    int yy = (( 66 * r + 129 * g +  25 * b + 128) >> 8) + 16;
    int uu = ((-38 * r -  74 * g + 112 * b + 128) >> 8) + 128;
    int vv = ((112 * r -  94 * g -  18 * b + 128) >> 8) + 128;
    *y = yy; *u = uu; *v = vv;
}

/* native rgba from lipstick to nv12 */
static void convert_rgba_to_nv12(uint8_t *dst, const uint8_t *src, int width, int height, int stride, int transform) {
    uint8_t *yplane = dst;
    uint8_t *uvplane = dst + (size_t)width * (size_t)height;

    for (int y = 0; y < height; y += 2) {
        int sy0 = (transform == LIPSTICK_RECORDER_TRANSFORM_Y_INVERTED) ? (height - 1 - y) : y;
        int sy1 = (y + 1 < height) ? ((transform == LIPSTICK_RECORDER_TRANSFORM_Y_INVERTED) ? (height - 2 - y) : (y + 1)) : sy0;
        const uint8_t *row0 = src + (size_t)sy0 * (size_t)stride;
        const uint8_t *row1 = src + (size_t)sy1 * (size_t)stride;

        for (int x = 0; x < width; x += 2) {
            const uint8_t *p00 = row0 + (size_t)x * 4;
            const uint8_t *p01 = row0 + (size_t)(x + 1 < width ? x + 1 : x) * 4;
            const uint8_t *p10 = row1 + (size_t)x * 4;
            const uint8_t *p11 = row1 + (size_t)(x + 1 < width ? x + 1 : x) * 4;

            int y00,u00,v00,y01,u01,v01,y10,u10,v10,y11,u11,v11;
            rgba_to_yuv(p00[0], p00[1], p00[2], &y00, &u00, &v00);
            rgba_to_yuv(p01[0], p01[1], p01[2], &y01, &u01, &v01);
            rgba_to_yuv(p10[0], p10[1], p10[2], &y10, &u10, &v10);
            rgba_to_yuv(p11[0], p11[1], p11[2], &y11, &u11, &v11);

            size_t yi0 = (size_t)y * (size_t)width + x;
            yplane[yi0] = clamp_u8(y00);
            if (x + 1 < width) yplane[yi0 + 1] = clamp_u8(y01);
            if (y + 1 < height) {
                size_t yi1 = (size_t)(y + 1) * (size_t)width + x;
                yplane[yi1] = clamp_u8(y10);
                if (x + 1 < width) yplane[yi1 + 1] = clamp_u8(y11);
            }

            int uavg = (u00 + u01 + u10 + u11) / 4;
            int vavg = (v00 + v01 + v10 + v11) / 4;
            size_t uvi = (size_t)(y / 2) * (size_t)width + x;
            uvplane[uvi] = clamp_u8(uavg);
            if (x + 1 < width) uvplane[uvi + 1] = clamp_u8(vavg);
        }
    }
}

/* required for housekeeping on multiple runs */
static void reset_recording_counters(Runner *runner) {
    runner->failed = 0;
    runner->worker_started = 0;
    runner->event_started = 0;
    runner->frames_captured = 0;
    runner->frames_sent_encoder = 0;
    runner->frames_dropped_queue = 0;
    runner->frames_dropped_no_buffer = 0;
    runner->frames_converted = 0;
    runner->cancelled = 0;
    runner->in_flight = 0;
    runner->seq_next = 0;
    runner->last_lipstick_time = 0;
    runner->first_lipstick_time = 0;
    runner->have_first_lipstick_time = 0;
    runner->first_capture_monotonic_us = 0;
    runner->min_delta = 0;
    runner->max_delta = 0;
    runner->sum_delta = 0;
    runner->delta_count = 0;
    runner->last_progress = -1.0;

    pthread_mutex_lock(&runner->queue.mutex);
    runner->queue.head = 0;
    runner->queue.tail = 0;
    runner->queue.count = 0;
    runner->queue.stop = 0;
    for (int n = 0; n < CAPTURE_QUEUE_CAP; n++) runner->queue.items[n] = NULL;
    pthread_mutex_unlock(&runner->queue.mutex);

    for (int i = 0; i < CAPTURE_BUFFER_COUNT; i++) {
        runner->buffers[i].state = 0;
    }
}

/* connecting wayland and lipstick recording listener/manager */
static int init_display(Runner *runner) {

    runner->display = wl_display_connect(NULL);
    if (!runner->display) {
        fprintf(stderr, "droidrec: wl_display_connect failed\n");
        return 0;
    }

    runner->registry = wl_display_get_registry(runner->display);
    wl_registry_add_listener(runner->registry, &registry_listener, runner);
    wl_display_roundtrip(runner->display); // Why twice?
    wl_display_roundtrip(runner->display);

    if (!runner->shm || !runner->output || !runner->manager) {
        fprintf(stderr, "droidrec: missing required Wayland globals\n");
        return 0;
    }

    runner->recorder = lipstick_recorder_manager_create_recorder(runner->manager, runner->output);
    if (!runner->recorder) {
        fprintf(stderr, "droidrec: create_recorder failed\n");
        return 0;
    }
    lipstick_recorder_add_listener(runner->recorder, &recorder_listener, runner);
    wl_display_roundtrip(runner->display);

    pthread_mutex_init(&runner->queue.mutex, NULL);
    pthread_cond_init(&runner->queue.cond, NULL);

    for (int i = 0; i < CAPTURE_BUFFER_COUNT; i++) {
        if (!create_capture_buffer(runner, &runner->buffers[i])) {
            fprintf(stderr, "droidrec: create_capture_buffer %d failed\n", i);
            return 0;
        }
    }

    printf("droidrec: ready width=%d height=%d stride=%d\n", runner->width, runner->height, runner->stride);
    fflush(stdout);
    return 1;
}

/* The evil primary loop */
static void *event_loop_thread(void *arg) {
    Runner *runner = (Runner *)arg;
    int fd = wl_display_get_fd(runner->display);
    double next_tick = runner->start_seconds;
    double period = runner->target_fps > 0 ? 1.0 / (double)runner->target_fps : (1.0 / 24.0);

    while (runner->running) {
        double now = now_seconds();
        if (now >= next_tick) {
            lipstick_recorder_repaint(runner->recorder);
            request_one(runner);
            next_tick += period;
            if (now - next_tick > 1.0) next_tick = now;
        }
        wl_display_dispatch_pending(runner->display);
        wl_display_flush(runner->display);

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int prc = poll(&pfd, 1, 5);
        if (prc > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_dispatch(runner->display) == -1) {
                perror("droidrec: wl_display_dispatch");
                break;
            }
        }
        // we use the 'fixed' second to record as a guard.
        // If someone forgets  to stop, it stops at runner->seconds
        if (runner->seconds > 0 && now_seconds() - runner->start_seconds >= (double)runner->seconds) {
            runner->running = 0;
        }
    }
    return NULL;
}


static int pts_push(EncoderState *enc, int64_t pts_us) {
    PtsNode *node = (PtsNode *)malloc(sizeof(PtsNode));

    if (!node) {
        return 0;
    }

    node->pts_us = pts_us;
    node->next = NULL;

    pthread_mutex_lock(&enc->mutex);

    if (enc->pts_tail) {
        enc->pts_tail->next = node;
    } else {
        enc->pts_head = node;
    }

    enc->pts_tail = node;
    enc->pts_count++;

    pthread_mutex_unlock(&enc->mutex);
    return 1;
}

static int pts_pop_locked(EncoderState *enc, int64_t *pts_us) {
    PtsNode *node = enc->pts_head;

    if (!node) {
        return 0;
    }

    enc->pts_head = node->next;

    if (!enc->pts_head) {
        enc->pts_tail = NULL;
    }

    if (enc->pts_count > 0) {
        enc->pts_count--;
    }

    *pts_us = node->pts_us;
    free(node);
    return 1;
}

static void pts_clear_locked(EncoderState *enc) {
    PtsNode *node = enc->pts_head;

    while (node) {
        PtsNode *next = node->next;
        free(node);
        node = next;
    }

    enc->pts_head = NULL;
    enc->pts_tail = NULL;
    enc->pts_count = 0;
}


static int create_tmp_file(size_t size) {
    char path[] = "/tmp/sfrec-hw-shm-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return -1; }
    unlink(path);
    if (ftruncate(fd, (off_t)size) < 0) { perror("ftruncate"); close(fd); return -1; }
    return fd;
}

int create_capture_buffer(Runner *runner, CaptureBuffer *buf) {
    memset(buf, 0, sizeof(*buf));
    int stride = runner->width * 4;
    size_t size = (size_t)stride * (size_t)runner->height;
    int fd = create_tmp_file(size);
    if (fd < 0) return 0;

    u_char *data = (u_char *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { perror("mmap"); close(fd); return 0; } memset(data, 0, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(runner->shm, fd, (int)size);
    if (!pool) { fprintf(stderr, "wl_shm_create_pool failed\n"); munmap(data, size); close(fd); return 0; }

    buf->wl_buffer = wl_shm_pool_create_buffer(pool, 0, runner->width, runner->height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    if (!buf->wl_buffer) { fprintf(stderr, "wl_shm_pool_create_buffer failed\n"); munmap(data, size); return 0; }
    wl_buffer_set_user_data(buf->wl_buffer, buf);
    buf->data = data;
    buf->size = size;
    buf->state = 0;
    return 1;
}

static int queue_push(Runner *runner, CaptureBuffer *buf) {
    CaptureQueue *q = &runner->queue;
    pthread_mutex_lock(&q->mutex);
    if (q->count >= CAPTURE_QUEUE_CAP) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }
    q->items[q->tail] = buf;
    q->tail = (q->tail + 1) % CAPTURE_QUEUE_CAP;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

static void input_unref(void *data) {
    free(data);
}

static void codec_signal_eos(void *data) {
    EncoderState *enc = (EncoderState *)data;
    pthread_mutex_lock(&enc->mutex);
    enc->eos++;
    pthread_mutex_unlock(&enc->mutex);
}

static void codec_error(void *data, int err) {
    EncoderState *enc = (EncoderState *)data;
    pthread_mutex_lock(&enc->mutex);
    enc->errors++;
    fprintf(stderr, "CODEC_ERROR %d\n", err);
    pthread_mutex_unlock(&enc->mutex);
}

static int codec_size_changed(void *data, int32_t width, int32_t height) {
    (void)data;
    printf("CODEC_SIZE_CHANGED %d %d\n", width, height);
    fflush(stdout);
    return 0;
}

static void encoded_available(void *data, DroidMediaCodecData *encoded) {
    EncoderState *enc = (EncoderState *)data;

    if (!encoded || !encoded->data.data || encoded->data.size <= 0) {
        return;
    }

    pthread_mutex_lock(&enc->mutex);

    enc->encoded_bytes += encoded->data.size;
    enc->encoded_packets++;

    /*
     * Kaynak motor her senkron kareye SPS/PPS basliklarini ekliyor.
     * Ayri codec-config paketi gelirse kare sayilmadan atlanir.
     */
    if (encoded->codec_config) {
        enc->codec_config_packets++;
        pthread_mutex_unlock(&enc->mutex);
        return;
    }

    int64_t pts_us = 0;

    if (!pts_pop_locked(enc, &pts_us)) {
        if (enc->last_pts_us >= 0) {
            pts_us = enc->last_pts_us + enc->nominal_frame_us;
        }

        fprintf(
            stderr,
            "PTS_QUEUE_EMPTY fallback=%lld\n",
            (long long)pts_us
        );
    }

    if (enc->last_pts_us >= 0 && pts_us <= enc->last_pts_us) {
        pts_us = enc->last_pts_us + 1;
    }

    enc->last_pts_us = pts_us;

    if (
        enc->mux &&
        enc->stream &&
        encoded->data.size <= INT_MAX
    ) {
        AVPacket *packet = av_packet_alloc();

        if (!packet) {
            enc->errors++;
            fprintf(stderr, "MUX_PACKET_ALLOC_ERROR\n");
        } else if (
            av_new_packet(packet, (int)encoded->data.size) < 0
        ) {
            enc->errors++;
            fprintf(stderr, "MUX_PACKET_DATA_ALLOC_ERROR\n");
        } else {
            const AVRational input_time_base = { 1, 1000000 };

            memcpy(
                packet->data,
                encoded->data.data,
                encoded->data.size
            );

            packet->stream_index = enc->stream->index;
            packet->pos = -1;

            packet->pts = av_rescale_q(
                pts_us,
                input_time_base,
                enc->stream->time_base
            );

            packet->dts = packet->pts;

            packet->duration = av_rescale_q(
                enc->nominal_frame_us,
                input_time_base,
                enc->stream->time_base
            );

            if (encoded->sync) {
                packet->flags |= AV_PKT_FLAG_KEY;
            }

            int result = av_interleaved_write_frame(
                enc->mux,
                packet
            );

            if (result < 0) {
                enc->errors++;

                fprintf(
                    stderr,
                    "MUX_WRITE_ERROR result=%d pts_us=%lld\n",
                    result,
                    (long long)pts_us
                );
            }
        }

        av_packet_free(&packet);
    }

    pthread_mutex_unlock(&enc->mutex);
}

static void *codec_loop_thread(void *arg) {
    EncoderState *enc = (EncoderState *)arg;
    enc->loop_started = 1;
    while (enc->loop_running) {
        DroidMediaCodecLoopReturn r = droid_media_codec_loop(enc->codec);
        pthread_mutex_lock(&enc->mutex);
        enc->loop_calls++;
        if (r == DROID_MEDIA_CODEC_LOOP_ERROR) {
            enc->errors++;
            enc->loop_running = 0;
        } else if (r == DROID_MEDIA_CODEC_LOOP_EOS) {
            enc->eos++;
            enc->loop_running = 0;
        }
        pthread_mutex_unlock(&enc->mutex);
        if (r != DROID_MEDIA_CODEC_LOOP_OK) break;
        usleep(1000);
    }
    return NULL;
}

static DroidMediaCodec *create_encoder(Runner *runner, int *chosen_format) {
    DroidMediaColourFormatConstants c;
    memset(&c, 0, sizeof(c));
    droid_media_colour_format_constants_init(&c);

    int formats_to_try[] = {
        21,
        c.OMX_COLOR_FormatYUV420SemiPlanar,
        c.OMX_COLOR_FormatYUV420Planar,
        c.OMX_COLOR_FormatYUV420Flexible,
        19,
        20,
        2135033992
    };

    for (unsigned int i = 0; i < sizeof(formats_to_try)/sizeof(formats_to_try[0]); i++) {
        DroidMediaCodecEncoderMetaData meta;
        memset(&meta, 0, sizeof(meta));
        meta.parent.type = "video/avc";
        meta.parent.width = runner->width;
        meta.parent.height = runner->height;
        meta.parent.fps = runner->target_fps;
        meta.parent.flags = DROID_MEDIA_CODEC_HW_ONLY;// | DROID_MEDIA_CODEC_USE_EXTERNAL_LOOP;
        meta.color_format = formats_to_try[i];
        meta.bitrate = runner->bitrate;
        meta.meta_data = 0;
        meta.stride = runner->width;
        meta.slice_height = runner->height;
        meta.max_input_size = runner->width * runner->height * 3 / 2;
        meta.bitrate_mode = DROID_MEDIA_CODEC_BITRATE_CONTROL_CBR;
        meta.codec_specific.h264.prepend_header_to_sync_frames = 1;

        printf("TRY_START_FORMAT %d\n", formats_to_try[i]);
        fflush(stdout);
        DroidMediaCodec *codec = droid_media_codec_create_encoder(&meta);
        if (!codec) continue;

        DroidMediaCodecCallbacks cb;
        memset(&cb, 0, sizeof(cb));
        cb.signal_eos = codec_signal_eos;
        cb.error = codec_error;
        cb.size_changed = codec_size_changed;
        droid_media_codec_set_callbacks(codec, &cb, &runner->enc);

        DroidMediaCodecDataCallbacks dcb;
        memset(&dcb, 0, sizeof(dcb));
        dcb.data_available = encoded_available;
        droid_media_codec_set_data_callbacks(codec, &dcb, &runner->enc);

        if (droid_media_codec_start(codec)) {
            *chosen_format = formats_to_try[i];
            return codec;
        }
        droid_media_codec_destroy(codec);
    }
    return NULL;
}

int encoder_start(Runner *runner) {
    /* droidmedia stays initialized for the whole daemon lifetime:
     * Deinit happens only at cleanup() (daemon shutdown). */
    if (!runner->droid_inited) {
        if (!droid_media_init()) {
            set_error("encoder_start: droid_media_init failed");
            printf("RESULT_DROID_INIT 0\n");
            return 0;
        }
        runner->droid_inited = 1;
    }
    pthread_mutex_init(&runner->enc.mutex, NULL);
    runner->enc.codec = create_encoder(runner, &runner->enc.chosen_format);

    if (!runner->enc.codec) {
        set_error("encoder_start: no usable encoder format"); return 0;
    }

    printf("RESULT_CHOSEN_FORMAT %d\n", runner->enc.chosen_format);

    runner->enc.last_pts_us = -1;
    runner->enc.nominal_frame_us =
        runner->target_fps > 0
        ? 1000000LL / runner->target_fps
        : 16667LL;

    int mux_result = avformat_alloc_output_context2(
        &runner->enc.mux,
        NULL,
        "mpegts",
        runner->out_path
    );

    if (mux_result < 0 || !runner->enc.mux) {
        fprintf(
            stderr,
            "ERROR mux context result=%d\n",
            mux_result
        );
        set_error("encoder_start: mux context alloc failed");
        return 0;
    }

    runner->enc.stream = avformat_new_stream(
        runner->enc.mux,
        NULL
    );

    if (!runner->enc.stream) {
        fprintf(stderr, "ERROR mux stream create\n");
        avformat_free_context(runner->enc.mux);
        runner->enc.mux = NULL;
        return 0;
    }

    runner->enc.stream->time_base =
        (AVRational){ 1, 1000 };

    runner->enc.stream->avg_frame_rate =
        (AVRational){ runner->target_fps, 1 };

    runner->enc.stream->r_frame_rate =
        (AVRational){ runner->target_fps, 1 };

    runner->enc.stream->codecpar->codec_type =
        AVMEDIA_TYPE_VIDEO;

    runner->enc.stream->codecpar->codec_id =
        AV_CODEC_ID_H264;

    runner->enc.stream->codecpar->codec_tag = 0;
    runner->enc.stream->codecpar->width = runner->width;
    runner->enc.stream->codecpar->height = runner->height;
    runner->enc.stream->codecpar->bit_rate = runner->bitrate;

    if (!(runner->enc.mux->oformat->flags & AVFMT_NOFILE)) {
        mux_result = avio_open(
            &runner->enc.mux->pb,
            runner->out_path,
            AVIO_FLAG_WRITE
        );

        if (mux_result < 0) {
            fprintf(
                stderr,
                "ERROR mux file open result=%d\n",
                mux_result
            );

            avformat_free_context(runner->enc.mux);
            runner->enc.mux = NULL;
            runner->enc.stream = NULL;
            set_error("encoder_start: mux file open failed");
            return 0;
        }
    }

    mux_result = avformat_write_header(
        runner->enc.mux,
        NULL
    );

    if (mux_result < 0) {
        fprintf(
            stderr,
            "ERROR mux header result=%d\n",
            mux_result
        );

        if (!(runner->enc.mux->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&runner->enc.mux->pb);
        }

        avformat_free_context(runner->enc.mux);
        runner->enc.mux = NULL;
        runner->enc.stream = NULL;
        set_error("encoder_start: mux header write failed");
        return 0;
    }

    printf("RESULT_MUX_STARTED MPEGTS\n");
    fflush(stdout);

    runner->enc.loop_running = 1;
    if (pthread_create(&runner->enc.loop_thread, NULL, codec_loop_thread, &runner->enc) != 0) {
        perror("pthread_create codec loop");
        set_error("encoder_start: codec loop thread failed");
        return 0;
    }
    for (int wait = 0; wait < 100 && !runner->enc.loop_started; wait++) usleep(10000);
    return 1;
}

void encoder_stop(Runner *runner) {
    if (runner->enc.codec) {
        droid_media_codec_drain(runner->enc.codec);
        double start = now_seconds();
        while (now_seconds() - start < 2.0) {
            pthread_mutex_lock(&runner->enc.mutex);
            int eos = runner->enc.eos;
            int err = runner->enc.errors;
            pthread_mutex_unlock(&runner->enc.mutex);
            if (eos > 0 || err > 0) break;
            usleep(20000);
        }
        runner->enc.loop_running = 0;
        droid_media_codec_drain(runner->enc.codec);
        pthread_join(runner->enc.loop_thread, NULL);
    }
    pthread_mutex_lock(&runner->enc.mutex);

    if (runner->enc.mux) {
        int trailer_result = av_write_trailer(
            runner->enc.mux
        );

        if (trailer_result < 0) {
            fprintf(
                stderr,
                "MUX_TRAILER_ERROR result=%d\n",
                trailer_result
            );
        }

        if (!(runner->enc.mux->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&runner->enc.mux->pb);
        }

        avformat_free_context(runner->enc.mux);
        runner->enc.mux = NULL;
        runner->enc.stream = NULL;
    }

    pts_clear_locked(&runner->enc);

    if (runner->enc.out) {
        fclose(runner->enc.out);
    }

    runner->enc.out = NULL;

    pthread_mutex_unlock(&runner->enc.mutex);
    if (runner->enc.codec) {
        droid_media_codec_stop(runner->enc.codec);
        droid_media_codec_destroy(runner->enc.codec);
        runner->enc.codec = NULL;
    }
    /* NOTE: droid_media_deinit() intentionally NOT called here.
     * The daemon re-uses droidmedia for the next recording; it is
     * deinitialized in cleanup() when the daemon shuts down. */
}


static int encoder_queue_frame(Runner *runner, uint8_t *nv12, int64_t pts_us, int sync) {
    DroidMediaCodecData in;
    memset(&in, 0, sizeof(in));
    in.data.data = nv12;
    in.data.size = runner->width * runner->height * 3 / 2;
    in.ts = pts_us;
    in.decoding_ts = pts_us;
    in.sync = sync;
    in.codec_config = false;

    DroidMediaBufferCallbacks bcb;
    memset(&bcb, 0, sizeof(bcb));
    bcb.unref = input_unref;
    bcb.data = nv12;

    if (!pts_push(&runner->enc, pts_us)) {
        pthread_mutex_lock(&runner->enc.mutex);
        runner->enc.errors++;
        pthread_mutex_unlock(&runner->enc.mutex);

        fprintf(stderr, "PTS_QUEUE_PUSH_ERROR\n");
    }

    droid_media_codec_queue(runner->enc.codec, &in, &bcb);
    return 1;
}

void *worker_thread(void *arg) {
    Runner *runner = (Runner *)arg;
    CaptureQueue *q = &runner->queue;
    int frame_size = runner->width * runner->height * 3 / 2;

    while (1) {
        pthread_mutex_lock(&q->mutex);
        while (!q->stop && q->count == 0) pthread_cond_wait(&q->cond, &q->mutex);
        if (q->stop && q->count == 0) { pthread_mutex_unlock(&q->mutex); break; }
        CaptureBuffer *buf = q->items[q->head];
        q->head = (q->head + 1) % CAPTURE_QUEUE_CAP;
        q->count--;
        pthread_mutex_unlock(&q->mutex);

        if (buf && buf->data) {
            uint8_t *nv12 = (uint8_t *)malloc((size_t)frame_size);
            if (nv12) {
                convert_rgba_to_nv12(nv12, buf->data, runner->width, runner->height, runner->stride, buf->transform);
                uint32_t elapsed_ms =
                    buf->capture_time_ms -
                    runner->first_lipstick_time;

                int64_t pts_us =
                    (int64_t)elapsed_ms * 1000LL;
                encoder_queue_frame(runner, nv12, pts_us, runner->frames_sent_encoder == 0);
                runner->frames_sent_encoder++;
                runner->frames_converted++;
            }
            buf->state = 0;
        }
    }
    return NULL;
}

void request_one(Runner *runner) {
    if (!runner->running || !runner->recorder) return;
    if (runner->in_flight > 0) return;
    for (int i = 0; i < CAPTURE_BUFFER_COUNT; i++) {
        if (runner->buffers[i].state == 0 && runner->buffers[i].wl_buffer) {
            runner->buffers[i].state = 1;
            runner->buffers[i].seq = ++runner->seq_next;
            runner->in_flight++;
            lipstick_recorder_record_frame(runner->recorder, runner->buffers[i].wl_buffer);
            wl_display_flush(runner->display);
            return;
        }
    }
    runner->frames_dropped_no_buffer++;
}

static void recorder_setup(void *data, struct lipstick_recorder *recorder, int32_t width, int32_t height, int32_t stride, int32_t format) {
    (void)recorder;
    Runner *runner = (Runner *)data;
    printf("SETUP width=%d height=%d stride=%d format=%d\n", width, height, stride, format);
    if (width > 0 && height > 0) {
        runner->width = width;
        runner->height = height;
        runner->stride = stride; }
    fflush(stdout);
}

static void recorder_frame(void *data, struct lipstick_recorder *recorder, struct wl_buffer *buffer, uint32_t time, int32_t transform) {
    (void)recorder;
    Runner *runner = (Runner *)data;
    CaptureBuffer *buf = (CaptureBuffer *)wl_buffer_get_user_data(buffer);
    if (buf && buf->state == 1 && runner->in_flight > 0) runner->in_flight--;

    runner->frames_captured++;
    if (runner->last_lipstick_time != 0) {
        uint32_t delta = time - runner->last_lipstick_time;
        if (runner->delta_count == 0 || delta < runner->min_delta) runner->min_delta = delta;
        if (runner->delta_count == 0 || delta > runner->max_delta) runner->max_delta = delta;
        runner->sum_delta += delta;
        runner->delta_count++;
    }
    if (!runner->have_first_lipstick_time) {
        runner->first_lipstick_time = time;
        runner->have_first_lipstick_time = 1;

        runner->first_capture_monotonic_us =
            now_monotonic_us();

        printf(
            "RESULT_FIRST_CAPTURE_MONOTONIC_US %lld\n",
            (long long)runner->first_capture_monotonic_us
        );

        fflush(stdout);
    }

    runner->last_lipstick_time = time;

    if (buf) {
        buf->capture_time_ms = time;
        buf->transform = transform;
        buf->state = 2;
        if (!queue_push(runner, buf)) {
            runner->frames_dropped_queue++;
            buf->state = 0;
        }
    }

    double elapsed = now_seconds() - runner->start_seconds;
    if (elapsed - runner->last_progress >= 1.0 || runner->frames_captured == 1) {
        pthread_mutex_lock(&runner->enc.mutex);
        long long bytes = runner->enc.encoded_bytes;
        int packets = runner->enc.encoded_packets;
        int errs = runner->enc.errors;
        pthread_mutex_unlock(&runner->enc.mutex);
        pthread_mutex_lock(&runner->queue.mutex);
        int qcount = runner->queue.count;
        pthread_mutex_unlock(&runner->queue.mutex);
        printf("PROGRESS cap=%llu enc_in=%llu packets=%d bytes=%lld fps=%.2f q=%d dropq=%llu err=%d\n",
               (unsigned long long)runner->frames_captured,
               (unsigned long long)runner->frames_sent_encoder,
               packets, bytes, elapsed > 0 ? runner->frames_captured / elapsed : 0.0,
               qcount, (unsigned long long)runner->frames_dropped_queue, errs);
        fflush(stdout);
        runner->last_progress = elapsed;
    }

    if (runner->seconds > 0 && elapsed >= (double)runner->seconds) { runner->running = 0; return; }
    request_one(runner);
}

static void recorder_failed(void *data, struct lipstick_recorder *recorder, int32_t result, struct wl_buffer *buffer) {
    (void)recorder;
    CaptureBuffer *buf = (CaptureBuffer *)wl_buffer_get_user_data(buffer);
    if (buf && buf->state) buf->state = 0;
    Runner *runner = (Runner *)data;
    if (runner->in_flight > 0) runner->in_flight--;
    fprintf(stderr, "RECORDER_FAILED result=%d\n", result);
    runner->failed = 1;
    runner->running = 0;
}

static void recorder_cancelled(void *data, struct lipstick_recorder *recorder, struct wl_buffer *buffer) {
    (void)recorder;
    CaptureBuffer *buf = (CaptureBuffer *)wl_buffer_get_user_data(buffer);
    if (buf && buf->state == 1) buf->state = 0;
    Runner *runner = (Runner *)data;
    if (runner->in_flight > 0) runner->in_flight--;
    runner->cancelled++;
}


const struct lipstick_recorder_listener recorder_listener = { recorder_setup, recorder_frame, recorder_failed, recorder_cancelled };

static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char *make, const char *model, int32_t transform) {
    (void)data;
    (void)output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void)output; Runner *runner = (Runner *)data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        runner->width = width;
        runner->height = height;
        runner->stride = width * 4;
        printf("OUTPUT current width=%d height=%d refresh_mHz=%d\n", width, height, refresh); }
}

static void output_done(void *data, struct wl_output *output) { (void)data; (void)output; }
static void output_scale(void *data, struct wl_output *output, int32_t factor) { (void)data; (void)output; (void)factor; }
static void output_name(void *data, struct wl_output *output, const char *name) { (void)data; (void)output; (void)name; }

static void output_description(void *data, struct wl_output *output, const char *description) {
    (void)data;
    (void)output;
    (void)description;
}
const struct wl_output_listener output_listener = { output_geometry, output_mode, output_done, output_scale, output_name, output_description };

/* this was cleaner in screenrecorder
 * revisit
 */
static void registry_global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
    Runner *runner = (Runner *)data;
    if (strcmp(interface, "wl_shm") == 0) {
        runner->shm = (struct wl_shm *)wl_registry_bind(registry, id, &wl_shm_interface, version < 1 ? version : 1); printf("FOUND wl_shm\n");
    } else if (strcmp(interface, "wl_output") == 0 && !runner->output) {
        uint32_t bind_ver = version < 4 ? version : 4;
        runner->output = (struct wl_output *)wl_registry_bind(registry, id, &wl_output_interface, bind_ver);
        wl_output_add_listener(runner->output, &output_listener, runner); printf("FOUND wl_output version=%u\n", version);
    } else if (strcmp(interface, "lipstick_recorder_manager") == 0) {
        runner->manager = (struct lipstick_recorder_manager *)wl_registry_bind(registry, id, &lipstick_recorder_manager_interface, version < 1 ? version : 1); printf("FOUND lipstick_recorder_manager version=%u\n", version);
    }
    fflush(stdout);
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t id) {
    (void)data;
    (void)registry;
    (void)id;
}

const struct wl_registry_listener registry_listener = { registry_global, registry_remove };

void stop_worker(Runner *runner) {
    if (!runner->worker_started) return;
    pthread_mutex_lock(&runner->queue.mutex);
    runner->queue.stop = 1;
    pthread_cond_signal(&runner->queue.cond);
    pthread_mutex_unlock(&runner->queue.mutex);
    pthread_join(runner->worker_thread, NULL);
    runner->worker_started = 0;
}

void cleanup(Runner *runner) {
    stop_worker(runner);
    encoder_stop(runner);
    for (int i = 0; i < CAPTURE_BUFFER_COUNT; i++) {
        if (runner->buffers[i].wl_buffer) wl_buffer_destroy(runner->buffers[i].wl_buffer);
        if (runner->buffers[i].data && runner->buffers[i].size) munmap(runner->buffers[i].data, runner->buffers[i].size);
    }
    if (runner->recorder) lipstick_recorder_destroy(runner->recorder);
    if (runner->manager) lipstick_recorder_manager_destroy(runner->manager);
    if (runner->output) wl_output_destroy(runner->output);
    if (runner->shm) wl_shm_destroy(runner->shm);
    if (runner->registry) wl_registry_destroy(runner->registry);
    if (runner->display) wl_display_disconnect(runner->display);
    pthread_mutex_destroy(&runner->queue.mutex);
    pthread_cond_destroy(&runner->queue.cond);
    pthread_mutex_destroy(&runner->enc.mutex);
    if (runner->droid_inited) droid_media_deinit();
    free((void *)runner->out_path);
    runner->out_path = NULL;
}


void print_result(Runner *runner) {
    double elapsed = now_seconds() - runner->start_seconds;
    pthread_mutex_lock(&runner->enc.mutex);
    long long bytes = runner->enc.encoded_bytes;
    int packets = runner->enc.encoded_packets;
    int cfg = runner->enc.codec_config_packets;
    int errs = runner->enc.errors;
    int eos = runner->enc.eos;
    long long loops = runner->enc.loop_calls;
    pthread_mutex_unlock(&runner->enc.mutex);
    pthread_mutex_lock(&runner->queue.mutex);
    int qleft = runner->queue.count;
    pthread_mutex_unlock(&runner->queue.mutex);

    printf("RESULT_FILE %s\n", runner->out_path);
    printf("RESULT_SECONDS %.3f\n", elapsed);
    printf("RESULT_CAPTURED_FRAMES %llu\n", (unsigned long long)runner->frames_captured);
    printf("RESULT_CAPTURE_AVG_FPS %.2f\n", elapsed > 0 ? runner->frames_captured / elapsed : 0.0);
    printf("RESULT_SENT_ENCODER %llu\n", (unsigned long long)runner->frames_sent_encoder);
    printf("RESULT_CONVERTED %llu\n", (unsigned long long)runner->frames_converted);
    printf("RESULT_DROPPED_QUEUE %llu\n", (unsigned long long)runner->frames_dropped_queue);
    printf("RESULT_DROPPED_NO_BUFFER %llu\n", (unsigned long long)runner->frames_dropped_no_buffer);
    printf("RESULT_QUEUE_LEFT %d\n", qleft);
    printf("RESULT_LOOP_CALLS %lld\n", loops);
    printf("RESULT_ENCODED_PACKETS %d\n", packets);
    printf("RESULT_CODEC_CONFIG_PACKETS %d\n", cfg);
    printf("RESULT_ENCODED_BYTES %lld\n", bytes);
    printf("RESULT_ERRORS %d\n", errs);
    printf("RESULT_EOS %d\n", eos);
    printf("RESULT_CANCELLED %llu\n", (unsigned long long)runner->cancelled);
    printf("RESULT_FAILED %d\n", runner->failed);
    if (runner->delta_count > 0) {
        double avg_delta = (double)runner->sum_delta / (double)runner->delta_count;
        printf("RESULT_LIPSTICK_DELTA_MS avg=%.2f min=%u max=%u samples=%llu\n", avg_delta, runner->min_delta, runner->max_delta, (unsigned long long)runner->delta_count);
    }
    printf("RESULT_OK %d\n", (bytes > 0 && packets > 0 && errs == 0 && !runner->failed) ? 1 : 0);
}

 /*
 * The Wayland connection, recorder global and shared-memory buffers
 * are created once, on the first recording start, remain for consequent starts
 */

int droidrec_prepare(void)
{
    /* Bring the backend up once (display, recorder global, capture buffers).
     * Lets the Gui report "Ready" before the first recording. */
    if (s_runner) return 0;

    s_runner = (Runner *)calloc(1, sizeof(Runner));
    if (!s_runner) {
        set_error("droidrec_prepare: out of memory");
        return -1;
    }
    if (!init_display(s_runner)) {
        set_error("droidrec_prepare: display/recorder setup failed");
        free(s_runner);
        s_runner = NULL;
        return -1;
    }
    set_error("ok");
    printf("droidrec: prepared\n");
    fflush(stdout);
    return 0;
}

/* called from recorder.cpp, the main start */
int droidrec_start(const DroidRecOptions *opts)
{
    if (!opts) {
        set_error("droidrec_start: no options");
        return -1;
    }

    if (s_runner && s_runner->running) {
        set_error("droidrec_start: already recording");
        return -1;
    }

    if (!s_runner) {
        s_runner = (Runner *)calloc(1, sizeof(Runner));
        if (!s_runner) {
            set_error("droidrec_start: out of memory");
            return -1;
        }

        if (!init_display(s_runner)) {
            set_error("droidrec_start: display/recorder setup failed");
            free(s_runner);
            s_runner = NULL;
            return -1;
        }
    }

    /* Ownership of the buffer lives here and is released in cleanup(). */
    /* filenaming is the same as in screenrecorder */

    Runner *runner = s_runner;
    {
        // the path can't actually be undefined?
        const char *want = opts->out_path ? opts->out_path : "/home/defaultuser/Videos/tailwind.ts";
        char *dup = strdup(want);
        if (dup) {
            free((void *)runner->out_path);
            runner->out_path = dup;
        } else {
            runner->out_path = want; /* strdup failed (OOM); keep the pointer */
        }
    }
    runner->target_fps = opts->target_fps > 0 ? opts->target_fps : 24;
    runner->bitrate    = opts->bitrate > 0 ? opts->bitrate : DEFAULT_BITRATE;
    runner->seconds    = opts->seconds;  /* 0 = record until droidrec_stop() */
    /* this is currently backwards. We should use a user option to specify
     * maximum recording length when no intervention takes place
     */
    if (opts->width > 0 && opts->height > 0) {
        runner->width  = opts->width;
        runner->height = opts->height;
        runner->stride = runner->width * 4;
    }

    printf("ARGS out=%s seconds=%d target_fps=%d bitrate=%d\n",
           runner->out_path, runner->seconds, runner->target_fps, runner->bitrate);

    reset_recording_counters(runner);
    memset(&runner->enc, 0, sizeof(runner->enc));

    runner->start_seconds = now_seconds();

    if (!encoder_start(runner)) {
        /* encoder_start() sets a specific error string; keep it. */
        return -1;
    }

    if (pthread_create(&runner->worker_thread, NULL, worker_thread, runner) != 0) {
        encoder_stop(runner);
        set_error("droidrec_start: worker thread failed");
        return -1;
    }
    runner->worker_started = 1;

    runner->running = 1;
    if (pthread_create(&runner->event_thread, NULL, event_loop_thread, runner) != 0) {
        runner->running = 0;
        stop_worker(runner);
        encoder_stop(runner);
        set_error("droidrec_start: event loop thread failed");
        return -1;
    }
    runner->event_started = 1;

    set_error("ok");
    printf("droidrec: recording started\n");
    fflush(stdout);
    return 0;
}

int droidrec_stop(void)
{
    if (!s_runner) {
        set_error("droidrec_stop: no recorder instance");
        return -1;
    }

    Runner *runner = s_runner;
    if (!runner->running) {
        set_error("droidrec_stop: not recording");
        return -1;
    }

    runner->running = 0;

    if (runner->event_started) {
        pthread_join(runner->event_thread, NULL);
        runner->event_started = 0;
    }

    stop_worker(runner);

    int had_output = (runner->enc.encoded_bytes > 0 || runner->enc.encoded_packets > 0);

    /* Drain the encoder, write the trailer and release the codec/muxer. */
    encoder_stop(runner);

    print_result(runner);
    fflush(stdout);

    set_error(had_output ? "ok" : "droidrec_stop: no frames were captured");
    return had_output ? 0 : -1;
}

void droidrec_shutdown(void)
{
    if (!s_runner) return;
    if (s_runner->running) droidrec_stop();
    cleanup(s_runner);
    free(s_runner);
    s_runner = NULL;
}

int droidrec_is_recording(void)
{
    return (s_runner && s_runner->running) ? 1 : 0;
}

int droidrec_state(void)
{
    if (!s_runner) return DROIDREC_STATE_IDLE;
    if (s_runner->running) return DROIDREC_STATE_RECORDING;
    return DROIDREC_STATE_READY;
}

const char *droidrec_last_error(void)
{
    return s_last_error;
}
