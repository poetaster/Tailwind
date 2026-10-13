import QtQuick 2.1
import Sailfish.Silica 1.0
import Nemo.DBus 2.0
import com.jolla.settings 1.0
import com.jolla.settings.system 1.0
import Nemo.Notifications 1.0
import org.nemomobile.lipstick 0.1 as Lipstick

SettingsToggle {
    id: root

    /* 0 = Idle/Loading, 1 = Ready, 2 = Recording, 3 = Saving */
    property int serviceState: 0
    property bool startedFromToggle: false

    DBusInterface {
        id: dbus
        service: "de.poetaster.tailwind"
        path: "/de/poetaster/tailwind"
        iface: "de.poetaster.tailwind"
        bus: DBus.SessionBus
        signalsEnabled: true

        function stateChanged(newState) {
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
            try {
                dbus.typedCall("getState", [], function (state) {
                    if (serviceState !== state)
                        serviceState = state
                })
            } catch (e) {
                serviceState = 0
            }
        }
    }

    Lipstick.LauncherItem {
        id: settingsLauncher
        filePath: "/usr/share/applications/TailWind.desktop"
    }

    name: qsTr("Record screen")
    icon.source: "image://theme/icon-m-camera"
    checked: active
    active: serviceState > 1
    busy: serviceState > 2
    activeText: serviceState === 2 ? qsTr("Recording...")
                                   : qsTr("Saving...")

    onToggled: {
        if (serviceState === 1) {
            startedFromToggle = true
            try { dbus.Start() }
            catch (e) { console.warn("TailWind: Start failed:", e) }
        } else if (serviceState === 2) {
            try { dbus.Stop() }
            catch (e) { console.warn("TailWind: Stop failed:", e) }
        }
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
