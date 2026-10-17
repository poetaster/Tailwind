#include <QtQuick>
#include <sailfishapp.h>

#include "audiofeeder.h"
#include "streamfilter.h"

int main(int argc, char *argv[])
{
    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    QScopedPointer<QQuickView> view(SailfishApp::createView());

    /* Microphone capture. The GUI appends the raw PCM to a shared file
     * (AudioFeeder::prepare()); the daemon tails that file into the AAC
     * encoder, so no per-chunk D-Bus round-trips stall the pump. */
    AudioFeeder *audioFeeder = new AudioFeeder(view.data());
    view->rootContext()->setContextProperty(QStringLiteral("audioFeeder"), audioFeeder);

    /* Mutes the mic/sidetone playback streams while a recording is
     * running, so microphone audio does not leak into the capture
     * (ported from Sail-Recorder, proven on this device). */
    StreamFilter *streamFilter = new StreamFilter(view.data());
    view->rootContext()->setContextProperty(QStringLiteral("streamFilter"), streamFilter);

    view->setSource(SailfishApp::pathToMainQml());
    view->show();

    return app->exec();
}
