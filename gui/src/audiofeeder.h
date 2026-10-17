/*
 * AudioFeeder - captures microphone audio with QAudioInput inside the
 * TailWind GUI app and appends the raw PCM to a shared file that the
 * daemon tails into the AAC encoder.
 *
 * Transport: a file, not D-Bus. A synchronous D-Bus round-trip per 8 KB
 * chunk stalls the capture pump and the recording crackles; appending to
 * a local file is just a write(2) and keeps up at 200 KB/s with headroom.
 *
 * Contract with the daemon:
 *  - prepare() creates/truncates the PCM file and returns its path;
 *  - the QML calls the daemon's BeginAudio() (no arguments - the path
 *    is a fixed convention both sides agree on, nothing crosses D-Bus)
 *    and then calls start();
 *  - on stop, finish() closes the file (flushing) BEFORE the QML sends
 *    EndAudio, so the daemon's tailer sees a clean end of file.
 *
 * Format: S16LE, 48 kHz, stereo, interleaved - what the daemon's AAC
 * encoder expects.  PulseAudio converts if the source uses a different
 * format.
 *
 * Uses QAudioInput (Qt Multimedia), the same capture pattern as
 * FiatVox's pitchdetector.cpp, which is proven to work on the device:
 * a QTimer pumps the stream because readyRead is not reliable there.
 */

#ifndef AUDIOFEEDER_H
#define AUDIOFEEDER_H

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QAudio>
#include <QString>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <sys/stat.h>

class QAudioInput;
class QIODevice;

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
     * path, or an empty string on failure (see error()). Call before
     * start(); the daemon's BeginAudio() (no args) opens the same
     * fixed path by convention. */
    Q_INVOKABLE QString prepare();
    Q_INVOKABLE bool start(const QString &sinkName);
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

private slots:
    void handleStateChange(QAudio::State state);
    void pumpAudio();

private:
    void finish(const QString &errorText);

    QString m_sinkName;
    QString m_outputPath;
    QString m_error;
    qlonglong m_bytes = 0;
    bool m_running = false;
    bool m_wasActive = false;   /* the stream has actually been live */

    QAudioInput *m_audio = nullptr;
    QPointer<QIODevice> m_source;   /* m_audio->start(), owned by m_audio */
    QTimer m_pump;                  /* polls the device; readyRead is not enough */
    int m_fd = -1;                  /* the PCM file the daemon tails (raw fd) */
    QByteArray m_scratch;
    bool m_writeErrorLogged = false;
};

#endif /* AUDIOFEEDER_H */
