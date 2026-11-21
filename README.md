# <img src="assets/disk-diver.svg" width="40" height="40" alt=""> Disk-Diver

Disk-Diver shows how much space each folder and file uses on a local Windows or Linux drive. Choose a drive or enter a folder path and select **Scan**. Larger rectangles represent larger entries. Inside a folder, the treemap shows which subfolders and files account for its size.

![Nested treemap and folder table. Videos uses 54 of 110 MiB, including 38 MiB of footage and 16 MiB of exports.](docs/desktop.png)

- Double-click a folder's title bar to zoom in. Hover over a tile to see its path and size. **Back** returns to the previous view, and **Up** opens the parent folder.
- The treemap starts with about 80% of the results area. Drag the divider to give the table more room. A narrow table shows names and the selected size measure. A wider table can also show the other measure and the entry type.
- Right-click a tile or table row to show it in the file manager, copy its full path, rename it, or move it to the **Recycle Bin** on Windows or **Trash** on Linux. Disk-Diver asks for confirmation before moving an entry and rescans after a change.

Scans use your account's permissions by default. Open **Scan options** and select **Use elevated privileges** to authorize a separate process to read protected folders. File actions use the desktop app's permissions. Disk-Diver reports skipped entries and read errors. Select **Cancel** to stop a running scan.

## Install

Open **Releases** on the project's GitHub page and choose the release marked **Latest**. Under **Assets**, download the installer for your 64-bit system. Replace `X.Y.Z` in the examples with the release version.

| System | File | Installation |
| --- | --- | --- |
| Windows x64 | `Disk-Diver-X.Y.Z-windows-x64.exe` | Run the installer, then open **Disk-Diver** from the Start menu. |
| Debian 12 or Ubuntu 24.04, amd64 | `disk-diver_X.Y.Z_amd64.deb` | In your download folder, run `sudo apt install ./disk-diver_X.Y.Z_amd64.deb`. |
| Fedora 44, x86_64 | `disk-diver-X.Y.Z-1.x86_64.rpm` | In your download folder, run `sudo dnf install ./disk-diver-X.Y.Z-1.x86_64.rpm`. |

Official release builds check for new stable versions in the background. Disk-Diver verifies the release signature, downloads the package with your user permissions, and checks its size and SHA-256 digest. Choose **Install Now** to start the update or **Later** to keep working. The **Updates** menu lets you install a downloaded update, check manually, or turn off automatic downloads. Windows opens the installer. On Linux, the system asks you to authorize installation through the package manager.

For a first installation or an upgrade from a version without the updater, download the installer from GitHub. Maintainers can find publishing instructions in the [release and signing guide](docs/releases.md).

## Build and run

The default build requires Rust with Cargo (tested with 1.93), CMake 3.22 or later, a C++20 compiler, Qt 6.2 or later with Widgets, and Python 3.10 or later. The update tests use the Python `cryptography` package. Linux also requires Qt DBus. Use a Release build for performance measurements.

**Windows** requires Visual Studio, the Windows SDK, and a Qt SDK built for your compiler. Set `QTDIR` to your Qt installation prefix, for example the `msvc2022_64` directory.

```powershell
cmake -S . -B build -A x64 "-DCMAKE_PREFIX_PATH=$env:QTDIR"
cmake --build build --config Release --parallel
$env:Path = "$env:QTDIR\bin;$env:Path"
ctest --test-dir build -C Release --output-on-failure
.\build\bin\Release\disk-diver.exe
```

