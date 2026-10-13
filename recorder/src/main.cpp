/*
 * TailWind - screen recorder daemon.
 *
 * Runs as a D-Bus service (de.poetaster.tailwind on the session bus) that
 * owns the droidrec capture/encode backend. The GUI and the Settings toggle
 * drive it over D-Bus: Start / Stop / Quit, and set Destination / Fps /
 * Bitrate / Seconds before starting a recording.
 */

#include "recorder.h"
#include "dbusadaptor.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QLoggingCategory>
#include <QTimer>

#include <signal.h>

Q_LOGGING_CATEGORY(logmain, "tailwind.main", QtDebugMsg)

static void initializeDBus()
{
    DBusAdaptor *adaptor = new DBusAdaptor(qApp);
    if (!adaptor->registerService()) {
        qCWarning(logmain) << "D-Bus registration failed, exiting";
        qApp->quit();
        return;
    }
    Recorder::instance()->init();
}

static void handleShutDownSignal(int)
{
    Recorder::instance()->handleShutDown();
    qApp->quit();
}

static void setShutDownSignal(int signalId)
{
    struct sigaction sa;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handleShutDownSignal;
    if (sigaction(signalId, &sa, nullptr) == -1) {
        perror("setting up termination signal");
        exit(1);
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("tailwind"));
    app.setOrganizationName(QStringLiteral("de.poetaster"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("TailWind screen recorder (droidrec backend)"));
    parser.addHelpOption();

    QCommandLineOption fpsOption(
            QStringLiteral("fps"),
        app.translate("main", "Framerate. Default is 24."),
        app.translate("main", "framerate"),
            QStringLiteral("24"));
    parser.addOption(fpsOption);

    QCommandLineOption bitrateOption(
            QStringLiteral("bitrate"),
            app.translate("main", "Video bitrate in bits/second. Default is backend (24000000)."));
    parser.addOption(bitrateOption);

    QCommandLineOption secondsOption(
            QStringLiteral("seconds"),
            app.translate("main", "Record for N seconds (0 = until stopped). Default 0."),
            app.translate("main", "seconds"),
            QStringLiteral("0"));
    parser.addOption(secondsOption);

    QCommandLineOption destinationOption(
            QStringLiteral("destination"),
            app.translate("main", "Output directory for recordings."),
            app.translate("main", "destination"));
    parser.addOption(destinationOption);

    QCommandLineOption daemonOption(
            {QStringLiteral("d"), QStringLiteral("daemon")},
            app.translate("main", "Run as a D-Bus daemon on the session bus."));
    parser.addOption(daemonOption);

    parser.process(app);

    const bool daemonize = parser.isSet(daemonOption);

    Recorder::Options options = Recorder::readOptions();
    if (parser.isSet(fpsOption))
        options.fps = parser.value(fpsOption).toInt();
    if (parser.isSet(bitrateOption))
        options.bitrate = parser.value(bitrateOption).toInt();
    if (parser.isSet(secondsOption))
        options.seconds = parser.value(secondsOption).toInt();
    if (parser.isSet(destinationOption))
        options.destination = parser.value(destinationOption);

    Recorder *recorder = new Recorder(&app);
    recorder->setOptions(options);

    if (daemonize) {
        qCDebug(logmain) << "Running as D-Bus daemon (de.poetaster.tailwind)";
        setShutDownSignal(SIGINT);   /* Ctrl-C */
        setShutDownSignal(SIGTERM);  /* systemctl stop / kill */
        QTimer::singleShot(0, &initializeDBus);
    } else {
        /* One-shot foreground run: start immediately, stop on SIGINT/SIGTERM. */
        setShutDownSignal(SIGINT);
        setShutDownSignal(SIGTERM);
        QTimer::singleShot(0, recorder, &Recorder::init);
    }

    return app.exec();
}
