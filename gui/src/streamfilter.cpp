#include "streamfilter.h"

#include <pulse/pulseaudio.h>

#include <QDebug>

static const int CONNECT_TIMEOUT_MS = 5000;
static const int QUERY_TIMEOUT_MS = 5000;
static const int POLL_MS = 25;   /* the sidetone stream appears mid-recording */

/* Identity tokens: a native client stream whose app/media/restore
 * identity contains any of these is suspected of carrying microphone
 * audio and gets muted. Ported from Sail-Recorder's MIC_TOKENS. */
static const char *const kMicTokens[] = {
    "microphone", "mic-input", "mic_input", "capture", "recording",
    "recorder", "voice", "voip", "call", "communication", "phone",
    "ofono", "telepathy", "uplink", "hotword", "speech-recognition",
    "camcorder", "loopback", "sidetone",
};

/* A stream is allowed to stay audible when it carries one of these
 * playback labels (media.name, media.role or restore id). Ported from
 * Sail-Recorder's PLAYBACK_TOKENS. */
static const char *const kPlaybackTokens[] = {
    "music", "video", "movie", "game", "event", "notification",
    "alarm", "ringtone", "system", "playback", "media",
};

static bool containsAny(const QString &haystack, const char *const *tokens)
{
    for (const char *const *t = tokens; *t; ++t)
        if (haystack.contains(QLatin1String(*t)))
            return true;
    return false;
}

static QString propValue(const pa_proplist *pl, const char *key)
{
    if (!pl)
        return QString();
    const char *v = pa_proplist_gets(pl, key);
    return v ? QString::fromUtf8(v) : QString();
}

/* Mirrors is_allowed_playback() in Sail-Recorder:
 *  - internal PulseAudio streams are left untouched,
 *  - mic/voice-tainted native streams are blocked,
 *  - labelled media playback is allowed,
 *  - unknown native streams are blocked (on this device the sidetone
 *    stream had no reliable label, while real media had
 *    media.name=music / a known media.role). */
static bool isAllowedPlayback(const StreamFilter::SinkInput &in)
{
    if (!in.isNative())
        return true;

    const QString searchable = (in.applicationName + QLatin1Char(' ')
                                + in.processBinary + QLatin1Char(' ')
                                + in.mediaName + QLatin1Char(' ')
                                + in.mediaRole + QLatin1Char(' ')
                                + in.restoreId).toLower();

    if (containsAny(searchable, kMicTokens))
        return false;

    const QString mediaName = in.mediaName.toLower();
    const QString mediaRole = in.mediaRole.toLower();
    const QString restoreId = in.restoreId.toLower();
    if (containsAny(mediaName, kPlaybackTokens)
            || containsAny(restoreId, kPlaybackTokens)
            || containsAny(mediaRole, kPlaybackTokens))
        return true;

    return false;
}

QString StreamFilter::SinkInput::identity() const
{
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7")
            .arg(driver)
            .arg(quint32(ownerModule))
            .arg(applicationName)
            .arg(processBinary)
            .arg(mediaName)
            .arg(mediaRole)
            .arg(restoreId);
}

static const StreamFilter::SinkInput *findInput(const QList<StreamFilter::SinkInput> &inputs,
                                                quint32 index)
{
    for (const StreamFilter::SinkInput &in : inputs)
        if (in.index == index)
            return &in;
    return nullptr;
}

StreamFilter::StreamFilter(QObject *parent)
    : QObject(parent)
{
    m_poll.setInterval(POLL_MS);
    connect(&m_poll, SIGNAL(timeout()), this, SLOT(poll()));

    m_mainloop = pa_threaded_mainloop_new();
    m_context = pa_context_new(pa_threaded_mainloop_get_api(m_mainloop),
                               "TailWind-StreamFilter");
    if (!m_mainloop || !m_context) {
        qWarning() << "StreamFilter: failed to create pulse context";
        return;
    }

    pa_context_set_state_callback(m_context, stateCallback, this);
    if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
        qWarning() << "StreamFilter: pulse connect refused";
        return;
    }
    pa_threaded_mainloop_start(m_mainloop);

    {
        QMutexLocker locker(&m_mutex);
        if (m_readyCond.wait(&m_mutex, CONNECT_TIMEOUT_MS) && m_ready) {
            m_ok = true;
        } else {
            qWarning() << "StreamFilter: pulse context never became ready";
        }
    }
    if (m_ok)
        qInfo() << "StreamFilter: connected to PulseAudio";
    else
        qWarning() << "StreamFilter: not connected to PulseAudio";
}

StreamFilter::~StreamFilter()
{
    stop();
    if (m_context) {
        pa_context_disconnect(m_context);
        pa_context_unref(m_context);
        m_context = nullptr;
    }
    if (m_mainloop) {
        pa_threaded_mainloop_stop(m_mainloop);
        pa_threaded_mainloop_free(m_mainloop);
        m_mainloop = nullptr;
    }
}

void StreamFilter::stateCallback(pa_context *c, void *userdata)
{
    StreamFilter *self = static_cast<StreamFilter *>(userdata);
    QMutexLocker locker(&self->m_mutex);
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
        self->m_ready = true;
        self->m_readyCond.wakeAll();
        break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        self->m_ready = false;
        self->m_readyCond.wakeAll();
        break;
    default:
        break;
    }
}

