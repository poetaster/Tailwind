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
#include <QSocketNotifier>
#include <QTimer>

#include <signal.h>
#include <unistd.h>

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

/* Self-pipe: the ONLY thing a POSIX signal handler may do here is a
 * write().  QCoreApplication::quit was the source of the SEGV on exit
 * (systemd SIGTERMs a Type=dbus service as soon as the bus name is
 * released during our own quit -- the old handler then re-entered
 * Recorder::handleShutDown() on a thread that tearing down).
  */
static int s_termPipe[2] = { -1, -1 };

static void handleShutDownSignal(int)
{
    if (s_termPipe[1] >= 0) {
        const char c = 1;
        (void)write(s_termPipe[1], &c, 1);
    }
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

    /* SIGINT/SIGTERM arrive in signal context: no Qt, no locks, no malloc.
     * The handler writes one byte here; the main thread reacts and performs
     * the real shutdown (stop in-flight recording, wait for the worker,
     * quit), so even `systemctl stop` during a recording finalises the file. */
    if (pipe(s_termPipe) == 0) {
        QSocketNotifier *sn = new QSocketNotifier(s_termPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(sn, &QSocketNotifier::activated, &app, [recorder](int fd) {
            char buf[64];
            while (read(fd, buf, sizeof(buf)) > 0) { }
            qCDebug(logmain) << "SIGINT/SIGTERM received; stopping and exiting";
            recorder->handleShutDown();
            qApp->quit();
        });
    }

    if (daemonize) {
        qCDebug(logmain) << "Running as D-Bus daemon (de.poetaster.tailwind)";
        setShutDownSignal(SIGINT);   /* Ctrl-C */
        setShutDownSignal(SIGTERM);  /* systemctl stop / kill */
        QObject::connect(recorder, &Recorder::shutdownRequested, qApp, [recorder]() {
            recorder->handleShutDown();
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        });
        QTimer::singleShot(0, &initializeDBus);
    } else {
        /* One-shot foreground run: start immediately, stop on SIGINT/SIGTERM. */
        setShutDownSignal(SIGINT);
        setShutDownSignal(SIGTERM);
        QTimer::singleShot(0, recorder, &Recorder::init);
    }

    return app.exec();
}
