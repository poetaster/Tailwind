#include "audiofeeder.h"

#include <pulse/pulseaudio.h>
#include <pulse/thread-mainloop.h>

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDebug>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

/* The shared PCM file lives in the scratch dir the QML passes to
 * prepare() (the app's cache dir, published as the dconf "tempdir" */
static const char kPcmFileName[] = "tailwind_audio.pcm";

static const int kConnectTimeoutMs = 5000;
static const int kQueryTimeoutMs = 3000;
static const int kWoundDownTimeoutMs = 3000;
static const qlonglong kEmitStepBytes = 16 * 1024;  /* bytesChanged() throttle */

AudioFeeder::AudioFeeder(QObject *parent)
    : QObject(parent)
{
}

AudioFeeder::~AudioFeeder()
{
    if (m_running || m_fd >= 0)
        stop();
    windDown();   /* defensive: release any pulse objects we failed to clean */
}

QString AudioFeeder::prepare(const QString &dir)
{
    if (m_fd >= 0)
        close(m_fd);

    /* dir comes from the QML (conf.tempdir, i.e. the app's cache dir);
     * fall back to our own cache location if none is given. */
    QString base = dir.trimmed();
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString path = base + QLatin1String("/") + QLatin1String(kPcmFileName);
    if (!QDir().mkpath(QFileInfo(path).path())) {
        m_error = QStringLiteral("cannot create directory for %1").arg(path);
        emit errorChanged();
        return QString();
    }
    const QByteArray pathUtf8 = path.toUtf8();
    m_fd = open(pathUtf8.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (m_fd < 0) {
        m_error = QString("open(%1) failed: %2").arg(path, strerror(errno));
        emit errorChanged();
        return QString();
    }

    m_outputPath = path;
    m_error.clear();
    m_bytes = 0;
    m_emitFloor = 0;
    m_writeErrorLogged = false;
    m_stoppedEmitted = false;
    emit errorChanged();
    emit bytesChanged();
    emit outputChanged();
    return m_outputPath;
}

bool AudioFeeder::start(const QString &sinkName)
{
    if (m_running) {
        qWarning() << "AudioFeeder: start() while already running";
        return false;
    }
    if (m_fd < 0) {
        m_error = QStringLiteral("PCM file not prepared - call prepare() first");
        emit errorChanged();
        return false;
    }
    if (sinkName.trimmed().isEmpty()) {
        m_error = QStringLiteral("no audio source selected");
        emit errorChanged();
        return false;
    }

    m_sinkName = sinkName;
    m_error.clear();
    emit errorChanged();

    /* --- connect the pulse context (the StreamFilter idiom) --- */
    m_mainloop = pa_threaded_mainloop_new();
    m_context = m_mainloop
                    ? pa_context_new(pa_threaded_mainloop_get_api(m_mainloop),
                                     "TailWind-AudioFeeder")
                    : nullptr;
    if (!m_mainloop || !m_context) {
        failStart(QStringLiteral("libpulse init failed"));
        return false;
    }

    pa_context_set_state_callback(m_context, contextStateCallback, this);
    {
        QMutexLocker locker(&m_lock);
        m_ctxDecided = false;
        m_ctxOk = false;
    }
    if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
        failStart(QStringLiteral("PulseAudio connect refused"));
        return false;
    }
    pa_threaded_mainloop_start(m_mainloop);
    m_mainloopStarted = true;

    {
        QMutexLocker locker(&m_lock);
        if (!m_ctxDecided)
            m_ctxReady.wait(&m_lock, kConnectTimeoutMs);
        if (!m_ctxOk) {
            failStart(QStringLiteral("PulseAudio context never became ready"));
            return false;
        }
    }
    qInfo() << "AudioFeeder: connected to PulseAudio";

    /* --- resolve the capture source --- */
    /* "mic" is the direct microphone (the only path offered in the QML);
     * anything else is treated as a PulseAudio sink to monitor. */
    QStringList candidates;
    if (sinkName == QLatin1String("mic")) {
        //candidates << QStringLiteral("source.primary_input")
        candidates << QStringLiteral("sink.deep_buffer.monitor")
                   << QStringLiteral("default");
    } else {
        candidates << (sinkName + QStringLiteral(".monitor")) << sinkName;
    }

    QString chosen;
    for (const QString &name : candidates) {
        if (sourceExists(name)) {
            chosen = name;
            qInfo() << "AudioFeeder: capturing from source" << name;
            break;
        }
    }
    if (chosen.isEmpty()) {
        failStart(QStringLiteral("no such PulseAudio source: ") + sinkName);
        return false;
    }

    /* open the capture stream --- */
    const pa_sample_spec spec = { PA_SAMPLE_S16LE, 48000, 2 };
    m_stream = pa_stream_new(m_context, "tailwind-capture", &spec, nullptr);
    if (!m_stream) {
        failStart(QStringLiteral("pa_stream_new failed"));
        return false;
    }
    pa_stream_set_state_callback(m_stream, streamStateCallback, this);
    pa_stream_set_read_callback(m_stream, readCallback, this);

    {
        QMutexLocker locker(&m_lock);
        m_streamDecided = false;
        m_streamOk = false;
    }
    /* connect_record takes the source NAME in this libpulse API;
     * NULL buffer attrs and flags mean "server defaults". The cast
     * is required: this libpulse's pa_stream_flags_t is an enum and
     * C++ won't take a bare 0. */
    if (pa_stream_connect_record(m_stream, chosen.toUtf8().constData(),
                                 nullptr, (pa_stream_flags_t)0) < 0) {
        failStart(QStringLiteral("failed to connect the capture stream"));
        return false;
    }

    {
        QMutexLocker locker(&m_lock);
        if (!m_streamDecided)
            m_streamUp.wait(&m_lock, kConnectTimeoutMs);
        if (!m_streamOk) {
            failStart(QStringLiteral("capture stream never went live"));
            return false;
        }
    }

    m_statsClock.restart();
    m_statsBytes = 0;
    m_running = true;
    emit runningChanged();
    qInfo() << "AudioFeeder: capturing to" << m_outputPath
            << "(S16LE 48 kHz stereo, fd" << m_fd << ")";
    return true;
}

