%global debug_package %{nil}

Name: disk-diver
Version: %{dd_version}
Release: 1
Summary: Visual disk usage analyzer for local drives
License: GPL-3.0-or-later
ExclusiveArch: x86_64
Requires: python3 >= 3.10
Requires: python3-cryptography
Requires: qt6-qtbase-gui
Requires: polkit
Requires: dnf

%description
Inspect logical and allocated sizes for directories and files.

%install
mkdir -p "%{buildroot}"
cp -a "%{dd_payload}/." "%{buildroot}/"

%files
%{_bindir}/disk-diver
/usr/lib/disk-diver/
%{_datadir}/applications/disk-diver.desktop
%{_datadir}/icons/hicolor/scalable/apps/disk-diver.svg
%{_datadir}/icons/hicolor/256x256/apps/disk-diver.png
%dir %{_docdir}/disk-diver/
%license %{_docdir}/disk-diver/copyright
%doc %{_docdir}/disk-diver/NOTICE
