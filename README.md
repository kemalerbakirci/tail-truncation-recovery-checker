build/wal_write_recover <mode> <file> [args...]

# Tail Truncation Recovery Checker (C++17)

**Goal:** Verify that a WAL file with records `[u32 len][payload][u32 crc]` is recoverable after a crash that cuts the **tail** mid-record. On startup, the tool scans, detects a bad tail (length/CRC mismatch or EOF), and **truncates** safely to the last good record.

- **Durability focus:** sequential append, CRC, tail truncation.
- **Why this matters:** power loss most often corrupts only the final record; recovery must *never* expose a torn entry to readers.

---

## Project Structure

```
├── build.sh                # Build script
├── LICENSE                 # License file (MIT)
├── README.md               # This documentation
├── .gitignore              # Git ignore rules
├── wal_write_recover       # Compiled binary (after build)
├── *.wal                   # WAL files (created by the tool)
└── src/
    └── wal_write_recover.cpp  # Main C++ source code
```

---

## Build
```bash
chmod +x build.sh && ./build.sh
```

## CLI
```
./wal_write_recover <mode> <file> [args...]

Modes:
  write <file> <N> <payload_bytes>   # append N entries of payload size
  corrupt <file> <bytes_to_cut>      # cut bytes from end (simulate crash)
  recover <file>                     # scan & truncate to last good record
  demo <file> <N> <payload_bytes>    # write -> corrupt 1/2 of last record -> recover
```

### Examples
```bash
./wal_write_recover write demo.wal 100 256
./wal_write_recover corrupt demo.wal 150
./wal_write_recover recover demo.wal
./wal_write_recover demo demo.wal 50 512
```

### Expected Output (sample)
```
[write] wrote 50 entries, bytes=26050
[corrupt] truncated 256 bytes from end; size 25800 -> 25544
[recover] scanned 49 good entries, truncated tail from offset=25490 to size=25490
[recover] OK: Recovered 49 entries, no parse error.
```

## Record Format
- `len` : u32 big-endian (payload length in bytes)
- `payload` : raw bytes
- `crc` : u32 IEEE CRC-32 over **payload** (big-endian stored)

## Recovery Algorithm
1. Iterate from offset 0:
   - Read 4B `len`. If EOF or `len` is implausible -> truncate at current offset.
   - Ensure `len + 4` more bytes exist (`payload + crc`). If not -> truncate.
   - Read `payload` and `crc`. Recompute and compare -> if mismatch -> truncate.
2. Truncate file to `last_good_offset`. Done.

This guarantees readers never see a partial/torn record.

---

## Security & Best Practices

- The `.gitignore` is configured to prevent accidental commits of binaries, WAL files, logs, and sensitive files (e.g., `.env`, keys).
- Never commit real data or secrets to the repository.
- Always test recovery on a copy of your WAL file.

## Troubleshooting

- If you see a build error about missing headers, ensure you are using a C++17-compatible compiler.
- For macOS, all dependencies are standard C++ and STL.
- If you encounter permission errors, check file and directory permissions.

## License

MIT License. See LICENSE file for details.

---

## Educational Content: Understanding WAL and Tail Truncation

### What is a Write-Ahead Log (WAL)?
A Write-Ahead Log (WAL) is a technique used in databases and filesystems to ensure data durability and consistency. Before making changes to the main data, all modifications are first written sequentially to a log file. This allows the system to recover to a consistent state after a crash by replaying or rolling back log entries.

**Key Properties:**
- Sequential writes (fast, reliable)
- Each record: `[length][payload][crc]`
- Used in databases (PostgreSQL, SQLite), filesystems, and distributed systems

### Why Does Tail Truncation Matter?
When a crash or power loss occurs, the last record in the WAL may be only partially written ("torn"). If the system does not detect and remove this torn tail, it can lead to data corruption or application errors.

**Example:**
```
| Record 1 | Record 2 | Record 3 | <crash> | partial/torn data |
```
The recovery process must scan the WAL, detect the last good record, and truncate any incomplete or corrupted tail.

### How Does This Project Help?
This tool demonstrates how to:
- Detect incomplete or corrupted records using length and CRC checks
- Safely truncate the WAL to the last valid record
- Simulate crashes and recovery for learning and testing

### Visual Diagram

```
WAL File:
| len | payload | crc | len | payload | crc | ... | len | payload | crc | <torn tail>
                                             ^
                                Truncate here after recovery
```

### Further Reading & Resources
- [Write-Ahead Logging (Wikipedia)](https://en.wikipedia.org/wiki/Write-ahead_logging)
- [PostgreSQL WAL Internals](https://www.postgresql.org/docs/current/wal-intro.html)
- [SQLite WAL Mode](https://sqlite.org/wal.html)
- [Crash Consistency in Storage Systems](https://queue.acm.org/detail.cfm?id=1814327)

---
