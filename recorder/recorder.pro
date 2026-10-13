# TailWind recorder - non-privileged daemon (QCoreApplication + QtDBus)
# Wraps the droidrec (Wayland capture + droidmedia HW H.264 + libav TS mux)
# pipeline behind a start()/stop() API, exposed over the D-Bus session bus
# as de.poetaster.tailwind.

TEMPLATE = app
TARGET = tailwind

target.path = /usr/sbin
INSTALLS += target

QT += core dbus concurrent platformsupport-private

# MDConfGroup (dconf) comes from the mlite5 package, same as the reference
CONFIG += link_pkgconfig
PKGCONFIG += mlite5
INCLUDEPATH += /usr/include/mlite5

SOURCES += \
    src/main.cpp \
    src/recorder.cpp \
    src/dbusadaptor.cpp \
    src/droidrec.c \
    src/wayland-lipstick-recorder-protocol.c

HEADERS += \
    src/recorder.h \
    src/dbusadaptor.h \
    src/droidrec.h \
    src/wayland-lipstick-recorder-client-protocol.h

# External libraries required by the recorder pipeline (same set as the
# original single-binary TailWind build). droidmedia pulls in the mlite
# HW codec backend at runtime on the device.
LIBS += -lwayland-client -ldroidmedia -ldl -lavformat -lavcodec -lavutil -lpthread

# D-Bus service activation (session bus) + system.d policy
dbusService.files = dbus/de.poetaster.tailwind.service
dbusService.path = /usr/share/dbus-1/services/
INSTALLS += dbusService

dbusConf.files = dbus/de.poetaster.tailwind.conf
dbusConf.path = /usr/share/dbus-1/system.d/
INSTALLS += dbusConf

# systemd user/system unit for autostart
systemd.files = systemd/tailwind.service
systemd.path = $$SPEC_UNITDIR
INSTALLS += systemd

DEFINES += QT_DEPRECATED_WARNINGS
DEFINES += QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII

isEmpty(PROJECT_PACKAGE_VERSION) {
    VERSION = 0.1.0
} else {
    VERSION = $$PROJECT_PACKAGE_VERSION
}
DEFINES += PROJECT_PACKAGE_VERSION=\\\"$$VERSION\\\"
