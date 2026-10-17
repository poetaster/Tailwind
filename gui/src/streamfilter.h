/*
 * StreamFilter - keeps microphone audio out of the sink-monitor capture.
 *
 * On this droid-HAL device, microphone content reaches a sink's monitor
 * through *playback* streams (the "mic sidetone" stream, voip uplinks,
 * ...). Sail-Recorder proves the fix on this exact device: while a screen
 * recording is running, mute the suspect playback streams and restore
 * their mute state when the recording ends. Only labelled media playback
 * (music, video, UI sounds) is left audible.
 *
 * Ported from Sail-Recorder's sail-recorder-daemon.py (apply_stream_filter
 * + is_allowed_playback), using libpulse directly instead of shelling out
 * to pactl. Everything is polled on a timer because the sidetone stream
 * only appears once microphone processing starts.
 */
#ifndef STREAMFILTER_H
#define STREAMFILTER_H

#include <QObject>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QWaitCondition>
#include <QTimer>

struct pa_context;
struct pa_threaded_mainloop;
struct pa_sink_input_info;

class StreamFilter : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool ok READ ok NOTIFY okChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(int muted READ muted NOTIFY mutedChanged)

public:
    explicit StreamFilter(QObject *parent = nullptr);
    ~StreamFilter() override;

    /* begin the protection: mute mic/sidetone playback streams and start
     * polling for new ones. Safe to call again while active.
     * Q_INVOKABLE: called from QML (FirstPage.qml). */
    Q_INVOKABLE bool start();

    /* restore the mute state of every stream we muted, stop the poll */
    Q_INVOKABLE void stop();

    bool ok() const;
    bool active() const;
    int muted() const;

signals:
    void okChanged();
    void activeChanged();
    void mutedChanged();

private slots:
    void poll();

public:
    struct SinkInput {
        quint32 index = 0;
        QString driver;
        quint32 ownerModule = 0;
        bool isMuted = false;
        QString applicationName;
        QString processBinary;
        QString mediaName;
        QString mediaRole;
        QString restoreId;

        bool isNative() const { return driver == QStringLiteral("protocol-native.c"); }
        QString identity() const;
    };

private:
    struct MuteRecord {
        QString identity;
        bool wasMuted;   /* mute state before we touched it */
        MuteRecord() = default;
        MuteRecord(const QString &id, bool m) : identity(id), wasMuted(m) {}
    };

    void applyFilter();
    QList<SinkInput> querySinkInputs();

    static void stateCallback(pa_context *c, void *userdata);
    static void sinkInputCallback(pa_context *c, const struct pa_sink_input_info *info,
                                  int eol, void *userdata);
    static void muteDone(pa_context *c, int success, void *userdata);

    /* pulse (its own threaded mainloop, like SinkList) */
    bool m_ok = false;
    bool m_active = false;
    pa_threaded_mainloop *m_mainloop = nullptr;
    pa_context *m_context = nullptr;

    /* query state (touched from the mainloop thread) */
    QMutex m_mutex;
    QWaitCondition m_readyCond;
    QWaitCondition m_doneCond;
    bool m_ready = false;
    bool m_done = false;
    QList<SinkInput> m_inputs;

    /* what we muted (GUI thread); index -> record */
    QHash<quint32, MuteRecord> m_muted;
    int m_mutedCount = 0;

    QTimer m_poll;
};

#endif /* STREAMFILTER_H */
