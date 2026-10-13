# NOTICE:
#
# Application name defined in TARGET has a corresponding QML filename.
# If name defined in TARGET is changed, the following needs to be done
# to match new name:
#   - corresponding QML filename must be changed
#   - desktop icon filename must be changed
#   - desktop filename must be changed
#   - icon definition filename in desktop file must be changed
#   - translation filenames have to be changed

# The name of your application
TARGET = TailWind

CONFIG += sailfishapp

HEADERS += src/wayland-lipstick-recorder-client-protocol.h
HEADERS += \
    src/droidrec.h

# External libraries required by the recorder
LIBS += -lwayland-client
LIBS += -ldroidmedia
LIBS += -ldl
LIBS += -lavformat
LIBS += -lavcodec
LIBS += -lavutil
SOURCES += src/TailWind.cpp
SOURCES += \
    src/droidrec.c
SOURCES += src/wayland-lipstick-recorder-protocol.c

DISTFILES += qml/TailWind.qml \
    qml/cover/CoverPage.qml \
    qml/pages/FirstPage.qml \
    qml/pages/SecondPage.qml \
    rpm/TailWind.changes.in \
    rpm/TailWind.changes.run.in \
    rpm/TailWind.spec \
    translations/*.ts \
    TailWind.desktop

SAILFISHAPP_ICONS = 86x86 108x108 128x128 172x172

# to disable building translations every time, comment out the
# following CONFIG line
CONFIG += sailfishapp_i18n

# German translation is enabled as an example. If you aren't
# planning to localize your app, remember to comment out the
# following TRANSLATIONS line. And also do not forget to
# modify the localized app name in the the .desktop file.
TRANSLATIONS += translations/TailWind-de.ts
