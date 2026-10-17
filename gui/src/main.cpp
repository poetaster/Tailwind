#include <QtQuick>
#include <sailfishapp.h>

#include "sinklist.h"

int main(int argc, char *argv[])
{
    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    QScopedPointer<QQuickView> view(SailfishApp::createView());

    /* PulseAudio sink enumeration for the audio sink combo box */
    SinkList *sinkList = new SinkList(view.data());
    view->rootContext()->setContextProperty(QStringLiteral("sinkList"), sinkList);

    view->setSource(SailfishApp::pathToMainQml());
    view->show();

    return app->exec();
}