void AudioFeeder::stop()
{
    if (!m_running && m_fd < 0)
        return;   /* nothing to do */

    windDown();

    m_running = false;
    emit runningChanged();
    if (!m_stoppedEmitted) {
        m_stoppedEmitted = true;
        emit stopped(m_error);
    }
}

bool AudioFeeder::running() const
{
    return m_running;
}

QString AudioFeeder::error() const
{
    return m_error;
}

qlonglong AudioFeeder::bytes() const
{
    return m_bytes;
}

QString AudioFeeder::output() const
{
    return m_outputPath;
}

/*  GUI-thread helpers */

bool AudioFeeder::sourceExists(const QString &name)
{
    m_queryUtf8 = name.toUtf8();   /* must outlive the async callback */
    {
        QMutexLocker locker(&m_lock);
        m_sourceDecided = false;
        m_sourceFound = false;
    }
    /* returns a pa_operation* - NULL means the query was refused */
    if (pa_context_get_source_info_by_name(m_context, m_queryUtf8.constData(),
                                           sourceInfoCallback, this) == nullptr) {
        qWarning() << "AudioFeeder: source query for" << name << "was refused";
        return false;
    }
    {
        QMutexLocker locker(&m_lock);
        if (!m_sourceDecided)
            m_sourceQuery.wait(&m_lock, kQueryTimeoutMs);
        qWarning() << "AudioFeeder: source found" << name << "m_sourceFound";
        return m_sourceFound;
    }
}

void AudioFeeder::windDown()
{
    /* Disconnect the capture, then wait until the mainloop thread has
     * delivered the final FAILED/TERMINATED callback - from that moment
     * no callback can be in flight, so the file fd is safe to close and
     * the pulse objects safe to release. (StreamFilter's idiom.) */
    if (m_mainloopStarted && m_context) {
        if (m_stream)
            pa_stream_disconnect(m_stream);

        {
            QMutexLocker locker(&m_lock);
            m_woundDownFlag = false;
        }
        pa_context_disconnect(m_context);
        {
            QMutexLocker locker(&m_lock);
            if (!m_woundDownFlag)
                m_woundDown.wait(&m_lock, kWoundDownTimeoutMs);
        }
    }

    closeFd();

    if (m_stream) {
        pa_stream_unref(m_stream);
        m_stream = nullptr;
    }
    if (m_context) {
        pa_context_unref(m_context);
        m_context = nullptr;
    }
    if (m_mainloop) {
        if (m_mainloopStarted)
            pa_threaded_mainloop_stop(m_mainloop);
        pa_threaded_mainloop_free(m_mainloop);
        m_mainloop = nullptr;
        m_mainloopStarted = false;
    }
}

