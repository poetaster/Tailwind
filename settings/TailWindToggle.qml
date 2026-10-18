import QtQuick 2.1
import Sailfish.Silica 1.0
import Nemo.DBus 2.0
import Nemo.Configuration 1.0
import com.jolla.settings 1.0
import com.jolla.settings.system 1.0
import Nemo.Notifications 1.0
import org.nemomobile.lipstick 0.1 as Lipstick

SettingsToggle {
    id: root

    /* 0 = unknown/not started, 1 = Ready, 2 = Recording, 3 = Saving */
    property int serviceState: 0
    property bool startedFromToggle: false
    property bool requestInFlight: false

    /* Same dconf store the GUI app writes to; the toggle only needs
     * the audio gate (audiosink). */
    ConfigurationGroup {
        id: conf
        path: "/de/poetaster/tailwind"
        property string audiosink: ""   /* "" = video only */
    }

    Timer {
        id: inFlightTimer
        interval: 5000
        repeat: false
        onTriggered: requestInFlight = false
    }

    DBusInterface {
        id: dbus
        service: "de.poetaster.tailwind"
        path: "/de/poetaster/tailwind"
        iface: "de.poetaster.tailwind"
        bus: DBus.SessionBus
        signalsEnabled: true

        function stateChanged(newState) {
            requestInFlight = false
            inFlightTimer.stop()
            if (serviceState !== newState)
                serviceState = newState
        }

        function recordingFinished(filename) {
            if (startedFromToggle) {
                notification.body = filename
                notification.publish()
                startedFromToggle = false
            }
        }

        Component.onCompleted: {
            dbus.typedCall("GetState", [], function (state) {
                if (serviceState !== state)
                    serviceState = state
            })
        }
    }

    /* NOTE: use dbus.call(), not dbus.typedCall(), for Start/Stop.
     * Stop() is declared Q_NOREPLY; Nemo.DBus's callback-based
     * typedCall() does not complete for no-reply methods, so the
     * call never returns.
     */
    function startRecording() {
        if (requestInFlight)
            return
        requestInFlight = true
        inFlightTimer.start()
        startedFromToggle = true
        /* This path never involves the GUI's audio capture pipeline, so
         * the recording must be video-only: clear the sink setting before
         * Start. The daemon re-reads dconf on every Start, and an empty
         * value means no AAC track is prepared (a prepared-but-never-fed
         * AAC track is what produced the unplayable files). The dconf
         * write completes before the Start message is dispatched, so the
         * daemon always sees it. */
        conf.audiosink = ""
        dbus.call("Start")
    }

    function stopRecording() {
        if (requestInFlight)
            return
        requestInFlight = true
        inFlightTimer.start()
        dbus.call("Stop")
    }

    Lipstick.LauncherItem {
        id: settingsLauncher
        filePath: "/usr/share/applications/TailWind.desktop"
    }

    name: qsTr("Record screen")
    icon.source: "image://theme/icon-m-camera"

    /* ON only while actually recording. While saving (state 3) the
     * toggle shows OFF so a "stop" tap never bounces
     * back to ON. Taps while busy are rejected by the toggle itself. */
    checked: active
    active: serviceState > 1
    busy: serviceState > 2 || requestInFlight
    activeText: serviceState == 2 ? "Recording..."
                                  : "Saving..."
    onToggled: {
        if (serviceState <= 1) {
            startRecording()
        } else if (serviceState === 2) {
            stopRecording()
        }
        /* state 3: already blocked by `busy` (in-progress error) */
    }

    menu: ContextMenu {
        SettingsMenuItem {
            onClicked: settingsLauncher.launchApplication()
        }
    }

    Notification {
        id: notification
        icon: "image://theme/icon-m-camera"
        appIcon: "image://theme/TailWind"
        appName: "TailWind"
        category: "x-nemo.general.warning"
        previewBody: body
        previewSummary: summary
        summary: qsTr("Recording saved")
    }
}
