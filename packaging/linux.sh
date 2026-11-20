#!/usr/bin/env bash
set -euo pipefail
format=deb
if [[ ${1:-} == --rpm ]]; then
    format=rpm
    shift
fi
if (( $# < 1 || $# > 2 )); then
    echo 'Usage: bash packaging/linux.sh [--rpm] BUILD_DIR [OUTPUT_DIR]' >&2
    exit 2
fi
build=$(realpath "$1")
out=$(realpath -m "${2:-dist}")
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
stage="$work/package"
prefix="$work/install"
app="$stage/usr/lib/disk-diver"
doc="$stage/usr/share/doc/disk-diver"
packaging=$(dirname "$(realpath "$0")")
version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=\([0-9]\+\.[0-9]\+\.[0-9]\+\)$/\1/p' "$build/CMakeCache.txt")
test -n "$version"
if [[ $format == rpm ]]; then
    platform=linux-rpm-x86_64
    test "$(rpm --eval '%{_arch}')" = x86_64 || { echo 'The RPM release targets x86_64.' >&2; exit 1; }
else
    platform=linux-deb-amd64
    test "$(dpkg --print-architecture)" = amd64 || { echo 'The Debian release targets amd64.' >&2; exit 1; }
fi
cmake --build "$build" --config Release --parallel
cmake --install "$build" --config Release --prefix "$prefix"
mkdir -p "$app" "$doc" "$stage/usr/bin"
install -m755 "$prefix/bin/disk-diver" "$prefix/bin/disk-diver-cpp" "$app/"
install -m644 "$prefix/bin/"{worker.py,dd_journal.py,dd_privilege.py,updater.py,install_update.py} "$app/"
install -m644 "$prefix/bin/"{disk-diver-engine.txt,disk-diver-ntfs-layout.txt,disk-diver-update.json} "$app/"
if [[ -f "$prefix/bin/disk-diver-rust" ]]; then
    install -m755 "$prefix/bin/disk-diver-rust" "$app/"
fi
install -m644 "$prefix/share/disk-diver/LICENSE" "$doc/copyright"
install -m644 "$prefix/share/disk-diver/NOTICE" "$doc/"
for asset in applications/disk-diver.desktop icons/hicolor/scalable/apps/disk-diver.svg icons/hicolor/256x256/apps/disk-diver.png; do
    install -Dm644 "$prefix/share/$asset" "$stage/usr/share/$asset"
done
ln -s ../lib/disk-diver/disk-diver "$stage/usr/bin/disk-diver"

/usr/bin/python3 -I -B "$packaging/smoke.py" "$app" --update-platform "$platform"
umask 022
find "$stage" -type d -exec chmod 755 {} +
if [[ $format == rpm ]]; then
    rpmbuild -bb "$packaging/rpm.spec" --define "_topdir $work/rpmbuild" \
        --define "dd_version $version" --define "dd_payload $stage"
    target="disk-diver-$version-1.x86_64.rpm"
    rpm -qp "$work/rpmbuild/RPMS/x86_64/$target" >/dev/null
    install -m644 "$work/rpmbuild/RPMS/x86_64/$target" "$out/$target"
    exit 0
fi

# Ask dpkg for the actual ABI dependencies of this distro's Qt and C++ runtime.
mkdir "$work/debian" "$stage/DEBIAN"
cat > "$work/debian/control" <<'CONTROL'
Source: disk-diver
Section: utils
Priority: optional
Maintainer: Disk-Diver contributors

Package: disk-diver
Architecture: any
Description: Local disk usage analyzer
CONTROL
executables=(-e"$app/disk-diver" -e"$app/disk-diver-cpp")
if [[ -f "$app/disk-diver-rust" ]]; then
    executables+=(-e"$app/disk-diver-rust")
fi
libraries=$(cd "$work" && dpkg-shlibdeps -O "${executables[@]}")
libraries=${libraries#shlibs:Depends=}
test -n "$libraries"
cat > "$stage/DEBIAN/control" <<CONTROL
Package: disk-diver
Version: $version
Architecture: amd64
Section: utils
Priority: optional
Maintainer: Disk-Diver contributors
Depends: $libraries, qt6-qpa-plugins, python3 (>= 3.10), python3-cryptography, pkexec | policykit-1, apt
Description: Visual disk usage analyzer for local drives
 Inspect logical and allocated sizes for directories and files.
CONTROL
chmod 644 "$stage/DEBIAN/control"
target="$out/disk-diver_${version}_amd64.deb"
dpkg-deb --build --root-owner-group "$stage" "$target"
dpkg-deb --info "$target" >/dev/null
