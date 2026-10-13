TEMPLATE = aux

settingsjson.files = tailwind.json
settingsjson.path = /usr/share/jolla-settings/entries

INSTALLS += settingsjson

settingsqml.files = TailWindToggle.qml
settingsqml.path = /usr/share/jolla-settings/pages/tailwind

INSTALLS += settingsqml
