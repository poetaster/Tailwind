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

# The name of your application (must match qml/TailWind.qml + TailWind.desktop
# + the icon filenames; distinct from the recorder daemon binary "tailwind" and
# the D-Bus service "de.poetaster.tailwind")
TARGET = TailWind

CONFIG += sailfishapp

SOURCES += src/main.cpp \
          src/audiofeeder.cpp \
          src/streamfilter.cpp

HEADERS += src/audiofeeder.h \
           src/streamfilter.h

LIBS += -lpulse

# QAudioInput for AudioFeeder (PulseAudio backend); streamfilter.cpp
# uses libpulse directly
QT += dbus multimedia

DISTFILES += \ \
    qml/TailWind.qml \
    qml/cover/CoverPage.qml \
    qml/pages/FirstPage.qml \
    rpm/TailWind.changes.in \
    rpm/TailWind.changes.run.in \
    rpm/TailWind.spec \
    translations/*.ts \
    TailWind.desktop

SAILFISHAPP_ICONS = 86x86 108x108 128x128 256x256

# to disable building translations every time, comment out the
# following CONFIG line
CONFIG += sailfishapp_i18n

TRANSLATIONS += translations/TailWind-de.ts
