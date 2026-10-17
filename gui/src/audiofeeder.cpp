/*
 * AudioFeeder implementation.
 *
 * QAudioInput over the microphone source (PulseAudio backend), appending
 * the raw PCM to a shared file the daemon tails. Follows FiatVox's
 * pitchdetector.cpp, which is proven to work on the device with this
 * SDK's Qt:
 *  - a QTimer pumps the stream (readyRead is not enough),
 *  - never call bytesAvailable() before reading - checking first is
 *    what made FiatVox read nothing at all on device,
 *  - read in a loop until a short read,
 *  - state changes (Stopped + error) end the capture.
 *
 * The chunk callback (D-Bus FeedAudio) is gone: a synchronous D-Bus
 * round-trip per chunk stalled the pump and the recording crackled.
 * Now each chunk is a raw write(2) to the PCM file - unbuffered, so
 * the daemon's tailer sees it as it lands. (QFile was tried first:
 * it reported write failures as "Unknown error" with errno 0, which
 * gave no way to tell what failed - raw POSIX I/O at least gives a
 * real errno string.)
 *
 * Everything runs on the GUI thread, so no mutexes are needed.
 */
#include "audiofeeder.h"

#include <QAudioDeviceInfo>
#include <QAudioFormat>
#include <QAudioInput>
#include <QDebug>

/* bytes per read attempt; 48 kHz stereo s16le is ~192 kB/s, so a
 * 20 ms pump tick has about 4 kB of audio in it */
static const int kChunkBytes = 8192;
static const int kPumpMs = 20;

/* Shared PCM file, one per recording, next to the video output - the
 * one place both the app and the daemon can write for sure (/tmp
 * writes failed silently on device). MUST match
 * TAILWIND_AUDIO_PCM_PATH in recorder/src/droidrec.h; the daemon opens
 * it by convention, so the path is never passed over D-Bus. The daemon
 * removes it once the recording is done. */
static const char kPcmPath[] = "/home/defaultuser/Videos/tailwind_audio.pcm";

AudioFeeder::AudioFeeder(QObject *parent)
    : QObject(parent)
{
    m_pump.setInterval(kPumpMs);
    connect(&m_pump, SIGNAL(timeout()), this, SLOT(pumpAudio()));
}

AudioFeeder::~AudioFeeder()
{
    stop();
}

QString AudioFeeder::prepare()
{
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }

    const QString path = QString::fromUtf8(kPcmPath);
    m_fd = open(kPcmPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (m_fd < 0) {
        m_error = QStringLiteral("cannot open %1: %2")
                .arg(path, QString::fromUtf8(strerror(errno)));
        emit errorChanged();
        return QString();
    }

    m_outputPath = path;
    emit outputChanged();
    m_bytes = 0;
    emit bytesChanged();
    m_error.clear();
    emit errorChanged();
    m_writeErrorLogged = false;
    m_wasActive = false;

    qDebug() << "AudioFeeder: PCM file ready at" << path;
    return path;
}

bool AudioFeeder::start(const QString &sinkName)
{
    if (m_running) {
        qWarning() << "AudioFeeder: already running";
        return false;
    }
    if (m_fd < 0) {
        m_error = QStringLiteral("prepare() was not called (no PCM file)");
        emit errorChanged();
        return false;
    }
    if (sinkName.isEmpty()) {
        m_error = QStringLiteral("no audio sink selected");
        emit errorChanged();
        return false;
    }

    m_sinkName = sinkName;
    m_bytes = 0;
    emit bytesChanged();
    m_error.clear();
    emit errorChanged();

    /* "mic" = direct microphone capture (the droid-HAL sink monitors
     * are voice-processed and sound distorted on the device).
     * Prefer source.primary_input, verified clean with arecord; fall
     * back to any non-null input device. Other names use the legacy
     * "<sink>.monitor" loopback lookup. */
    QAudioDeviceInfo device;
    const QList<QAudioDeviceInfo> inputs
            = QAudioDeviceInfo::availableDevices(QAudio::AudioInput);
    if (sinkName == QStringLiteral("mic")) {
        for (int i = 0; i < inputs.size() && device.isNull(); ++i) {
            if (inputs.at(i).deviceName()
                    == QStringLiteral("source.primary_input"))
                device = inputs.at(i);
        }
        for (int i = 0; i < inputs.size() && device.isNull(); ++i) {
            if (!inputs.at(i).deviceName().contains(QStringLiteral("null")))
                device = inputs.at(i);
        }
    } else {
        for (int i = 0; i < inputs.size(); ++i) {
            const QAudioDeviceInfo &d = inputs.at(i);
            if (d.deviceName() == sinkName + QStringLiteral(".monitor")
                    || d.deviceName() == sinkName) {
                device = d;
                break;
            }
        }
    }
    if (device.isNull()) {
        device = QAudioDeviceInfo::defaultInputDevice();
        if (device.isNull()) {
            qWarning() << "AudioFeeder: no input devices at all for sink"
                       << sinkName;
            m_error = QStringLiteral("no audio input devices available");
            emit errorChanged();
            return false;
        }
        qWarning() << "AudioFeeder: monitor source for" << sinkName
                   << "not found; FALLING BACK to default input (microphone):"
                   << device.deviceName();
    }

    /* What the daemon's AAC encoder expects; PulseAudio resamples the
     * source if it runs at a different rate. */
    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(2);
    format.setSampleSize(16);
    format.setCodec(QStringLiteral("audio/pcm"));
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);

    if (!device.isFormatSupported(format))
        format = device.nearestFormat(format);

    /* The daemon's AAC encoder requires exactly 48 kHz stereo s16le.
     * Anything else and we would be interpreting the bytes wrong,
     * which is worse than saying so. */
    if (format.sampleRate() != 48000
            || format.channelCount() != 2
            || format.sampleSize() != 16
            || format.sampleType() != QAudioFormat::SignedInt) {
        m_error = QStringLiteral("%1 · %2 Hz %3ch %4-bit")
                .arg(device.deviceName())
                .arg(format.sampleRate())
                .arg(format.channelCount())
                .arg(format.sampleSize());
        emit errorChanged();
        return false;
    }

    m_audio = new QAudioInput(device, format, this);
    connect(m_audio, SIGNAL(stateChanged(QAudio::State)),
            this, SLOT(handleStateChange(QAudio::State)));

    QIODevice *source = m_audio->start();
    if (!source) {
        m_error = QStringLiteral("failed to start capture on %1")
                .arg(device.deviceName());
        m_audio->deleteLater();
        m_audio = nullptr;
        if (m_fd >= 0) {
            close(m_fd);
            m_fd = -1;
        }
        m_outputPath.clear();
        emit outputChanged();
        emit errorChanged();
        return false;
    }
    m_source = source;
    /* Connected as a hint, not as the mechanism - the timer does the work. */
    connect(m_source.data(), SIGNAL(readyRead()), this, SLOT(pumpAudio()));

    m_running = true;
    emit runningChanged();
    m_pump.start();

    qDebug() << "AudioFeeder: capturing" << device.deviceName()
             << format.sampleRate() << "Hz"
             << format.channelCount() << "ch"
             << format.sampleSize() << "bit"
             << "to" << m_outputPath
             << "| state" << m_audio->state()
             << "| error" << m_audio->error();
    return true;
}

