import QtQuick 2.1
import Sailfish.Silica 1.0
import Nemo.DBus 2.0
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
     * call never lands. dbus.call() is the pattern the working
     * reference (screenrecorder) and the app GUI use. */
    function startRecording() {
        if (requestInFlight)
            return
        requestInFlight = true
        inFlightTimer.start()
        startedFromToggle = true
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
     * toggle shows OFF + busy spinner, so a "stop" tap never bounces
     * back to ON. Taps while busy are rejected by the toggle itself. */
    checked: serviceState === 2
    active: serviceState === 2
    busy: serviceState === 3 || requestInFlight
    activeText: qsTr("Recording...")

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
