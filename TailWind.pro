TEMPLATE = subdirs
SUBDIRS = \
    recorder \
    gui \
    settings

gui.depends = recorder
settings.depends = recorder

OTHER_FILES += \
    rpm/TailWind.spec \
    rpm/TailWind.changes.in \
    rpm/TailWind.changes.run.in
