/*
 * TailWind - Qt wrapper around the droidrec backend (see recorder.h).
 */

#include "recorder.h"
#include "droidrec.h"

#include <QDateTime>
#include <QDebug>
#include <QLoggingCategory>
#include <QMutexLocker>

#include <MDConfGroup>
#include <thread>


Q_LOGGING_CATEGORY(logrecorder, "tailwind.recorder", QtDebugMsg)

namespace {
Recorder *s_instance = nullptr;

/* Default output directory. On Sailfish the recording daemon runs with access
 * to the primary user's home; the original droidrec used this path too. */
const char *kDefaultVideosDir = "/home/defaultuser/Videos";
}

Recorder::Recorder(QObject *parent)
    : QObject(parent)
{
    m_options = readOptions();
    s_instance = this;
    qCDebug(logrecorder) << "Recorder created, destination dir:" << m_options.destination;
}

Recorder::~Recorder()
{
    if (s_instance == this)
        s_instance = nullptr;
}

Recorder *Recorder::instance()
{
    return s_instance;
}

Recorder::Options Recorder::readOptions()
{
    /* Settings are stored by the GUI via Nemo.Configuration (dconf) at
     * /de/poetaster/tailwind and read here, as in
     * screenrecorder /org/coderus/screenrecorder. */
    MDConfGroup dconf(QStringLiteral("/de/poetaster/tailwind"));
    Options o;
    o.destination = dconf.value(QStringLiteral("destination"), QString::fromUtf8(kDefaultVideosDir)).toString();
    o.fps     = dconf.value(QStringLiteral("fps"), 24).toInt();
    o.bitrate = dconf.value(QStringLiteral("bitrate"), 24).toInt() * 1000000; /* Mbit/s -> bits/sec */
    o.seconds = dconf.value(QStringLiteral("seconds"), 0).toInt();            /* 0 = record until stop() */
    o.width   = 0;   /* native */
    o.height  = 0;   /* native */
    o.audioSink = dconf.value(QStringLiteral("audiosink"), QString()).toString(); /* empty = video only */
    return o;
}

Recorder::Status Recorder::status() const
{
    QMutexLocker locker(&m_mutex);
    return m_status;
}

void Recorder::setStatus(Status status)
{
    {
        QMutexLocker locker(&m_mutex);
        if (m_status == status)
            return;
        m_status = status;
    }
    qCDebug(logrecorder) << Q_FUNC_INFO << status;
    emit statusChanged((int)status);
}

Recorder::Options Recorder::options() const
{
    QMutexLocker locker(&m_mutex);
    return m_options;
}

void Recorder::setOptions(const Options &options)
{
    QMutexLocker locker(&m_mutex);
    m_options = options;
}

void Recorder::init()
{
    if (droidrec_prepare() == 0) {
        if (status() == StatusIdle)
            setStatus(StatusReady);
    } else {
        qWarning(logrecorder) << "droidrec_prepare failed:" << droidrec_last_error();
        /* Stay Idle: the GUI keeps polling GetState() until this succeeds. */
    }
}