void AudioFeeder::stop()
{
    finish(QString());
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

void AudioFeeder::handleStateChange(QAudio::State state)
{
    if (!m_audio)
        return;

    qWarning() << "AudioFeeder: audio input state" << state
               << "error" << m_audio->error();

    if (state == QAudio::ActiveState) {
        /* the stream is actually live. Only from this point on does an
         * IdleState mean the stream went away: QAudioInput also emits
         * IdleState at START-UP (its initial state before activation),
         * and treating that as a death signal is what closed the PCM
         * file in the middle of start() and left the pump with a bad
         * file descriptor ("Bad file descriptor" on the first write).
         * It also swallowed the very first run: capture delivered bytes
         * but the file was already closed, so 0 of them landed. */
        m_wasActive = true;
        return;
    }

    if (state == QAudio::StoppedState && m_audio->error() != QAudio::NoError) {
        m_error = QStringLiteral("audio capture failed (device error)");
        emit errorChanged();
        finish(m_error);
    } else if (state == QAudio::IdleState && m_wasActive) {
        /* stream drained and closed (e.g. the sink went away) */
        finish(QString());
    }
}

void AudioFeeder::pumpAudio()
{
    if (!m_source)
        return;

    /* Note what is NOT here: a bytesAvailable() check. FiatVox found
     * that asking politely first is exactly what made the read return
     * nothing at all on the device. */
    for (;;) {
        m_scratch.resize(kChunkBytes);
        const qint64 read = m_source->read(m_scratch.data(), kChunkBytes);
        if (read <= 0)
            break;

        /* Count what was DELIVERED, but check the write: a silent
         * write failure is exactly the failure mode that left a 0-byte
         * file on the device while this counter showed 1.8 MB. Raw
         * write(2) gives a real errno string if it fails. */
        const ssize_t written = write(m_fd, m_scratch.constData(), read);
        if (written < 0 || (size_t)written != (size_t)read) {
            if (!m_writeErrorLogged) {
                m_writeErrorLogged = true;
                qWarning() << "AudioFeeder: write FAILED"
                           << "written" << written << "of" << read
                           << strerror(errno)
                           << "path" << m_outputPath;
            }
            break;
        }

        m_bytes += read;
        emit bytesChanged();

        if (read < kChunkBytes)
            break;
    }
}

void AudioFeeder::finish(const QString &errorText)
{
    /* close the PCM file even if capture never started, so QML can
     * call stop() from any error path; the daemon must see a closed
     * file before it gets EndAudio. lseek(SEEK_CUR) is the ground
     * truth for what actually reached the file. */
    if (m_fd >= 0) {
        const off_t size = lseek(m_fd, 0, SEEK_CUR);
        qDebug() << "AudioFeeder: PCM file" << m_outputPath
                 << "on disk:" << size << "bytes"
                 << "(" << m_bytes << "bytes were delivered from capture)";
        close(m_fd);
        m_fd = -1;
    }

    if (!m_running)
        return;

    m_running = false;
    m_pump.stop();
    if (m_audio) {
        m_audio->stop();
        m_audio->deleteLater();
        m_audio = nullptr;
    }
    m_source = nullptr;

    m_error = errorText;
    emit runningChanged();
    emit errorChanged();
    emit stopped(errorText);

    if (errorText.isEmpty())
        qDebug() << "AudioFeeder stopped:" << m_outputPath
                 << m_bytes << "bytes captured";
    else
        qWarning() << "AudioFeeder stopped with error:" << errorText;
}
