import QtQuick 2.6
import Sailfish.Silica 1.0
import Nemo.DBus 2.0
import Nemo.Configuration 1.0
import Nemo.Notifications 1.0
import QtMultimedia 5.6

Page {
    id: page
    property bool startedFromToggle:false

    MediaPlayer {
        id: tickPlayer
        source: Qt.resolvedUrl("../sounds/tick.wav")
        autoPlay: false
    }
    Timer {
        id: repeatedGetState
        interval: 1000
        repeat: false
        onTriggered: {
            dbus.typedCall("GetState", [], function(initialState) {
                serviceState = initialState
            }, function() {
                repeatedGetState.start()
            })
        }
    }
    Timer {
        id: delayTimer
        interval: 1000
        repeat: false
        running:false
        onTriggered: {
            trigger();
        }
        function trigger() {
            if (serviceState == 1) {
                dbus.call("Start")
            }
            delayTimer.stop();
            tickTimer.stop();
        }
    }
    Timer {
        id: tickTimer
        interval: 1000
        repeat: true
        running:false
        onTriggered: {
            tickPlayer.play()
        }
    }

    DBusInterface {
        id: dbus
        service: "de.poetaster.tailwind"
        path: "/de/poetaster/tailwind"
        iface: "de.poetaster.tailwind"
        bus: DBus.SessionBus

        signalsEnabled: true

        function stateChanged(newState) {
            serviceState = newState
            if (repeatedGetState.running) {
                repeatedGetState.stop()
            }
        }

        function recordingFinished(filename) {
            if (startedFromToggle) {
                notification.body = filename
                notification.publish()
                startedFromToggle = false
            }
        }

        Component.onDestruction: {
            //call("Quit")
        }

        Component.onCompleted: {
            dbus.typedCall("GetState", [], function(initialState) {
                serviceState = initialState
            });
        }
    }

    ConfigurationGroup {
        id: conf
        path: "/de/poetaster/tailwind"
        property int fps: 24
        property int bitrate: 24
        property int startdelay: 0
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
            Slider {
                id: startDelaySlider
                width: parent.width
                minimumValue: 0
                maximumValue: 10
                stepSize: 1
                value: conf.startdelay
                label: qsTr("Start delay")
                valueText: qsTr("%1 s").arg(value)
                onReleased: {
                    delayTimer.interval = value * 1000
                    conf.startdelay = value
                }
            }
            Slider {
                id: fpsSlider
                width: parent.width
                minimumValue: 1
                maximumValue: 24
                stepSize: 1
                value: conf.fps
                label: qsTr("Frames per second")
                valueText: qsTr("%1 fps").arg(value)
                onReleased: {
                    conf.fps = value
                }
            }

            Slider {
                id: bitrateSlider
                width: parent.width
                minimumValue: 2
                maximumValue: 30
                stepSize: 1
                value: conf.bitrate
                label: qsTr("Bitrate")
                valueText: qsTr("%1 Mbit/s").arg(value)
                onReleased: {
                    conf.bitrate = value
                }
            }

            Button {
                id: actionButton
                anchors.horizontalCenter: parent.horizontalCenter
                text: {
                    switch (serviceState) {
                    case 0:
                        return qsTr("Loading")
                    case 1:
                        return qsTr("Record")
                    case 2:
                        return qsTr("Stop")
                    case 3:
                        return qsTr("Saving, please wait")
                    }
                }
                enabled: serviceState > 0 && serviceState < 3
                onClicked: {
                    trigger()
                }
                function trigger() {
                    if (serviceState == 1) {
                        startedFromToggle = true
                        delayTimer.running = true
                        tickTimer.running = true
                    } else if (serviceState == 2) {
                        dbus.call("Stop")
                    }
                }

            }

            Label {
                id: filenameLabel
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: Theme.horizontalPageMargin
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                visible: text
            }
        }

        VerticalScrollDecorator {}
    }

    Connections {
        target: appWindow
        onCoverTriggered: {
            console.log("cover")
            if (actionButton.enabled) {
                actionButton.trigger()
            }
        }
    }
    Component.onCompleted: {
        dbus.typedCall("GetState", [], function(initialState) {
            serviceState = initialState
            delayTimer.interval = conf.startdelay * 1000
        });
        if (serviceState == 0) {
            repeatedGetState.start()
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
        summary: "Recording completed"
    }

}
