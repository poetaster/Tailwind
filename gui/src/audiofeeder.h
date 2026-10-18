/*
 * AudioFeeder - captures audio with a direct libpulse client inside the
 * TailWind GUI app and appends the raw PCM to a shared file that the
 * daemon tails into the AAC encoder.
 *
 * Why not QAudioInput: Qt 5.6.3's PulseAudio backend delivered crackly,
 * corrupted audio in every test, while a direct libpulse capture of the
 * same source at the same rate (parecord) was clean - even while video
 * was recording. The corruption is inside Qt's delivery path, so we
 * bypass it and drive libpulse ourselves, with the same pattern
 * parecord uses.
 *
 * Transport: a file, not D-Bus. A synchronous D-Bus round-trip per 8 KB
 * chunk stalls the capture pump and the recording crackles; appending to
 * a local file is just a write(2) and keeps up at ~200 KB/s with headroom.
 *
 * Contract with the daemon:
 *  - prepare(dir) creates/truncates the PCM file in dir and returns
 *    its path;
 *  - the QML calls the daemon's StartAudio() (no arguments - the path
 *    is derived on both sides: here from the dir the QML passes, on
 *    the daemon from the dconf "tempdir" key the QML publishes; the
 *    two are the same dir, so nothing crosses D-Bus) and then calls
 *    start();
 *  - stop() closes the file (flushing) BEFORE the QML sends EndAudio, so
 *    the daemon's tailer sees a clean end of file. stop() blocks until
 *    the capture thread has fully wound down, so that ordering holds.
 *
 * Format: S16LE, 48 kHz, stereo, interleaved - what the daemon's AAC
 * encoder expects. PulseAudio resamples server-side if the source's
 * native rate differs (parecord at 48 kHz proves that is clean on the
 * device).
 *
 * Threading: a pa_threaded_mainloop runs the capture on its own thread;
 * the stream's read callback drains the Pulse stream buffer straight to
 * the file (pa_stream_peek/pa_stream_drop, like parecord) - no Qt pump
 * and no intermediate buffer to underflow or corrupt. Public methods
 * run on the GUI thread. Follows the StreamFilter class' proven
 * libpulse idiom (this project's headers use the legacy libpulse API:
 * PA_STREAM_READY, request-style read callback, pa_operation*).
 */

#ifndef AUDIOFEEDER_H
#define AUDIOFEEDER_H

#include <QObject>
#include <QByteArray>
#include <QMutex>
#include <QWaitCondition>
#include <QElapsedTimer>
#include <QString>

struct pa_context;
struct pa_stream;
struct pa_threaded_mainloop;

class AudioFeeder : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(qlonglong bytes READ bytes NOTIFY bytesChanged)
    Q_PROPERTY(QString output READ output NOTIFY outputChanged)

public:
    explicit AudioFeeder(QObject *parent = nullptr);
    ~AudioFeeder() override;

    /* Create/truncate the PCM file the daemon will tail; returns its
     * path, or an empty string on failure (see error()). dir is the
     * scratch dir (the app's cache dir; the daemon resolves the same
     * one from the dconf "tempdir" key) - empty = our cache location.
     * Call before start(); the daemon's StartAudio() (no args) opens
     * the same path. */
    Q_INVOKABLE QString prepare(const QString &dir);

    /* Begin capture. sinkName is "mic" for the microphone, or a Pulse
     * sink name to monitor (name + ".monitor"). Blocks until the stream
     * is live or the failure is final; returns false with error() set on
     * failure. The PCM file must have been prepared first. */
    Q_INVOKABLE bool start(const QString &sinkName);

    /* Wind down capture and close the PCM file. Idempotent: safe to call
     * when capture never started (closes the prepared file) or twice. */
    Q_INVOKABLE void stop();

    bool running() const;
    QString error() const;
    qlonglong bytes() const;
    QString output() const;

signals:
    void runningChanged();
    void errorChanged();
    void bytesChanged();
    void outputChanged();
    void stopped(const QString &errorText);

private:
    /* GUI-thread helpers */
    bool sourceExists(const QString &name);
    void windDown();                 /* pulse teardown + file close */
    void closeFd();                  /* final-size log + close(2) */
    void failStart(const QString &err);

    /* Pulse callbacks (pa_threaded_mainloop thread) */
    static void contextStateCallback(pa_context *c, void *userdata);
    static void streamStateCallback(pa_stream *s, void *userdata);
    static void readCallback(pa_stream *s, size_t nbytes, void *userdata);
    static void sourceInfoCallback(pa_context *c, const struct pa_source_info *info,
                                   int eol, void *userdata);

    /* --- state --- */
    QString m_sinkName;
    QString m_outputPath;
    QString m_error;
    qlonglong m_bytes = 0;         /* written by the capture thread; 64-bit
                                     loads are atomic on the target */
    qlonglong m_emitFloor = 0;     /* bytesChanged() throttling */
    bool m_running = false;        /* capture live (GUI-thread view) */
    bool m_stoppedEmitted = false;
    bool m_writeErrorLogged = false;
    QElapsedTimer m_statsClock;    /* wall clock for the delivery-rate log */
    qlonglong m_statsBytes = 0;    /* delivered count at the last stats line */

    int m_fd = -1;                 /* the PCM file the daemon tails (raw fd) */

    /* pulse objects: created and released on the GUI thread, used from
     * the mainloop thread; only windDown() frees them, and only after
     * the mainloop thread has delivered the final state callback */
    pa_threaded_mainloop *m_mainloop = nullptr;
    pa_context *m_context = nullptr;
    pa_stream *m_stream = nullptr;
    bool m_mainloopStarted = false;

    /* handshake flags with the mainloop thread (guarded by m_lock) */
    QMutex m_lock;
    QWaitCondition m_ctxReady;
    QWaitCondition m_streamUp;
    QWaitCondition m_sourceQuery;
    QWaitCondition m_woundDown;
    bool m_ctxDecided = false;     /* context reached READY or FAILED */
    bool m_ctxOk = false;          /* ...and it was READY */
    bool m_streamDecided = false;  /* stream reached READY or FAILED */
    bool m_streamOk = false;       /* ...and it was READY */
    bool m_sourceDecided = false;  /* source-name query answered */
    bool m_sourceFound = false;
    bool m_woundDownFlag = false;  /* context fully disconnected */

    QByteArray m_queryUtf8;        /* must outlive the async source query */
};

#endif /* AUDIOFEEDER_H */
