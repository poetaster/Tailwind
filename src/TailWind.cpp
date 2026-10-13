#ifdef QT_QML_DEBUG
#include <QtQuick>
#endif

#include <sailfishapp.h>
#include "droidrec.h"


int main(int argc, char *argv[])
{
    // SailfishApp::main() will display "qml/TailWind.qml", if you need more
    // control over initialization, you can use:
    //
    //   - SailfishApp::application(int, char *[]) to get the QGuiApplication *
    //   - SailfishApp::createView() to get a new QQuickView * instance
    //   - SailfishApp::pathTo(QString) to get a QUrl to a resource file
    //   - SailfishApp::pathToMainQml() to get a QUrl to the main QML file
    //
    // To display the view, call "show()" (will show fullscreen on device).
    //return SailfishApp::main(argc, argv);
    App app;
    memset(&app, 0, sizeof(app));

    //app.out_path = argc > 1 ? argv[1] : "/tmp/sail-recorder-video.ts";
    app.out_path = "/home/defaultuser/Videos/sail-recorder-video.ts";
    //app.seconds = parse_int(argc, argv, 2, 5, 1, 120);
    app.seconds = 20;
    //app.target_fps = parse_int(argc, argv, 3, 45, 1, 60);
    app.target_fps = 24;
    //app.bitrate = parse_int(argc, argv, 4, DEFAULT_BITRATE, 1000000, 80000000);
    app.bitrate = 50000000;
    app.width = 1088;
    app.height = 1920;
    app.stride = app.width * 4;
    pthread_mutex_init(&app.queue.mutex, NULL);
    pthread_cond_init(&app.queue.cond, NULL);

    printf("ARGS out=%s seconds=%d target_fps=%d bitrate=%d\n", app.out_path, app.seconds, app.target_fps, app.bitrate);
    printf("ENV XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s\n", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(null)", getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(null)");

    app.display = wl_display_connect(NULL);
    if (!app.display) {
        fprintf(stderr, "ERROR wl_display_connect failed\n");
        cleanup(&app); return 1;
    }

    app.registry = wl_display_get_registry(app.display);

    wl_registry_add_listener(app.registry, &registry_listener, &app);
    wl_display_roundtrip(app.display);
    wl_display_roundtrip(app.display);

    if (!app.shm || !app.output || !app.manager) {
        fprintf(stderr, "ERROR missing globals\n");
        cleanup(&app); return 2;
    }

    printf("USING width=%d height=%d stride=%d\n", app.width, app.height, app.stride);
    app.recorder = lipstick_recorder_manager_create_recorder(app.manager, app.output);

    if (!app.recorder) {
        fprintf(stderr, "ERROR create_recorder failed\n");
        cleanup(&app); return 3;
    }
    lipstick_recorder_add_listener(app.recorder, &recorder_listener, &app);
    wl_display_roundtrip(app.display);

    for (int i = 0; i < CAPTURE_BUFFER_COUNT; i++) {
        if (!create_capture_buffer(&app, &app.buffers[i])) {
            fprintf(stderr, "ERROR create_capture_buffer %d failed\n", i);
            cleanup(&app);
            return 4;
        }
    }

    if (!encoder_start(&app)) {
        fprintf(stderr, "ERROR encoder_start failed\n");
        cleanup(&app);
        return 5;
    }

    if (pthread_create(&app.worker_thread, NULL, worker_thread, &app) != 0) {
        perror("pthread_create worker");
        cleanup(&app);
        return 6;
    }
    app.worker_started = 1;

    app.running = 1;
    app.start_seconds = now_seconds();
    app.last_progress = -1.0;

    int fd = wl_display_get_fd(app.display);
    double next_tick = app.start_seconds;
    double period = 1.0 / (double)app.target_fps;

    while (app.running) {
        double now = now_seconds();
        if (now >= next_tick) {
            lipstick_recorder_repaint(app.recorder);
            request_one(&app);
            next_tick += period;
            if (now - next_tick > 1.0) next_tick = now;
        }
        wl_display_dispatch_pending(app.display);
        wl_display_flush(app.display);
        struct pollfd pfd;
        pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
        int prc = poll(&pfd, 1, 5);
        if (prc > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_dispatch(app.display) == -1) {
                perror("wl_display_dispatch");
                break;
            }
        }
        if (now_seconds() - app.start_seconds >= (double)app.seconds) app.running = 0;
    }

    stop_worker(&app);
    print_result(&app);
    cleanup(&app);
    return 0;
}
