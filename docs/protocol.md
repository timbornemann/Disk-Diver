# Local scanner stream

`scan ROOT` writes binary data to stdout and diagnostics to stderr. Fatal errors produce a nonzero exit code. `list ROOT` follows the same rules and limits the output to one directory level. `summary ROOT` produces a JSON report. `replay CAPTURE` produces the same report from a saved scanner stream.

A native stream starts with the four ASCII bytes `DDV1`. Each parent appears before its children. A record's zero-based position is its node ID within the stream. All integers are little-endian.

| Field | Type | Meaning |
| --- | --- | --- |
| Kind | u8 | 1 for a directory, 2 for a file, 3 for a link or other entry, 4 for a read error |
| Parent ID | u64 | Parent node ID, or UINT64_MAX for the root |
| Logical size | u64 | File length in bytes |
| Allocated size | u64 | Allocated bytes, or UINT64_MAX if unknown |
| Device ID, file ID | u64 each | Together they identify hard links. A file ID of 0 means the identity is unknown. |
| Name length | u32 | Number of name bytes that follow |
| Name | bytes | One path component, encoded as UTF-16LE on Windows or the original filename bytes on Linux |

The root is a directory with an empty name. An unnamed error record under a directory means the scanner could not read that directory's contents. A single byte with value 5 ends the stream. The reader treats a missing end marker or an unsuccessful process exit as an interrupted scan.

The Python worker adds a status byte after the `DDV1` header. A value of 6 identifies a cached snapshot and 7 identifies a fresh scan. If verification exactly matches the snapshot already sent, the worker sends the header, status byte 8 and end marker 5. The UI can then mark its existing index as current. The cache stores the native stream without the status byte. Timestamps and status messages travel separately on stderr. The Qt UI can send `cancel\n` to the worker.
