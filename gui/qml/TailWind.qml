import QtQuick 2.0
import Sailfish.Silica 1.0
import "pages"

ApplicationWindow {
    id: appWindow

    initialPage: Component { FirstPage { } }
    cover: Qt.resolvedUrl("cover/CoverPage.qml")
    allowedOrientations: defaultAllowedOrientations

    /* 0 = Idle/Loading, 1 = Ready, 2 = Recording, 3 = Saving */
    property int serviceState: 0
    signal coverTriggered
}
