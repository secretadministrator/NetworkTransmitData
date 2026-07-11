# AGENTS.md

## Project

**DirectTransfer** - C++17 Win32 GUI point-to-point file transfer tool.
It uses Winsock TCP with a custom binary v5 protocol, not SMB, Robocopy, net shares, or Windows credentials.

- **Language**: C++17, Win32 API, Simplified Chinese UI strings in source
- **UI style**: Dark terminal-inspired theme (`ConsoleTheme.h`), owner-draw controls, DPI-aware
- **Build**: CMake + Ninja, MinGW-W64 g++ 16.1, CMake 4.3.2, Ninja 1.13.2
- **Dependency**: nlohmann/json v3.11.3, header-only, downloaded by CMake when absent
- **Tests/CI**: none

## Build and run

```powershell
cmake -G Ninja -DCMAKE_CXX_COMPILER=g++ -B build
cmake --build build
.\build\DirectTransfer.exe
```

## Architecture

Single EXE with two startup roles and a dark console-style dashboard:

```text
DirectTransfer.exe
|-- Sender   - scan source dir, discover/manual-select receiver,
|              verify protocol version, send files over TCP with v5 binary protocol
`-- Receiver - choose target dir, auto-generate session token, receive files
               to .dtpart temp files, then rename on completion
```

## Key decisions

- **No SMB / net share / IPC$ / Robocopy**: custom Winsock TCP on port `49321`.
- **No registry modifications**: no LSA policy changes.
- **No auto firewall rule**: users handle firewall/admin setup when needed.
- **IP config is optional**: APIPA/manual IP must work without admin. Admin static IP setup is only a helper for `192.168.88.x`.
- **Authentication**: session token exchanged via UDP discovery, not 6-digit pairing code.
- **Protocol version v5**: SERVER_HELLO/CLIENT_HELLO/HELLO_ACK handshake verifies protocol compatibility.
- **Binary catalog**: sender encodes file entries in binary format with CRC32 checksum; receiver builds commit plan per-file.
- **Small file batch windowing**: up to 512 files per batch, up to 4 batches in-flight concurrently; sender waits for BATCH_ACK per batch.
- **Large file streaming**: individual FILE_BEGIN_V5 / FILE_DATA_V5 / FILE_END_V5 / FILE_ACK_V5 flow with CRC32.
- **Automatic reconnect**: sender retries with exponential backoff (1s → 15s max) on connection loss; already-committed files are not retransmitted (tracked by file ID).
- **Integrity**: CRC32 per file chunk during transfer; source file stability verified before and after read (`SourceFileMatches`).
- **`.dtpart` temp files**: receiver writes to `.dtpart`, renames to final name on completion. Small files are written concurrently by a thread pool tuned to disk type (1 thread for HDD, 4 for SSD).
- **Transfer timeout**: handshake uses 30s socket timeout; file transfer uses 5s I/O wait with 90s idle timeout before reconnect.
- **Progress**: byte-level tracking via `ProgressTracker`, speed averaged over 10 samples, displayed via `ConsoleDashboard` custom owner-draw control.
- **Mirror mode**: UI label is `L"\u540c\u6b65\u955c\u50cf"` ("sync mirror"); target-only files are deleted before receiving.
- **AV false-positive mitigation**: keep VERSIONINFO resource; avoid startup firewall COM automation and subnet-directed broadcast enumeration.

## Source layout

```text
src/
|-- main.cpp                  WinMain, COM init, MainWindow, DHCP restore on exit
|-- Version.h                 version constants (1.1.0, protocol v5)
|-- AppConfig.h/.cpp          defaults and CLI overrides
|-- Utils.h/.cpp              UTF-8 helpers, timestamps, paths, SHA-256 (BCrypt)
|-- AuditLogger.h/.cpp        UTF-8 write-only log file
|-- MainWindow.h/.cpp         Win32 frame (820×720), page switching, dark theme,
|                              owner-draw UI, ConsoleDashboard + ConsoleLogView
|-- IPage.h                   page interface with Relayout(dpi)
|-- RoleSelectPage.h/.cpp     role picker
|-- SenderPage.h/.cpp         sender 3-step setup UI
|-- ReceiverPage.h/.cpp       receiver 3-step setup UI
|-- DirPicker.h/.cpp          IFileDialog folder picker
|-- ConsoleTheme.h            colour palette and font helpers (dark terminal)
|-- ConsoleDashboard.h/.cpp   owner-draw status panel with progress bar,
|                              speed, ETA, file count
|-- ConsoleLogView.h/.cpp     owner-draw scrollable log view (256 lines)
|-- NetworkDiscovery.h/.cpp   UDP broadcast discover/listen (protocol v5)
|-- PairingHandler.h/.cpp     session token generation/verify (replaces pairing code)
|-- TransferProtocol.h/.cpp   binary packet format helpers
|-- TransferSession.h/.cpp    v5 sender/receiver worker thread, reconnect,
|                              batch windowing, CRC32 streaming
|-- FileScanner.h/.cpp        recursive scan and excludes
|-- Manifest.h/.cpp           JSON manifest (legacy, used by TransferPlanner)
|-- TransferPlanner.h/.cpp    manifest vs target comparison, mirror delete
|-- ResumeManager.h/.cpp      .dtpart offset detection (legacy, v5 uses file-ID tracking)
|-- ExcludeRules.h/.cpp       default excludes (System Volume Information, etc.)
|-- ProgressTracker.h/.cpp    speed and ETA calculation
|-- ReportGenerator.h/.cpp    UTF-8 transfer report
|-- IpConfigurator.h/.cpp     optional admin static IP/DHCP helper
|-- Resource.h                control IDs and custom WM_* messages
`-- resources.rc              VERSIONINFO + manifest reference
```

## Important implementation rules

- Include `winsock2.h` before `windows.h` in any file that needs both.
- Use `utils::ToUtf8()` and `utils::FromUtf8()` for all UTF-8/UTF-16 conversion.
- `TransferSession::SendAll()` and `RecvExact()` must loop until full I/O is complete. TCP `send()`/`recv()` may partially transfer.
- `TransferSession::Stop()` must close sockets under lock, release the lock, then `join()` the worker thread.
- File ID (`uint64_t`) is the primary key for tracking committed files across reconnects. Sender tracks `committed` set; receiver tracks `committed` vector.
- Source file stability is verified before and after reading via `SourceFileMatches()` (size + last-write-time comparison).
- Small file batches: each batch contains up to 512 files or 8 MB. Up to 4 batches may be in-flight. Sender waits for BATCH_ACK before sending next window.
- Large files: streamed with CRC32 final check in FILE_END_V5; receiver responds with FILE_ACK_V5 (1 = ok, 0 = fail).
- On connection loss, sender retries with exponential backoff (1s → 15s max). Already committed files are skipped via the `committed` set.
- Receiver uses `SmallFileWriterPool` for concurrent .dtpart writes. Thread count determined by `DeviceIoControl` seek-penalty query (1 for HDD, 4 for SSD, 2 for unknown/network).
- Logs and reports are UTF-8 narrow streams.
- Manual IP entry and direct receiving are first-class flows. Do not make admin IP configuration a required UI step.
- `NormalizePath()` prepends `\\?\` for absolute local paths and `\\?\UNC\` for UNC paths.
- Keep Chinese UI strings as wide literals/Unicode escapes consistent with surrounding code.
- Current CMake uses `target_link_options(DirectTransfer PRIVATE -static)` with the RC resource included.
- DPI scaling: all page controls implement `Relayout()` using `MulDiv`. `MainWindow` handles `WM_DPICHANGED`.
