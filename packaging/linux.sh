#!/usr/bin/env bash
set -euo pipefail
if (( $# < 1 || $# > 2 )); then
    echo 'Usage: bash packaging/linux.sh BUILD_DIR [OUTPUT_DIR]' >&2
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
arch=$(dpkg --print-architecture)
cmake --build "$build" --config Release --parallel
cmake --install "$build" --config Release --prefix "$prefix"
mkdir -p "$app" "$doc" "$stage/DEBIAN" "$stage/usr/bin" "$stage/usr/share/applications"
install -m755 "$prefix/bin/disk-diver" "$prefix/bin/disk-diver-cpp" "$app/"
install -m644 "$prefix/bin/"{worker.py,dd_journal.py,dd_privilege.py} "$app/"
install -m644 "$prefix/bin/"{disk-diver-engine.txt,disk-diver-ntfs-layout.txt} "$app/"
executables=(-e"$app/disk-diver" -e"$app/disk-diver-cpp")
if [[ -f "$prefix/bin/disk-diver-rust" ]]; then
    install -m755 "$prefix/bin/disk-diver-rust" "$app/"
    executables+=(-e"$app/disk-diver-rust")
fi
install -m644 "$prefix/share/disk-diver/LICENSE" "$doc/copyright"
install -m644 "$prefix/share/disk-diver/NOTICE" "$doc/"
install -m644 "$packaging/disk-diver.desktop" "$stage/usr/share/applications/"
ln -s ../lib/disk-diver/disk-diver "$stage/usr/bin/disk-diver"

# Ask dpkg for the actual ABI dependencies of this distro's Qt and C++ runtime.
mkdir "$work/debian"
cat > "$work/debian/control" <<'CONTROL'
Source: disk-diver
Section: utils
Priority: optional
Maintainer: Disk-Diver contributors

Package: disk-diver
Architecture: any
Description: Local disk usage analyzer
CONTROL
libraries=$(cd "$work" && dpkg-shlibdeps -O "${executables[@]}")
libraries=${libraries#shlibs:Depends=}
test -n "$libraries"
python3 -I -S -B "$packaging/smoke.py" "$app"
cat > "$stage/DEBIAN/control" <<CONTROL
Package: disk-diver
Version: $version
Architecture: $arch
Section: utils
Priority: optional
Maintainer: Disk-Diver contributors
Depends: $libraries, qt6-qpa-plugins, python3 (>= 3.10), pkexec | policykit-1
Description: Visual disk usage analyzer for local drives
 Inspect logical and allocated sizes for directories and files.
CONTROL
umask 022
find "$stage" -type d -exec chmod 755 {} +
chmod 644 "$stage/DEBIAN/control"
target="$out/disk-diver_${version}_${arch}.deb"
dpkg-deb --build --root-owner-group "$stage" "$target"
dpkg-deb --info "$target" >/dev/null
