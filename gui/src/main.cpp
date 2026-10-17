#include <QtQuick>
#include <sailfishapp.h>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>

#include "sinklist.h"
#include "audiofeeder.h"
#include "streamfilter.h"

int main(int argc, char *argv[])
{
    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    QScopedPointer<QQuickView> view(SailfishApp::createView());

    /* PulseAudio sink enumeration for the audio sink combo box */
    SinkList *sinkList = new SinkList(view.data());
    view->rootContext()->setContextProperty(QStringLiteral("sinkList"), sinkList);

    /* System audio capture (sink monitor), piped to the daemon over D-Bus.
     * The pump runs on the GUI thread and the daemon's FeedAudio never
     * blocks (it drops the oldest samples when its FIFO is full), so a
     * blocking call keeps the stream in order. */
    AudioFeeder *audioFeeder = new AudioFeeder(view.data());
    view->rootContext()->setContextProperty(QStringLiteral("audioFeeder"), audioFeeder);

    /* Mutes the mic/sidetone playback streams while a recording is
     * running, so microphone audio does not leak into the monitor
     * capture (ported from Sail-Recorder, proven on this device). */
    StreamFilter *streamFilter = new StreamFilter(view.data());
    view->rootContext()->setContextProperty(QStringLiteral("streamFilter"), streamFilter);

    QDBusInterface *tailwindBus = new QDBusInterface(
        QStringLiteral("de.poetaster.tailwind"),
        QStringLiteral("/de/poetaster/tailwind"),
        QStringLiteral("de.poetaster.tailwind"),
        QDBusConnection::sessionBus(), view.data());
    tailwindBus->setTimeout(500);   /* cap each blocking call */
    int feedFailures = 0;
    audioFeeder->setChunkCallback([tailwindBus, &feedFailures](const char *data, size_t n) {
        const QDBusMessage reply = tailwindBus->call(
            "FeedAudio",
            QByteArray::fromRawData(data, static_cast<qint32>(n)));
        if (reply.type() == QDBusMessage::ErrorMessage) {
            if (feedFailures++ == 0)   /* log the first failure, not all 50/s */
                qWarning() << "AudioFeeder: FeedAudio failed:" << reply;
            return;
        }
        if (!reply.arguments().isEmpty() && reply.arguments().at(0).toInt() != 0) {
            if (feedFailures++ == 0)
                qWarning() << "AudioFeeder: FeedAudio rejected:" << reply.arguments().at(0);
        }
    });

    view->setSource(SailfishApp::pathToMainQml());
    view->show();

    return app->exec();
}
