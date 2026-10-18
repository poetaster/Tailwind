#ifndef TAILWIND_DBUSADAPTOR_H
#define TAILWIND_DBUSADAPTOR_H

#include <QDBusAbstractAdaptor>
#include "recorder.h"

class DBusAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "de.poetaster.tailwind")
public:
    explicit DBusAdaptor(QObject *parent = nullptr);
    virtual ~DBusAdaptor();

    bool registerService();
    void unregisterService();

    Q_PROPERTY(int State READ GetState NOTIFY StateChanged)
    Q_PROPERTY(QString Destination READ GetDestination WRITE SetDestination FINAL)
    Q_PROPERTY(int Fps READ GetFps WRITE SetFps FINAL)
    Q_PROPERTY(int Bitrate READ GetBitrate WRITE SetBitrate FINAL)
    Q_PROPERTY(int Seconds READ GetSeconds WRITE SetSeconds FINAL)

public slots:
    Q_NOREPLY void Quit();
    void Start();
    Q_NOREPLY void Stop();

    /* Audio: the GUI captures microphone/media PCM (S16LE/stereo/48 kHz)
     * and appends it to a shared file in the app's cache dir, which it
     * prepares BEFORE calling StartAudio; this slot resolves the same
     * path from the dconf "tempdir" key and hands it to
     * droidrec_audio_begin(), which tails the file into the AAC encoder.
     * Stays argumentless: Nemo.DBus on device rejects string-argument
     * calls. */
    int StartAudio();
    void StopAudio();
    int GetAudioRate() const;
    int GetAudioChannels() const;

    int GetState() const;
    QString GetLastError() const;

    QString GetDestination() const;
    void SetDestination(const QString &destination);

    int GetFps() const;
    void SetFps(int fps);

    int GetBitrate() const;
    void SetBitrate(int bitrate);

    int GetSeconds() const;
    void SetSeconds(int seconds);

signals:
    void StateChanged(int state);
    void RecordingFinished(const QString &fileName);
};

#endif /* TAILWIND_DBUSADAPTOR_H */
