import QtQuick 2.6
import Sailfish.Silica 1.0
import Nemo.DBus 2.0
import Nemo.Configuration 1.0
import Nemo.Notifications 1.0
import QtMultimedia 5.6

Page {
    id: page
    property bool startedFromToggle:false
    property bool audioArmed:false   /* sink selected; start feed when state==2 */
    /* BeginAudio succeeded on the daemon and EndAudio has not been sent
     * yet. Independent of audioFeeder.running: if the capture dies
     * mid-recording the daemon still has the audio pipeline open and
     * MUST get EndAudio, or droidrec_stop() hangs on the audio thread. */
    property bool daemonAudio:false

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
                if (conf.audiosink !== "") {
                    /* the daemon is not recording yet; start the feed once
                     * it reaches state 2 (see stateChanged below) */
                    console.log("audio armed for sink:", conf.audiosink)
                    audioArmed = true
                }
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
            if (newState == 2 && audioArmed) {
                audioArmed = false
                startAudioFeed()
            } else if (newState != 2) {
                /* the recording is ending (normal stop, top menu, or the
                 * daemon stopped itself): never let the capture or the
                 * stream filter outlive it, and always close the daemon's
                 * audio pipeline if it was opened */
                endDaemonAudio()
                if (streamFilter && streamFilter.active) {
                    streamFilter.stop()
                }
            }
        }

        function startAudioFeed() {
            var pcmPath = audioFeeder.prepare()
            if (pcmPath === "") {
                console.warn("audio: prepare() failed:", audioFeeder.error)
                return
            }
            console.log("audio: PCM file prepared at", pcmPath,
                        "source:", conf.audiosink)
            /* No arguments: the daemon opens the same fixed path
             * (/home/defaultuser/Videos/tailwind_audio.pcm) by convention.
             * Passing the path
             * as a typed argument is what Nemo.DBus rejects on device
             * with "Invalid type"; no-argument calls are the
             * proven-working pattern (Start, Stop, EndAudio, GetState). */
            dbus.typedCall("BeginAudio", [], function(rc) {
                if (rc != 0) {
                    console.warn("BeginAudio rejected (rc", rc + "): video-only recording")
                    audioFeeder.stop()   /* close the prepared PCM file */
                    return
                }
                daemonAudio = true
                /* mute the mic/sidetone playback streams BEFORE the
                 * capture starts, so microphone audio can't leak into
                 * the recording (Sail-Recorder's proven approach) */
                var filterOk = (streamFilter && streamFilter.start())
                if (!audioFeeder.start(conf.audiosink)) {
                    console.warn("audio capture failed to start:", audioFeeder.error)
                    endDaemonAudio()
                    if (streamFilter && streamFilter.active)
                        streamFilter.stop()
                    return
                }
                if (!filterOk)
                    console.warn("stream filter unavailable; mic may leak into the recording")
            }, function(err) {
                console.warn("BeginAudio call failed:", err)
                audioFeeder.stop()   /* close the prepared PCM file */
            })
        }

        /* close the daemon's audio pipeline exactly once, whether or not
         * the capture is still alive: the daemon's Stop path waits for
         * EndAudio before it can flush and finish.
         * Ordering matters: audioFeeder.stop() closes (flushes) the PCM
         * file FIRST, so the daemon's tailer sees the end of file before
         * EndAudio tells it to drain. */
        function endDaemonAudio() {
            if (!daemonAudio)
                return
            daemonAudio = false
            audioFeeder.stop()
            dbus.call("EndAudio")
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
        property int scale: 1
        property int startdelay: 0
        property string audiosink: ""   /* pulse sink name; empty = video only */
    }

    ListModel {
        id: audioSinkModel
        /* capture sources: "mic" = direct microphone (source.primary_input,
         * verified clean on the device); "" = video only. The Pulse
         * sink-monitor (loopback) path is not offered: the droid-HAL
         * monitor tap is voice-processed and sounds distorted on the
         * device, whatever sink is chosen. */
        Component.onCompleted: {
            clear();
            append({ name: "",    label: qsTr("No audio") });
            append({ name: "mic", label: qsTr("Microphone") });
            /* restore the saved selection */
            var sel = 0;
            for (var j = 0; j < audioSinkModel.count; j++) {
                if (audioSinkModel.get(j).name === conf.audiosink) {
                    sel = j;
                    break;
                }
            }
            audioSinkCombo.currentIndex = sel;
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

            ComboBox {
                id: audioSinkCombo
                width: parent.width
                label: qsTr("Audio")
                currentIndex: 0
                onCurrentIndexChanged: {
                    if (currentIndex >= 0 && currentIndex < audioSinkModel.count) {
                        conf.audiosink = audioSinkModel.get(currentIndex).name
                    } else {
                        conf.audiosink = ""
                    }
                }
                menu: ContextMenu {
                    Repeater {
                        model: audioSinkModel
                        MenuItem {
                            text: label
                            font.pixelSize: Theme.fontSizeExtraSmall
                        }
                    }
                }
            }

            Label {
                id: audioStatus
                width: parent.width
                visible: text !== ""
                text: audioFeeder.running
                    ? qsTr("Capturing: %1 KiB %2").arg(Math.round(audioFeeder.bytes / 1024)).arg(audioFeeder.output)
                    : (audioFeeder.error !== "" ? qsTr("Audio error: %1").arg(audioFeeder.error) : "")
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
                        console.log("stop called from button or top menu")
                        /* tell the daemon the stream is done (even if the
                         * capture already died); Stop() then drains the
                         * FIFO and flushes the AAC. dbus.*: endDaemonAudio
                         * is a member of the DBusInterface object, not a
                         * global function (QML scopes are per-object). */
                        dbus.endDaemonAudio()
                        /* release every sink input the filter muted */
                        if (streamFilter && streamFilter.active) {
                            streamFilter.stop()
                        }
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
