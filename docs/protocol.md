# Lokaler Scanner-Datenstrom

`scan ROOT` schreibt nur Binärdaten nach stdout. Diagnosen gehen nach stderr, fatale Fehler setzen einen von null verschiedenen Exitcode. `list ROOT` verwendet dieselben Regeln und begrenzt die Ausgabe auf eine Ebene. `summary ROOT` berechnet einen JSON-Bericht. `replay CAPTURE` berechnet denselben Bericht aus einem gespeicherten Scanner-Datenstrom.

Ein nativer Datenstrom beginnt mit den vier ASCII-Bytes `DDV1`. Die Einträge folgen in Eltern-vor-Kind-Reihenfolge. Ihre Position ab null ist die lokale Knoten-ID. Alle Ganzzahlen sind little-endian.

| Feld | Typ | Bedeutung |
| --- | --- | --- |
| Art | u8 | 1 Ordner, 2 Datei, 3 Link oder sonstiger Eintrag, 4 Lesefehler |
| Eltern-ID | u64 | ID des Elternknotens, an der Wurzel UINT64_MAX |
| Logische Größe | u64 | Dateilänge in Bytes |
| Belegung | u64 | Zugewiesene Bytes, UINT64_MAX für unbekannte Belegung |
| Geräte-ID, Datei-ID | je u64 | Zusammen die Identität für Hardlinks, Datei-ID 0 bei unbekannter Identität |
| Namenslänge | u32 | Anzahl folgender Bytes |
| Name | Bytes | Einzelne Pfadkomponente, UTF-16LE unter Windows und Originalbytes unter Linux |

Die Wurzel ist ein Ordner mit leerem Namen. Ein namenloser Fehler unter einem Ordner bezeichnet einen Fehler beim Lesen seines Inhalts. Ein einzelnes Byte mit Wert 5 beendet den Datenstrom. Die Auswertung behandelt eine fehlende Endmarke oder einen fehlerhaften Prozessabschluss als abgebrochenen Scan.

Der Worker ergänzt nach der Kennung ein Byte für den Anzeigezustand. 6 bezeichnet den gespeicherten Stand, 7 die aktuelle Erfassung. Stimmt eine Prüfung exakt mit dem bereits übertragenen gespeicherten Stand überein, folgen Kennung, Statusbyte 8 und die Endmarke 5. Die Oberfläche übernimmt dann den bereits aufgebauten Index als aktuell. Der Cache speichert das native Format ohne Anzeigezustand. Zeitstempel und Status laufen getrennt über stderr. Die Qt-Oberfläche kann `cancel\n` an den Worker senden.
