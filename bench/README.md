# Disk-Diver benchmarks

Use release builds, Python 3.10 or later, and a quiet test filesystem. The generated fixture is deterministic. Its primary tree contains mostly small files, wide and nested directories, a hardlink across the subtree boundary, and a controlled group of files for size changes and renames. The runner restores that group after the last round. A default smoke fixture needs a new directory and usually takes seconds to create.

```sh
python bench/fixture.py /tmp/dd-fixture --files 2048
python bench/run.py --fixture /tmp/dd-fixture --cpp build/bin/disk-diver-cpp --rust rust/target/release/disk-diver-rust --output smoke.json
```

On Windows, substitute the local paths and `.exe` filenames. To measure a complete volume, put a new fixture with `--files 1000000` on a quiet dedicated test volume. Pass its mount root as `--full-root`. Place `--temp` and `--output` on a different volume, supply `--hardware`, `--storage`, `--filesystem`, and add `--production` to attest the test conditions. Reference runs must see stable contents and matching permissions. For example:

```sh
python bench/fixture.py /mnt/dd/reference --files 1000000
python bench/run.py --fixture /mnt/dd/reference --full-root /mnt/dd --cpp build/bin/disk-diver-cpp --rust rust/target/release/disk-diver-rust --rounds 5 --hardware "CPU and RAM" --storage "SSD model" --filesystem ext4 --production --output linux.json
```

The runner checks both scanners against each other and both index implementations against one shared DDV1 capture. On the generated tree it also checks expected logical bytes and file count. Measured wall times include process startup. The four cases are:

| Case | Timed operation |
| --- | --- |
| Full | Scan the selected volume or smoke tree and build the full in-memory index |
| Subtree | Scan and index only the selected directory |
| Refresh | Load the previous real SQLite cache, scan after deterministic edits, stream both generations and commit the new result |
| Replay | Rebuild the index from identical captured metadata |

Each core has a separate cache. Each refresh checks the historical and fresh frames and replays the fresh frame against the current reference. The candidate order alternates each round. The worker uses ordinary user mode and records the observed refresh strategy. Elapsed time to a rendered frame, user-input latency and simultaneous total process-tree memory require a separate GUI measurement. Windows reports peak working set. Linux reports `wait4` peak RSS, which can include inherited launcher memory on very short tasks. Raw JSON contains every round and the OS-specific peak readings.

The application benchmark measures when a user can start navigating the Qt interface. [Qt measurements](ui.md)

For first-scan measurements, prepare the fixture, restart the host, then run exactly one candidate using `python bench/cold.py PATH_TO_BINARY VOLUME_ROOT --state first-after-reboot --output cold.json`. Give the second candidate equivalent conditions after another restart. The `uncontrolled-first-run` label is available for an observed first run without a controlled cache reset. Neither script flushes the operating system cache.

