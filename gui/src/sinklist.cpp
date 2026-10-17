#include "sinklist.h"

#include <pulse/pulseaudio.h>

#include <QDebug>

static const int CONNECT_TIMEOUT_MS = 5000;
static const int QUERY_TIMEOUT_MS = 5000;

SinkList::SinkList(QObject *parent)
    : QObject(parent)
{
    m_mainloop = pa_threaded_mainloop_new();
    m_context = pa_context_new(pa_threaded_mainloop_get_api(m_mainloop), "TailWind");
    if (!m_mainloop || !m_context) {
        qWarning() << "SinkList: failed to create pulse context";
        m_ok = false;
        return;
    }

    pa_context_set_state_callback(m_context, SinkList::stateCallback, this);

    int rc = pa_context_connect(m_context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr);
    if (rc < 0) {
        qWarning() << "SinkList: pulse connect refused:" << rc;
        m_ok = false;
        return;
    }

    pa_threaded_mainloop_start(m_mainloop);

    {
        QMutexLocker locker(&m_mutex);
        bool timed_out = !m_readyCond.wait(&m_mutex, CONNECT_TIMEOUT_MS);
        if (timed_out)
            qWarning() << "SinkList: pulse context never became ready";
        m_ok = m_ready;
    }

    if (!m_ok)
        qWarning() << "SinkList: not connected to PulseAudio";
    else
        qInfo() << "SinkList: connected to PulseAudio";
}

SinkList::~SinkList()
{
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

void SinkList::stateCallback(pa_context *c, void *userdata)
{
    SinkList *self = static_cast<SinkList *>(userdata);
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

void SinkList::sinkInfoCallback(pa_context *c, const struct pa_sink_info *info,
                                int eol, void *userdata)
{
    Q_UNUSED(c)
    SinkList *self = static_cast<SinkList *>(userdata);
    QMutexLocker locker(&self->m_mutex);
    if (eol) {
        self->m_done = true;
        self->m_doneCond.wakeAll();
    } else if (info && info->name) {
        self->m_names << QString::fromUtf8(info->name);
    }
}

QStringList SinkList::names()
{
    if (!m_ok || !m_context)
        return QStringList();

    QMutexLocker locker(&m_mutex);
    m_names.clear();
    m_done = false;

    pa_operation *op = pa_context_get_sink_info_list(m_context, SinkList::sinkInfoCallback, this);
    if (!op) {
        qWarning() << "SinkList::names: get_sink_info_list failed";
        return QStringList();
    }

    m_doneCond.wait(&m_mutex, QUERY_TIMEOUT_MS);
    if (!m_done)
        qWarning() << "SinkList::names: timed out waiting for sink list";

    return m_names;
}
