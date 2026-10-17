/*
 * SinkList - PulseAudio sink enumeration for the TailWind GUI.
 *
 * Uses libpulse directly (pa_threaded_mainloop) instead of shelling out
 * to pactl, which is not guaranteed to be installed on the device.
 * Pattern follows sailfish-audio-output (me.henkkalkwater).
 */

#ifndef SINKLIST_H
#define SINKLIST_H

#include <QObject>
#include <QStringList>
#include <QMutex>
#include <QWaitCondition>

struct pa_context;
struct pa_threaded_mainloop;

class SinkList : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool ok READ ok CONSTANT)

public:
    explicit SinkList(QObject *parent = nullptr);
    ~SinkList() override;

    /* true if the connection to the PulseAudio server succeeded */
    bool ok() const { return m_ok; }

    /* sink names in server order (empty list if not connected) */
    Q_INVOKABLE QStringList names();

private:
    static void stateCallback(pa_context *c, void *userdata);
    static void sinkInfoCallback(pa_context *c, const struct pa_sink_info *info,
                                 int eol, void *userdata);

    mutable QMutex m_mutex;
    mutable QWaitCondition m_readyCond;
    mutable QWaitCondition m_doneCond;

    pa_context *m_context = nullptr;
    pa_threaded_mainloop *m_mainloop = nullptr;
    bool m_ok = false;
    bool m_ready = false;
    bool m_done = false;
    QStringList m_names;
};

#endif /* SINKLIST_H */
