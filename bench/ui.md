# Time to a navigable view

The Qt benchmark opens the application's main window and starts a scan using the Rust scanner and Python worker. Both timers start when the scan begins. The first stops when the top-level table is navigable and its treemap has been calculated, using either live results or a cached snapshot. The second stops when the worker finishes and the sorted, verified view is ready. The benchmark uses Qt's offscreen plugin, so the measurements exclude the system compositor.

## Windows

The benchmark ran on Windows 11 with MSVC and Qt 6.8.3. The dedicated NTFS test volume was a dynamically expanding VHDX on a Samsung SSD 970 EVO Plus. Scans used normal user permissions and a warm operating system cache. Peak memory is the Qt process's peak working set. Every run reported zero read errors and zero entries with unknown allocated size. [Individual runs](ui-windows.json) · [Hardware and test tree](scale-windows.json)

| Scan | Entries | Runs | Median time to first view | Median time to verified view | Median peak memory |
| --- | ---: | ---: | ---: | ---: | ---: |
| Full volume, application cache off | 1,008,897 | 5 | 245 ms | 876 ms | 167.4 MiB |
| Subtree, application cache off | 2,066 | 5 | 189 ms | 202 ms | 24.8 MiB |
| Full volume, first scan with caching | 1,008,897 | 1 | 246 ms | 941 ms | 167.8 MiB |
| Full volume, verify unchanged cache | 1,008,897 | 3 | 257 ms | 872 ms | 189.4 MiB |

## Linux

The release build used GCC 12.2 and Qt 6.4.2 on Debian 12 in Docker Desktop with WSL2, on an AMD Ryzen 9 5900XT. The dedicated ext4 volume contained one million regular test files. These runs followed the native scanner benchmarks and used normal permissions and a warm operating system cache. GNU time reported peak memory with `%M`. Measuring the simultaneous memory use of all processes would require a separate measurement. Every run reported zero read errors and zero entries with unknown allocated size. [Individual runs](ui-linux.json) · [Hardware and test tree](scale-linux.json)

| Scan | Entries | Runs | Median time to first view | Median time to verified view | Median peak memory |
| --- | ---: | ---: | ---: | ---: | ---: |
| Full volume, application cache off | 1,008,884 | 5 | 262 ms | 3,058 ms | 153.9 MiB |
| Subtree, application cache off | 2,066 | 5 | 90 ms | 98 ms | 29.8 MiB |
| Full volume, first scan with caching | 1,008,884 | 1 | 262 ms | 2,491 ms | 153.7 MiB |
| Full volume, verify unchanged cache | 1,008,884 | 3 | 261 ms | 2,561 ms | 154.0 MiB |

The cached tree remains navigable throughout the Linux scan. If the metadata matches, the worker confirms the existing tree. A separate development benchmark measured the effect with the C++ scanner. The previous implementation built a second index after verification and took a median of 4,708 ms and 236.2 MiB over three runs. Reusing the verified index brought the medians to 2,974 ms and 153.7 MiB over five runs with the same scanner. Individual times ranged from 2,871 to 6,711 ms. The two series ran consecutively on a shared development system.

## Reproduce the measurements

Build the release configuration with `BUILD_TESTING=ON`. To measure one Linux scan, run:

```sh
QT_QPA_PLATFORM=offscreen ./build/bin/disk-diver-ui-test --timing /path/to/start-folder --no-cache
```

On Windows, set `$env:QT_QPA_PLATFORM = 'offscreen'` and run `.\build\bin\Release\disk-diver-ui-test.exe --timing D:\TestTree --no-cache`. Qt must be available as it is for the desktop build. The command prints JSON with the elapsed times, the source of the first view and the entry count.

Omit `--no-cache` to measure repeated scans with application caching. Point `XDG_CACHE_HOME` on Linux or `LOCALAPPDATA` on Windows at an empty directory for the first run and reuse it for subsequent runs. To compare runs, keep the file tree, permissions, initial cache state and scan root the same.
