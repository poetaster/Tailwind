/*
 * TailWind - Qt wrapper around the droidrec capture/encode backend.
 *
 * This is a thin QObject facade over the record_stream C API declared in
 * droidrec.h. It does not talk to Wayland or droidmedia directly: those
 * responsibilities live entirely in droidrec.c (which owns its own event
 * loop, worker and encoder threads). The Recorder's job is to:
 *
 *   - hold the current recording options (destination, fps, bitrate, ...)
 *   - expose a Qt status enum + statusChanged() signal
 *   - translate start()/stop() into droidrec_start()/droidrec_stop()
 *   - report the finished file via recordingFinished(QString)
 */

#ifndef TAILWIND_RECORDER_H
#define TAILWIND_RECORDER_H

#include <QObject>
#include <QMutex>
#include <QWaitCondition>

#include <thread>

class Recorder : public QObject
{
    Q_OBJECT
public:
    struct Options {
        QString tempdir;     /* scratch dir the recording is written into while capturing */
        QString destination; /* final dir the finished file is moved to on stop (~/Videos) */
        int     fps;         /* 0 = backend default (24) */
        int     bitrate;     /* bits per second, 0 = backend default */
        int     seconds;     /* 0 = record until stop() is called */
        int     width;       /* 0 = native output mode */
        int     height;      /* 0 = native output mode */
        QString audioSink;   /* pulse SINK name; its .monitor is recorded; empty = no audio */
    };

    enum Status {
        StatusIdle = 0,      /* backend not initialised yet */
        StatusReady = 1,     /* ready to start a recording */
        StatusRecording = 2, /* actively capturing + encoding */
        StatusSaving = 3,    /* draining the encoder, writing the trailer */
    };
    Q_ENUM(Status)

    explicit Recorder(QObject *parent = nullptr);
    ~Recorder() override;

    static Recorder *instance();

    Status  status() const;
    void    setStatus(Status status);

    Options options() const;
    void    setOptions(const Options &options);

    /* Sensible defaults: native resolution, 24 fps, backend-default bitrate,
     * record into the temp dir until stopped, then move to the destination
     * (~/Videos) so the gallery never sees a half-written file. */
    static Options readOptions();

signals:
    /* NOTE:  need to examine again, but
     * int, not Status. This signal is emitted from a worker thread
     * (droidrec runs off the event loop), so the connection is queued.
     * Qt 5.6 cannot queue unregistered enum types and drops the signal
     * silently; int is a built-in meta type and always works. */
    void statusChanged(int status);
    void recordingFinished(const QString &fileName);

    /* Emitted once a Stop has fully completed (droidrec_stop() returned, the
     * encoder is drained, the file is saved) AND this was a user-initiated
     * Stop -- i.e. we are not already inside handleShutDown() from a SIGTERM
     * or the D-Bus Quit() call, which own the teardown themselves.
     *
     * main() connects this to wind the daemon down and exit, so the process
     * serves one recording and then goes away; the GUI re-activates it on the
     * next Start (D-Bus/systemd). That keeps the daemon from holding stale or
     * stuck state across recordings. */
    void shutdownRequested();

public slots:
    /* Initialises the backend on first use; safe to call repeatedly. */
    void init();
    /* Begin a recording. Returns true on success. */
    bool start();
    /* Stop a recording and return the finished file path ("" on failure). */
    QString stop();
    /* Final shutdown: stops any active recording and releases the backend. */
    void handleShutDown();

private:
    Status  m_status = StatusIdle;
    mutable QMutex m_mutex;        /* guards m_status + the in-progress flags */
    bool    m_startInProgress = false;
    bool    m_stopInProgress  = false;
    bool    m_shutdown        = false;
    QWaitCondition m_opCond;      /* signalled when a start/stop worker finishes */
    /* Owned (NOT detached) workers, joined in ~Recorder(): a detached worker
     * capturing `this` would outlive the app-owned Recorder if the process
     * quit mid start/stop, and its first member access after the long
     * droidrec_start()/droidrec_stop() call would be a use-after-free. */
    std::thread m_startWorker;
    std::thread m_stopWorker;
    Options m_options;
    /* persistent var backing droidrec's out_path, probably redundant */
    QString m_currentPath;

    friend class DBusAdaptor; // this is uncessarry
};

#endif /* TAILWIND_RECORDER_H */
