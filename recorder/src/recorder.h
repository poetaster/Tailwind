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

class Recorder : public QObject
{
    Q_OBJECT
public:
    struct Options {
        QString destination; /* full output path (MPEG-TS under the Videos dir) */
        int     fps;         /* 0 = backend default (24) */
        int     bitrate;     /* bits per second, 0 = backend default */
        int     seconds;     /* 0 = record until stop() is called */
        int     width;       /* 0 = native output mode */
        int     height;      /* 0 = native output mode */
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
     * record to the Videos directory until explicitly stopped. */
    static Options readOptions();

signals:
    /* NOTE: int, not Status. This signal is emitted from a worker thread
     * (droidrec runs off the event loop), so the connection is queued.
     * Qt 5.6 cannot queue unregistered enum types and drops the signal
     * silently; int is a built-in meta type and always works. */
    void statusChanged(int status);
    void recordingFinished(const QString &fileName);

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
    Options m_options;
    QString m_currentPath; /* persistent buffer backing droidrec's out_path */

    friend class DBusAdaptor;
};

#endif /* TAILWIND_RECORDER_H */
