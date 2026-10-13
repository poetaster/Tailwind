import QtQuick 2.0
import Sailfish.Silica 1.0

CoverBackground {
    Label {
        id: label
        anchors.centerIn: parent
        text: {
            if ( serviceState === 2 ) return qsTr("Recording\u2026")
            if ( serviceState === 1 ) return qsTr("Ready \u2013 tap to record")
            if ( serviceState > 2 ) return qsTr("Saving\u2026")
            return qsTr("Connecting\u2026")
        }
    }

    CoverActionList {
        id: coverAction
        enabled: serviceState > 0 && serviceState < 3

        CoverAction {
            iconSource: serviceState === 1
                ? "image://theme/icon-cover-camera"
                : "image://theme/icon-cover-cancel"
            onTriggered: {
                coverTriggered()
            }
        }
    }
}