bool Recorder::start()
{
    {
        QMutexLocker locker(&m_mutex);
        if (m_status == StatusRecording || m_status == StatusSaving) {
            qWarning(logrecorder) << Q_FUNC_INFO << "already" << m_status;
            return false;
        }
        if (m_startInProgress) {
            qWarning(logrecorder) << Q_FUNC_INFO << "start already in progress";
            return false;
        }
        /* Re-read current settings from dconf so the GUI's latest values are
         * authoritative at record time. The GUI (Nemo.Configuration) writes the
         * selected audio sink to dconf, possibly *after* this lazily-activated
         * daemon read it once at startup, and there is no SetAudioSink D-Bus call
         * to push it -- so a stale/empty cached value would wrongly disable audio.
         * The GUI is the settings source of truth and does not use the Set* methods. */
        m_options = readOptions();
        m_startInProgress = true;
    }
    /* same naming convention as in screenrecorder
    * and the same date format as jolla gallery
    */
    const QString dateString = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    QString path;
    int fps, bitrate, seconds, width, height;
    QString audioSink;
    {
        QMutexLocker locker(&m_mutex);
        path = m_options.destination + QStringLiteral("/tailwind-%1.ts").arg(dateString);
        m_currentPath = path;
        fps     = m_options.fps;
        bitrate = m_options.bitrate;
        seconds = m_options.seconds;
        width   = m_options.width;
        height  = m_options.height;
        audioSink = m_options.audioSink;
    }

    const QByteArray outPath = path.toUtf8();
    const QByteArray sinkName = audioSink.toUtf8();
    qCDebug(logrecorder) << "Starting (async):" << path
                         << "fps=" << fps
                         << "bitrate=" << bitrate
                         << "seconds=" << seconds
                         << "audiosink=" << (sinkName.isEmpty() ? QStringLiteral("(none)") : QString::fromUtf8(sinkName));

    /* droidrec_start() can block, maybe, so don't run on the Qt
     * event-loop thread:  * Run it on a detached worker;
     * the status update + signals are emitted
     * from that worker, Qt auto-queues them back to the main thread. */
    std::thread([this, outPath, sinkName, fps, bitrate, seconds, width, height]() {
        DroidRecOptions opts;
        opts.out_path   = outPath.constData();
        opts.target_fps = fps;
        opts.bitrate    = bitrate;
        opts.seconds    = seconds;
        opts.width      = width;
        opts.height     = height;
        opts.audio_sink = sinkName.isEmpty() ? nullptr : sinkName.constData();

        const int rc = droidrec_start(&opts);
        {
            QMutexLocker locker(&m_mutex);
            m_startInProgress = false;
        }
        m_opCond.wakeAll();

        if (rc == 0) {
            setStatus(StatusRecording);
        } else {
            qWarning(logrecorder) << "droidrec_start failed:" << droidrec_last_error();
            setStatus(StatusReady);
        }
    }).detach();

    return true;
}

QString Recorder::stop()
{
    QString path;
    {
        QMutexLocker locker(&m_mutex);
        if (m_status != StatusRecording) {
            qWarning(logrecorder) << Q_FUNC_INFO << "not recording (state" << m_status << ")";
            return QString();
        }
        if (m_stopInProgress) {
            qWarning(logrecorder) << Q_FUNC_INFO << "stop already in progress";
            return m_currentPath;
        }
        m_stopInProgress = true;
        path = m_currentPath;
    }

    qCDebug(logrecorder) << "Stopping (async):" << path;
    setStatus(StatusSaving);

    /* droidrec_stop() drains the encoder (up to ~2s) and pthread_joins the
     * codec/worker threads.
     * Run it on a detached worker and report the
     * finished file from that worker once cleanup is done. */

    std::thread([this, path]() {
        const int rc = droidrec_stop();
        bool requestExit;
        {
            QMutexLocker locker(&m_mutex);
            m_stopInProgress = false;
            /* Only a user Stop should trigger an exit. If a SIGTERM/Quit
             * already drove us into handleShutDown(), that path owns the
             * teardown (it calls handleShutDown() + qApp->quit() itself), so
             * don't emit the shutdown alert again. */
            requestExit = !m_shutdown;
        }
        m_opCond.wakeAll();

        if (rc != 0) {
            qWarning(logrecorder) << "droidrec_stop reported:" << droidrec_last_error();
        }
        setStatus(StatusReady);
        qCDebug(logrecorder) << "Recording saved to" << path;
        emit recordingFinished(path);
        if (requestExit) {
            qCDebug(logrecorder) << "stop() complete; requesting shutdown";
            emit shutdownRequested();
        }
    }).detach();

    return path;
}

void Recorder::handleShutDown()
{
    bool needStop = false;
    {
        QMutexLocker locker(&m_mutex);
        if (m_shutdown)
            return;
        m_shutdown = true;
        needStop = (m_status == StatusRecording);
    }

    qCDebug(logrecorder) << Q_FUNC_INFO;
    if (needStop)
        stop();

    /* Shut down is a hard stop: wait for any in-flight start/stop worker to
     * finish so droidrec_shutdown() does not race the codec finalisation. */
    {
        QMutexLocker locker(&m_mutex);
        while (m_startInProgress || m_stopInProgress)
            m_opCond.wait(&m_mutex);
    }

    droidrec_shutdown();
    /* Deliberately do NOT announce StatusIdle here: every caller of
     * handleShutDown() exits the process immediately after, so "alive but
     * idle" is never a real end state. The last meaningful broadcast already
     * sent is StatusReady (from the stop worker). Leaving the GUI's cached
     * state at Ready keeps its Record button armed, so the next Start call
     * re-activates the daemon via D-Bus/systemd instead of the button being
     * disabled at state 0. */
}
