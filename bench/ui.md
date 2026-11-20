# Zeit bis zur navigierbaren Ansicht

Das Qt-Messprogramm öffnet das reguläre Hauptfenster und startet einen Scan mit dem Rust-Scanner über den Python-Worker. Die erste Zeit endet, sobald die erste Ordnerebene in der Tabelle navigierbar und die zugehörige Treemap berechnet ist. Das kann eine vorläufige Live-Ansicht oder ein gespeicherter Stand sein. Die zweite Zeit endet nach Abschluss des Workers und Bereitstellung der sortierten, überprüften Ansicht. Beide Zeiten beginnen beim Start des Scans. Der System-Compositor ist bei der Messung mit dem Qt-Offscreen-Plugin außen vor.

## Windows

Windows 11, MSVC und Qt 6.8.3. Das dedizierte NTFS-Testvolume lag als dynamische VHDX auf einer Samsung SSD 970 EVO Plus. Der Scan lief mit gewöhnlichen Benutzerrechten und warmem Betriebssystemcache. Die Speicherspitze ist das maximale Working Set des Qt-Prozesses. Alle Läufe meldeten null Lesefehler und null unbekannte Belegungen. [Einzelmessungen](ui-windows.json) · [Hardware und Dateibaum](scale-windows.json)

| Auswahl | Einträge | Läufe | Erste Ansicht, Median | Verifiziert, Median | Speicherspitze, Median |
| --- | ---: | ---: | ---: | ---: | ---: |
| Volume ohne Anwendungscache | 1.008.897 | 5 | 245 ms | 876 ms | 167,4 MiB |
| Startordner ohne Anwendungscache | 2.066 | 5 | 189 ms | 202 ms | 24,8 MiB |
| Volume, erstmalig mit Cache | 1.008.897 | 1 | 246 ms | 941 ms | 167,8 MiB |
| Volume, unveränderten Cache bestätigen | 1.008.897 | 3 | 257 ms | 872 ms | 189,4 MiB |

## Linux

Release-Build mit GCC 12.2 und Qt 6.4.2 unter Debian 12 in Docker Desktop/WSL2, AMD Ryzen 9 5900XT. Das separate ext4-Testvolume enthielt eine Million reguläre Testdateien. Die Messungen erfolgten im normalen Scanmodus nach den nativen Benchmarks, bei warmem Betriebssystemcache. Die Spitze ist der von GNU time (`%M`) erfasste Speicherbedarf. Gleichzeitig belegter Speicher über mehrere Prozesse müsste getrennt gemessen werden. Alle Läufe meldeten null Lesefehler und null unbekannte Belegungen. [Einzelmessungen](ui-linux.json) · [Hardware und Dateibaum](scale-linux.json)

| Auswahl | Einträge | Läufe | Erste Ansicht, Median | Verifiziert, Median | Speicherspitze, Median |
| --- | ---: | ---: | ---: | ---: | ---: |
| Volume ohne Anwendungscache | 1.008.884 | 5 | 262 ms | 3.058 ms | 153,9 MiB |
| Startordner ohne Anwendungscache | 2.066 | 5 | 90 ms | 98 ms | 29,8 MiB |
| Volume, erstmalig mit Cache | 1.008.884 | 1 | 262 ms | 2.491 ms | 153,7 MiB |
| Volume, unveränderten Cache bestätigen | 1.008.884 | 3 | 261 ms | 2.561 ms | 154,0 MiB |

Der gespeicherte Baum bleibt während des Linux-Scans navigierbar. Bei identischen Metadaten bestätigt der Worker diesen Baum direkt. In einer separaten Entwicklungsreihe mit dem C++-Scanner zeigte sich der Effekt dieser Änderung. Die frühere Fassung baute nach der Prüfung einen zweiten Index auf und brauchte bei drei Wiederholungen im Median 4.708 ms und 236,2 MiB. Mit direkter Bestätigung lagen fünf Wiederholungen mit demselben Scanner bei 2.974 ms und 153,7 MiB. Die einzelnen Laufzeiten reichten von 2.871 bis 6.711 ms. Die Messreihen liefen nacheinander auf einem gemeinsam genutzten Entwicklungssystem.

## Nachmessen

Den Release-Build mit `BUILD_TESTING=ON` erstellen. Für einen einzelnen Messlauf unter Linux genügt dieses Kommando.

```sh
QT_QPA_PLATFORM=offscreen ./build/bin/disk-diver-ui-test --timing /pfad/zum/startordner --no-cache
```

Unter Windows `$env:QT_QPA_PLATFORM = 'offscreen'` setzen und `.\build\bin\Release\disk-diver-ui-test.exe --timing D:\Testbaum --no-cache` starten. Qt muss wie beim regulären Desktop-Build erreichbar sein. Der Prozess gibt die Zeiten, die Quelle der ersten Ansicht und die Zahl der erkannten Einträge als JSON aus.

Für Wiederholungen mit Anwendungscache `--no-cache` weglassen. Ein leeres, separates `XDG_CACHE_HOME` beziehungsweise `LOCALAPPDATA` für den ersten Lauf setzen und für Folgeläufe beibehalten. Für einen belastbaren Vergleich müssen Baum, Rechte, Cachezustand und Auswahlwurzel übereinstimmen.
