%define theme sailfish-default
%define _binary_payload w2.xzdio

%{!?qtc_qmake5:%define qtc_qmake5 %qmake5}
%{!?qtc_make:%define qtc_make make}

Name:       TailWind
Summary:    Sailfish screen recorder
Version:    0.1.0
Release:    1
Group:      System/GUI/Other
License:    GPLv3
URL:        https://github.com/poetaster/Tailwind
Source0:    %{name}-%{version}.tar.bz2

Requires:   sailfishsilica-qt5 >= 0.10.9a
Requires:   ffmpeg
Requires:   ffmpeg-tools
Requires:   pulseaudio

BuildRequires:  pkgconfig(sailfishapp) >= 1.0.2
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  desktop-file-utils
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Gui)
BuildRequires:  pkgconfig(Qt5DBus)
BuildRequires:  pkgconfig(Qt5Concurrent)
BuildRequires:  droidmedia-devel
BuildRequires:  ffmpeg-devel
BuildRequires:  qt5-qtplatformsupport-devel
BuildRequires:  qt5-qtwayland-wayland_egl-devel
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(libpulse)
BuildRequires:  pkgconfig(mlite5)
BuildRequires:  systemd
BuildRequires:  sailfish-svg2png


%description
TailWind is a SailfishOS screen recorder with a top-menu toggle,
app for settings and recording on a timer. Videos is hardware compressed.

%if "%{?vendor}" == "chum"
PackageName: Tailwind
Type: desktop-application
Categories:
 - Video
 - Graphics
PackagerName: Mark Washeim (poetaster)
Custom:
 - Repo: https://github.com/poetaster/Tailwind
PackageIcon: https://github.com/poetaster/Tailwind/blob/main/gui/icons/256x256/TailWind.png?raw=true
Url:
 - Bugtracker: https://github.com/poetaster/Tailwind/issues
Links:
  Homepage: https://github.com/poetaster/Tailwind
  Bugtracker: https://github.com/poetaster/Tailwind/issues
  Donation: https://liberapay.com/poetaster
%endif
%prep
%setup -q -n %{name}-%{version}

%build
%qtc_qmake5 \
  "PROJECT_PACKAGE_VERSION=%{version}" \
  SPEC_UNITDIR=%{_userunitdir}
%qtc_make %{_smp_mflags}

%install
rm -rf %{buildroot}
%qmake5_install

%post
systemctl-user start tailwind.service

%preun
systemctl-user stop tailwind.service

%postun
systemctl-user stop tailwind.service


%files
%defattr(-,root,root,-)
%attr(2755, root, privileged) %{_sbindir}/tailwind

%attr(755, root, root) %{_bindir}/TailWind
%{_datadir}/TailWind
%{_datadir}/applications/TailWind.desktop
%{_datadir}/icons/hicolor/*/apps/TailWind.png

%{_datadir}/dbus-1/services/de.poetaster.tailwind.service
%{_datadir}/dbus-1/system.d/de.poetaster.tailwind.conf
%{_userunitdir}/tailwind.service

%{_datadir}/jolla-settings/entries/tailwind.json
%{_datadir}/jolla-settings/pages/tailwind/TailWindToggle.qml