void AudioFeeder::closeFd()
{
    if (m_fd < 0)
        return;
    const off_t size = lseek(m_fd, 0, SEEK_CUR);
    qDebug() << "AudioFeeder: PCM file" << m_outputPath
             << "closed:" << size << "bytes on disk (" << m_bytes
             << "delivered from capture)";
    close(m_fd);
    m_fd = -1;
}

void AudioFeeder::failStart(const QString &err)
{
    qWarning() << "AudioFeeder:" << err;
    windDown();
    m_running = false;
    m_error = err;
    emit errorChanged();
    emit runningChanged();
    if (!m_stoppedEmitted) {
        m_stoppedEmitted = true;
        emit stopped(err);
    }
}

/* Pulse callbacks (pa_threaded_mainloop thread) */

void AudioFeeder::contextStateCallback(pa_context *c, void *userdata)
{
    AudioFeeder *self = static_cast<AudioFeeder *>(userdata);
    QMutexLocker locker(&self->m_lock);
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
        self->m_ctxOk = true;
        self->m_ctxDecided = true;
        self->m_ctxReady.wakeAll();
        break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        self->m_ctxOk = false;
        self->m_ctxDecided = true;
        self->m_streamDecided = true;   /* release a blocked start() wait */
        self->m_woundDownFlag = true;   /* context is gone; safe to unref */
        self->m_ctxReady.wakeAll();
        self->m_streamUp.wakeAll();
        self->m_woundDown.wakeAll();
        break;
    default:
        break;
    }
}

void AudioFeeder::streamStateCallback(pa_stream *s, void *userdata)
{
    AudioFeeder *self = static_cast<AudioFeeder *>(userdata);
    QMutexLocker locker(&self->m_lock);
    switch (pa_stream_get_state(s)) {
    case PA_STREAM_READY:
        self->m_streamOk = true;
        self->m_streamDecided = true;
        self->m_streamUp.wakeAll();
        break;
    case PA_STREAM_FAILED:
    case PA_STREAM_TERMINATED:
        if (!self->m_streamOk)
            qWarning() << "AudioFeeder: capture stream failed to establish"
                       << "(capture will deliver no data)";
        self->m_streamDecided = true;
        self->m_streamUp.wakeAll();
        break;
    default:
        break;
    }
}

void AudioFeeder::sourceInfoCallback(pa_context *c, const struct pa_source_info *info,
                                     int eol, void *userdata)
{
    Q_UNUSED(c)
    AudioFeeder *self = static_cast<AudioFeeder *>(userdata);
    QMutexLocker locker(&self->m_lock);
    if (info)
        self->m_sourceFound = true;
    if (eol || info) {
        self->m_sourceDecided = true;
        self->m_sourceQuery.wakeAll();
    }
}

void AudioFeeder::readCallback(pa_stream *s, size_t nbytes, void *userdata)
{
    Q_UNUSED(nbytes)
    AudioFeeder *self = static_cast<AudioFeeder *>(userdata);

    /* parecord's read path: peek at the current fragment, consume it,
     * drop it. data==NULL with nbytes>0 is a hole in the stream (e.g.
     * an underrun we caused) - drop it without writing. */
    const void *data = nullptr;
    size_t length = 0;
    if (pa_stream_peek(s, &data, &length) < 0)
        return;
    if (data && length > 0) {
        /* The whole point of this class: the bytes go straight to the
         * file the daemon tails - sub-millisecond drain, no Qt queue, no
         * intermediate buffer to underflow or corrupt. */
        const ssize_t written = write(self->m_fd, data, length);
        if (written < 0 || static_cast<size_t>(written) != length) {
            if (!self->m_writeErrorLogged) {
                self->m_writeErrorLogged = true;
                qWarning() << "AudioFeeder: write(" << self->m_outputPath
                           << ") failed:" << written << "of" << length
                           << strerror(errno);
            }
        } else {
            self->m_bytes += length;
            if (self->m_bytes - self->m_emitFloor >= kEmitStepBytes) {
                self->m_emitFloor = self->m_bytes;
                emit self->bytesChanged();
            }

            /* once-per-second delivery-rate log, wall clock
             * (capture-thread only) */
            if (self->m_statsClock.elapsed() >= 1000) {
                const qlonglong dt = self->m_statsClock.elapsed();
                const qlonglong db = self->m_bytes - self->m_statsBytes;
                qDebug() << "AudioFeeder: stats rate" << (1000.0 * db / dt)
                         << "B/s, delivered" << self->m_bytes;
                self->m_statsClock.restart();
                self->m_statsBytes = self->m_bytes;
            }
        }
    }
    pa_stream_drop(s);
}
