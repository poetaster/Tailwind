#include "dbusadaptor.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDBusConnection>
#include <QDBusError>
#include <QLoggingCategory>

static const QString s_dbusService   = QStringLiteral("de.poetaster.tailwind");
static const QString s_dbusObject    = QStringLiteral("/de/poetaster/tailwind");
static const QString s_dbusInterface = QStringLiteral("de.poetaster.tailwind");

Q_LOGGING_CATEGORY(logadaptor, "tailwind.adaptor", QtDebugMsg)

DBusAdaptor::DBusAdaptor(QObject *parent)
    : QDBusAbstractAdaptor(parent)
{
    setAutoRelaySignals(true);

    Recorder *rec = Recorder::instance();
    if (rec) {
        connect(rec, &Recorder::statusChanged, this, &DBusAdaptor::StateChanged);
        connect(rec, &Recorder::recordingFinished, this, &DBusAdaptor::RecordingFinished);
    }
}

DBusAdaptor::~DBusAdaptor()
{
    unregisterService();
}

int DBusAdaptor::GetState() const
{
    return Recorder::instance() ? Recorder::instance()->status() : Recorder::StatusIdle;
}

QString DBusAdaptor::GetDestination() const
{
    return Recorder::instance() ? Recorder::instance()->options().destination : QString();
}

void DBusAdaptor::SetDestination(const QString &destination)
{
    if (!Recorder::instance())
        return;
    Recorder::Options o = Recorder::instance()->options();
    o.destination = destination;
    Recorder::instance()->setOptions(o);
    qCDebug(logadaptor) << Q_FUNC_INFO << destination;
}

int DBusAdaptor::GetFps() const
{
    return Recorder::instance() ? Recorder::instance()->options().fps : 0;
}

void DBusAdaptor::SetFps(int fps)
{
    if (!Recorder::instance() || fps <= 0)
        return;
    Recorder::Options o = Recorder::instance()->options();
    o.fps = fps;
    Recorder::instance()->setOptions(o);
    qCDebug(logadaptor) << Q_FUNC_INFO << fps;
}

int DBusAdaptor::GetBitrate() const
{
    return Recorder::instance() ? Recorder::instance()->options().bitrate : 0;
}

void DBusAdaptor::SetBitrate(int bitrate)
{
    if (!Recorder::instance() || bitrate <= 0)
        return;
    Recorder::Options o = Recorder::instance()->options();
    o.bitrate = bitrate;
    Recorder::instance()->setOptions(o);
    qCDebug(logadaptor) << Q_FUNC_INFO << bitrate;
}

int DBusAdaptor::GetSeconds() const
{
    return Recorder::instance() ? Recorder::instance()->options().seconds : 0;
}

void DBusAdaptor::SetSeconds(int seconds)
{
    if (!Recorder::instance() || seconds < 0)
        return;
    Recorder::Options o = Recorder::instance()->options();
    o.seconds = seconds;
    Recorder::instance()->setOptions(o);
    qCDebug(logadaptor) << Q_FUNC_INFO << seconds;
}

bool DBusAdaptor::registerService()
{
    QDBusConnection bus = QDBusConnection::sessionBus();

    const bool registerServiceSuccess = bus.registerService(s_dbusService);
    qCDebug(logadaptor) << Q_FUNC_INFO << "Service registered:" << registerServiceSuccess;
    if (!registerServiceSuccess) {
        qCWarning(logadaptor) << Q_FUNC_INFO << bus.lastError().message();
        return false;
    }

    const bool registerObjectSuccess = bus.registerObject(s_dbusObject, s_dbusInterface, qApp);
    qCDebug(logadaptor) << Q_FUNC_INFO << "Object registered:" << registerObjectSuccess;
    if (!registerObjectSuccess) {
        qCWarning(logadaptor) << Q_FUNC_INFO << bus.lastError().message();
        return false;
    }
    return true;
}

void DBusAdaptor::unregisterService()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    const bool unregisterServiceSuccess = bus.unregisterService(s_dbusService);
    qCDebug(logadaptor) << Q_FUNC_INFO << "Service unregistered:" << unregisterServiceSuccess;
    if (!unregisterServiceSuccess) {
        qCWarning(logadaptor) << Q_FUNC_INFO << bus.lastError().message();
        return;
    }
    bus.unregisterObject(s_dbusObject);
}

void DBusAdaptor::Quit()
{
    qCDebug(logadaptor) << Q_FUNC_INFO;
    Recorder *rec = Recorder::instance();
    if (rec)
        rec->handleShutDown();
    qApp->quit();
}

void DBusAdaptor::Start()
{
    qCDebug(logadaptor) << Q_FUNC_INFO;
    Recorder::instance()->start();
}

void DBusAdaptor::Stop()
{
    qCDebug(logadaptor) << Q_FUNC_INFO;
    /* stop() finalises on a worker thread; the Recorder emits
     * recordingFinished() once the file is saved, which is relayed here. */
    if (Recorder::instance())
        Recorder::instance()->stop();
}