Pass `--gdu PATH` or `--wiztree PATH` to time optional installed external tools. The report keeps full JSON or CSV export timings in `external`. These include export overhead. Record tool versions alongside results. [gdu export](https://github.com/dundee/gdu) · [WizTree CSV export](https://diskanalyzer.com/guide)

## Architecture decision

```sh
python bench/select_core.py windows.json linux.json
```

The selector needs one qualified report per platform using the same worker SHA-256. It compares warm medians for full scan, subtree and changed refresh on each platform. A core qualifies if every case finishes within 15% of that case's fastest time. When only one qualifies, it wins. When both qualify, the six-case geometric-mean time decides, with C++ chosen when the difference is below 10%. If both miss the guardrail, profile once, rerun both platforms, and add `--after-profile`. The lower maximum relative slowdown then wins, with a tie favoring C++. Peak memory and replay timings are reported alongside the decision.

After measuring, `python bench/fixture.py FIXTURE --remove` removes the generated files that still contain the original bytes. Restore the controlled edit set before cleanup.

### Windows full-mode acceptance

An elevated run compared every native record from NTFS layout enumeration and both directory scanners on the temporary NTFS volume. Names, parent identities, file identities, kinds and both size fields matched for 1,008,897 entries. Six alternating warm runs streamed the complete metadata to the same sink. Median elapsed times were 571 ms for Rust, 1,336 ms for C++ and 3,647 ms for the layout query. The shipped build uses Rust for full-volume enumeration. [Native measurements](ntfs-layout.json)

On the same volume the USN refresh matched a fresh Rust traversal after create, rename, delete, two writes on a continuously open handle, close and final deletion. Reopening an unchanged scan promoted the existing preview. A separate UAC run verified the protected cache from a normal desktop process. Its unelevated account could not list the cache directory. [Acceptance fixture](../tests/check_ntfs.py)

## Observed smoke runs

Five warmed rounds, 2,077 entries in the full tree and 23 in the selected subtree. Four files changed size per refresh and two were renamed. Both implementations returned 114,001 logical bytes before edits. The Windows run used a local NTFS directory, and Linux used a Docker Desktop WSL2 container with OverlayFS. Both cache refreshes used ordinary directory traversal. Values are medians in milliseconds.

| Host | C++ full | Rust full | C++ subtree | Rust subtree | C++ refresh | Rust refresh | C++ replay | Rust replay |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Windows 11 | 83.80 | 84.92 | 18.30 | 19.12 | 274.02 | 294.08 | 17.34 | 16.75 |
| Linux WSL2 | 8.17 | 7.46 | 1.90 | 1.23 | 111.63 | 108.18 | 2.21 | 1.75 |

These small runs verify the comparison procedure. The million-file section below covers the architecture decision. [Windows samples](smoke-windows.json) · [Linux samples](smoke-linux.json)

## Million-file runs

**Rust won the core comparison.** It had the fastest median in all six decision cases. C++ took 17.0% longer by geometric mean, with its largest gap at 34.1% on the NTFS cache refresh. Both reports used the same cache coordinator, SHA-256 `40e7f4ce8b2be9dcdd39f5d5d919fe460020c18f20d4295065a702f9dfa79621`. Their refresh cases deliberately changed the tree. The current desktop can also reuse a saved preview after it verifies an unchanged result. [Windows samples](scale-windows.json) · [Linux samples](scale-linux.json)

The dedicated 8 GiB NTFS VHDX was mounted on a Windows 11 host with a Ryzen 9 5900XT, 32 GiB of RAM and a Samsung 970 EVO Plus NVMe SSD. Both cores used the same elevated token and ordinary directory traversal. The full volume contained **1,008,897 entries**, including NTFS-visible system and acceptance-fixture files. Both scanners returned 56,086,546 logical bytes and 59,559,064 allocated bytes, with every allocation known. The warm medians summarize five runs. [Windows samples](scale-windows.json)

| Windows case | C++ | Rust |
| --- | ---: | ---: |
| Full scan and index | 918.93 ms | 732.28 ms |
| Selected subtree, 2,066 entries | 16.70 ms | 15.87 ms |
| Reload, rescan after changes, commit and stream | 1.832 s | 1.367 s |
| Build index from identical capture | 529.99 ms | 243.75 ms |
| Peak working set during full scan, median | 142.9 MiB | 134.0 MiB |

On the same NTFS volume, gdu 5.39.0 wrote a 63,565,081-byte JSON export in **698.87 ms** at **469.4 MiB** median peak working set. The Windows run used `-p -o` because gdu's `-x` mount discovery is implemented for Linux. The VHDX held one NTFS filesystem. [gdu 5.39 release](https://github.com/dundee/gdu/releases/tag/v5.39.0)

The ext4 volume in a Debian 12 container on WSL2 held **1,008,884 entries**. Both engines found 56,053,686 logical bytes and 4,063,813,632 allocated bytes. Each refresh resized 1,000 files and renamed 500. On a Ryzen 9 5900XT with 15 GiB of guest RAM, these were the medians of five warm runs. [Linux samples](scale-linux.json)

| Linux case | C++ | Rust |
| --- | ---: | ---: |
| Full scan and index | 2.682 s | 2.369 s |
| Selected subtree, 2,066 entries | 7.93 ms | 6.27 ms |
| Reload, rescan after changes, commit and stream | 2.774 s | 2.742 s |
| Build index from identical capture | 522.75 ms | 308.51 ms |
| Peak RSS during full scan, median | 108.6 MiB | 120.0 MiB |

Debian's gdu 5.22.0 produced a full JSON export in **760.93 ms** at **734.3 MiB** median peak RSS. A separate validation export contained all 1,001,002 files and 7,882 directories and occupied 65,213,054 bytes. The Disk-Diver full-scan command keeps a navigable index in memory and prints a short JSON summary. These timings cover different outputs. [gdu export format](https://github.com/dundee/gdu)
