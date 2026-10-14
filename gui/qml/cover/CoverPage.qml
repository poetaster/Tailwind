import QtQuick 2.0
import Sailfish.Silica 1.0

CoverBackground {
    Label {
        id: name
        text: "Tailwind"
        anchors.bottom: label.top
        anchors.horizontalCenter: parent.horizontalCenter
    }
    Label {
        id: label
        anchors.centerIn: parent
        text: {
            if ( serviceState === 2 ) return qsTr("Recording...")
            if ( serviceState === 3 ) return qsTr("Saving...")
            if ( serviceState === 1 ) return qsTr("Ready")
            return qsTr("TailWind")
        }
    }

    CoverActionList {
        id: coverAction
        enabled: serviceState > 0 && serviceState < 4

        CoverAction {
            iconSource: (serviceState === 2 || serviceState === 3)
                        ? "image://theme/icon-cover-cancel"
                        : "image://theme/icon-cover-location"
            onTriggered: {
                coverTriggered()
            }
        }
    }
}