**Linux** requires the Qt development packages, such as `qt6-base-dev` on Debian or Ubuntu.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/bin/disk-diver
```

When started from the build directory, Disk-Diver runs scans with normal permissions. Elevated scans require a protected, system-wide installation. On Windows, the application, scanners, and Python runtime must be installed under **Program Files**. On Linux, root must own those files and their directories, and the desktop session must have a polkit authentication agent. The project provides [packaging scripts](packaging/) for the Windows installer, Debian package, and Fedora RPM.

To build a Debian package, run `bash packaging/linux.sh build dist` after the Release build. Packaging also requires `dpkg-dev`, `qt6-qpa-plugins`, and `python3-cryptography`. On Fedora, configure the build with `-DDISK_DIVER_UPDATE_PLATFORM=linux-rpm-x86_64`, then run `bash packaging/rpm.sh build dist`. Windows packaging uses Inno Setup 6, the official x64 embeddable Python distribution, and Python wheels with pinned SHA-256 digests. [Build workflow](.github/workflows/build.yml) · [Windows packaging script](packaging/windows.ps1)

## File sizes and scan results

- **Logical** is the file's length in bytes. **Allocated** is the space the filesystem reports for the file. The two values can differ substantially for sparse files. Disk-Diver totals the files it scanned. Filesystem metadata, reserved space, snapshots, and shared blocks can affect both the drive's reported usage and how much space deleting a file frees.
- Hard-linked files appear under every name. Within the scan area, their bytes count once, under the lexicographically first native path. The scanner stays on the selected filesystem and skips symlinks, junctions, and nested mounts when traversing folders.
- The scanner reads metadata. On an active filesystem, the results can reflect changes made while the scan was running. When you reopen a scan, Disk-Diver shows the cached result with its timestamp. If the fresh metadata matches byte for byte, the interface reuses the existing index.
- Both scan modes store their caches locally. The normal scan's SQLite cache lives under `%LOCALAPPDATA%\disk-diver` on Windows or `$XDG_CACHE_HOME/disk-diver` on Linux (default `~/.cache/disk-diver`). Elevated scans use a separate cache for each user, accessible with administrator or root privileges. Scans of a parent folder skip the active cache directory. **Clear cache** applies to the selected mode. Canceling a scan preserves the last completed result.

## Architecture and scanner comparison

The C++ and Rust scanners implement the same commands. On Windows they read directory metadata in batches. On Linux they query each entry relative to its parent directory. Both report native filenames, file identities, sizes, and read errors. An index uses parent IDs to calculate folder totals and account for hard links. The Qt client consumes the same stream from either scanner and displays partial results as records arrive. [C++ scanner](cpp/) · [Rust scanner](rust/) · [Binary protocol](docs/protocol.md)

The Python worker coordinates the native scanner, privilege requests, and caching. During a normal scan it forwards binary data and writes it to SQLite in a single transaction. For a full NTFS scan, the worker can use a valid USN change journal to identify and reread affected directories. If the journal has a gap, reports ambiguous changes, or is unavailable, the worker compares the full metadata. On Linux, and for scans of a selected subtree, the next scan checks the tree again from its root.

The status **NTFS change journal checked.** means the scan reflects the changes Windows has recorded. For known open writes, Disk-Diver holds the journal cursor until the file closes. If a file was already open for writing before the oldest retained journal entry, its latest size change may only become visible after a later event. Turn off **Use scan cache** to force a full metadata comparison. [Microsoft's documentation on change journal records](https://learn.microsoft.com/en-us/windows/win32/fileio/change-journal-records)

**Rust is the default scanner on Windows and Linux.** In the NTFS and ext4 comparison, Rust had the shorter median runtime in all six scored cases. On test volumes with just over a million entries each, a warm full scan with index construction took **0.73 seconds on Windows** and **2.37 seconds on Linux**. Configure with `-DDISK_DIVER_ENGINE=cpp` to build the same desktop app with the C++ scanner. [Benchmark results and methodology](bench/README.md)

You can test the scanners independently of Qt.

```sh
cmake -S . -B build-core -DDISK_DIVER_GUI=OFF -DDISK_DIVER_ENGINE=cpp -DCMAKE_BUILD_TYPE=Release
cmake --build build-core --config Release
cargo test --locked --manifest-path rust/Cargo.toml
cargo build --release --locked --manifest-path rust/Cargo.toml
```

Run either scanner with `summary <folder>` for a JSON report. On Windows, `layout <volume-root>` exercises the experimental volume-wide NTFS query in elevated mode. All three methods returned the same 1,008,897 entries in testing. The median time to emit metadata was 3.65 seconds for the volume-wide query and 0.57 seconds for the Rust directory scanner. The experimental `DISK_DIVER_NTFS_LAYOUT` build option is therefore off by default. [NTFS measurements](bench/ntfs-layout.json)

The benchmark report includes the commands, measurements, and selection rule used across platforms. You can measure a full scan, a small subtree, a cache refresh, or a replay of the same metadata separately. [Benchmark report](bench/README.md)

## Contributing and license

Include a test with a known expected result when changing how sizes are calculated. When changing a scan algorithm, compare Release builds with the same directory tree, cache state, and permissions. For bug reports, include the operating system, filesystem, scan mode, and a small reproducible example. You can substitute sample filenames for names from private scans.

After editing the source SVG, run `python packaging/make_icons.py` with `resvg-py==0.5.0` installed to regenerate the desktop app's PNG and ICO assets. [Source icon](assets/disk-diver.svg)

Disk-Diver is licensed under **GPL-3.0-or-later**. [License](LICENSE) · [Notices](NOTICE)
