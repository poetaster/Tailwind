import QtQuick 2.6
import Sailfish.Silica 1.0
import Nemo.DBus 2.0
import Nemo.Notifications 1.0

Page {
    id: page
    property bool startedFromToggle: false

    /* Poll the daemon while it is not yet reachable (state 0). */
    Timer {
        id: pollState
        interval: 1000
        repeat: false
        running: false
        onTriggered: refreshState()
    }

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
            if (pollState.running)
                pollState.stop()
        }

        function recordingFinished(filename) {
            lastFile = filename
            if (startedFromToggle) {
                notification.body = filename
                notification.publish()
                startedFromToggle = false
            }
        }
    }

    function refreshState() {
        try {
            dbus.typedCall("getState", [], function (state) {
                if (serviceState !== state)
                    serviceState = state
                if (serviceState === 0)
                    pollState.start()
            }, function (err) {
                pollState.start()
            })
        } catch (e) {
            pollState.start()
        }
    }

    function applyOptions() {
        try {
            dbus.setFps(Math.round(fpsSlider.value))
            dbus.setBitrate(Math.round(bitrateSlider.value) * 1000000)
        } catch (e) {
            console.warn("TailWind: failed to push options:", e)
        }
    }

    SilicaFlickable {
        id: flick
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column
            width: parent.width

            PageHeader {
                title: qsTr("TailWind")
            }

            TextSwitch {
                text: qsTr("Capture the screen in H.264 (hardware) to MPEG-TS under your Videos folder.")
                font.pointSize: Theme.fontSizeSmall
                wrapMode: Text.Wrap
            }

            Slider {
                id: fpsSlider
                width: parent.width
                minimumValue: 1
                maximumValue: 30
                stepSize: 1
                value: 24
                label: qsTr("Frames per second")
                valueText: qsTr("%1 fps").arg(value)
                onReleased: applyOptions()
            }

            Slider {
                id: bitrateSlider
                width: parent.width
                minimumValue: 2
                maximumValue: 30
                stepSize: 1
                value: 24
                label: qsTr("Bitrate")
                valueText: qsTr("%1 Mbps").arg(value)
                onReleased: applyOptions()
            }

            Button {
                id: actionButton
                anchors.horizontalCenter: parent.horizontalCenter
                text: {
                    switch (serviceState) {
                    case 0: return qsTr("Loading")
                    case 1: return qsTr("Record")
                    case 2: return qsTr("Stop")
                    case 3: return qsTr("Saving, please wait")
                    default: return qsTr("...")
                    }
                }
                enabled: serviceState === 1 || serviceState === 2
                onClicked: {
                    if (serviceState === 1) {
                        startedFromToggle = true
                        applyOptions()
                        try { dbus.Start() }
                        catch (e) { console.warn("TailWind: Start failed:", e) }
                    } else if (serviceState === 2) {
                        try { dbus.Stop() }
                        catch (e) { console.warn("TailWind: Stop failed:", e) }
                    }
                }
            }

            Label {
                id: filenameLabel
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: Theme.horizontalPageMargin
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                text: {
                    if (serviceState === 2) return qsTr("Recording in progress\u2026")
                    if (lastFile) return qsTr("Saved: %1").arg(lastFile)
                    return ""
                }
                visible: text !== ""
            }

            property string lastFile: ""
        }

        VerticalScrollDecorator {}
    }

    Connections {
        target: appWindow
        onCoverTriggered: {
            if (actionButton.enabled)
                actionButton.clicked()
        }
    }

    Component.onCompleted: refreshState()

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