void StreamFilter::sinkInputCallback(pa_context *c, const struct pa_sink_input_info *info,
                                     int eol, void *userdata)
{
    Q_UNUSED(c)
    StreamFilter *self = static_cast<StreamFilter *>(userdata);
    QMutexLocker locker(&self->m_mutex);
    if (eol) {
        self->m_done = true;
        self->m_doneCond.wakeAll();
    } else if (info) {
        SinkInput in;
        in.index = info->index;
        in.driver = QString::fromUtf8(info->driver ? info->driver : "");
        in.ownerModule = info->owner_module;
        in.isMuted = info->mute != 0;
        in.applicationName = propValue(info->proplist, "application.name");
        in.processBinary = propValue(info->proplist, "application.process.binary");
        in.mediaName = propValue(info->proplist, "media.name");
        in.mediaRole = propValue(info->proplist, "media.role");
        in.restoreId = propValue(info->proplist, "module-stream-restore.id");
        self->m_inputs.append(in);
    }
}

QList<StreamFilter::SinkInput> StreamFilter::querySinkInputs()
{
    if (!m_context)
        return QList<SinkInput>();

    {
        QMutexLocker locker(&m_mutex);
        m_inputs.clear();
        m_done = false;
    }
    if (!pa_context_get_sink_input_info_list(m_context, sinkInputCallback, this)) {
        qWarning() << "StreamFilter: get_sink_input_info_list failed";
        return QList<SinkInput>();
    }
    {
        QMutexLocker locker(&m_mutex);
        if (!m_doneCond.wait(&m_mutex, QUERY_TIMEOUT_MS))
            qWarning() << "StreamFilter: sink input list timed out";
    }
    QMutexLocker locker(&m_mutex);
    return m_inputs;
}

void StreamFilter::muteDone(pa_context *c, int success, void *userdata)
{
    Q_UNUSED(c)
    if (!success)
        qWarning() << "StreamFilter: set_sink_input_mute failed"
                   << "(stream may have disappeared already)";
    Q_UNUSED(userdata)
}

void StreamFilter::applyFilter()
{
    if (!m_active || !m_context)
        return;

    const QList<SinkInput> inputs = querySinkInputs();
    if (inputs.isEmpty())
        return;

    QList<quint32> toMute;
    QList<quint32> toUnmute;
    bool changed = false;

    {
        QMutexLocker locker(&m_mutex);

        /* drop stale records: stream gone, or the index was reused by a
         * different stream */
        for (auto it = m_muted.begin(); it != m_muted.end(); ) {
            const SinkInput *cur = findInput(inputs, it.key());
            if (!cur || cur->identity() != it.value().identity) {
                it = m_muted.erase(it);
                --m_mutedCount;
                changed = true;
            } else {
                ++it;
            }
        }

        for (const SinkInput &in : inputs) {
            const auto recIt = m_muted.constFind(in.index);
            const MuteRecord *rec = (recIt != m_muted.constEnd()) ? &recIt.value() : nullptr;
            if (isAllowedPlayback(in)) {
                /* was muted by us before its metadata filled in: release */
                if (rec && rec->identity == in.identity() && !rec->wasMuted) {
                    toUnmute.append(in.index);
                    m_muted.remove(in.index);
                    --m_mutedCount;
                    changed = true;
                }
                continue;
            }

            if (rec)
                continue;   /* already muted (or recorded as pre-muted) */

            if (!in.isMuted)
                toMute.append(in.index);
            m_muted.insert(in.index, MuteRecord(in.identity(), in.isMuted));
            ++m_mutedCount;
            changed = true;
        }
    }

    for (const quint32 idx : toMute) {
        qInfo() << "StreamFilter: muting sink input" << idx;
        pa_context_set_sink_input_mute(m_context, idx, 1, muteDone, this);
    }
    for (const quint32 idx : toUnmute) {
        qInfo() << "StreamFilter: unmouting sink input" << idx;
        pa_context_set_sink_input_mute(m_context, idx, 0, muteDone, this);
    }

    if (changed)
        emit mutedChanged();
}

void StreamFilter::poll()
{
    applyFilter();
}

bool StreamFilter::start()
{
    if (m_active)
        return true;
    if (!m_ok) {
        qWarning() << "StreamFilter: not connected; filter disabled";
        return false;
    }
    m_active = true;
    emit activeChanged();
    qInfo() << "StreamFilter: active - muting mic/sidetone playback streams";
    applyFilter();
    m_poll.start();
    return true;
}

void StreamFilter::stop()
{
    if (!m_active)
        return;
    m_active = false;
    m_poll.stop();

    if (m_context) {
        /* restore only streams we actually muted, and only if they are
         * still the same stream */
        const QList<SinkInput> inputs = querySinkInputs();
        QList<quint32> toRestore;
        {
            QMutexLocker locker(&m_mutex);
            for (auto it = m_muted.constBegin(); it != m_muted.constEnd(); ++it) {
                if (it.value().wasMuted)
                    continue;
                const SinkInput *cur = findInput(inputs, it.key());
                if (cur && cur->identity() == it.value().identity)
                    toRestore.append(it.key());
            }
            m_muted.clear();
            m_mutedCount = 0;
        }
        for (const quint32 idx : toRestore) {
            qInfo() << "StreamFilter: restoring sink input" << idx;
            pa_context_set_sink_input_mute(m_context, idx, 0, muteDone, this);
        }
        if (!toRestore.isEmpty())
            qInfo() << "StreamFilter:" << toRestore.size() << "stream(s) restored";
    }

    emit activeChanged();
    emit mutedChanged();
}

bool StreamFilter::ok() const
{
    return m_ok;
}

bool StreamFilter::active() const
{
    return m_active;
}

int StreamFilter::muted() const
{
    return m_mutedCount;
}
