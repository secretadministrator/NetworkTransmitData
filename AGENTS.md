# AGENTS.md

## Project

**DirectTransfer** - C++17 Win32 GUI point-to-point file transfer tool.
It uses Winsock TCP with a custom protocol, not SMB, Robocopy, net shares, or Windows credentials.

- **Language**: C++17, Win32 API, Simplified Chinese UI strings in source
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

Single EXE with two startup roles:

```text
DirectTransfer.exe
|-- Sender   - scan source dir, discover/manual-select receiver,
|              verify 6-digit pairing code, send files over TCP
`-- Receiver - choose target dir, display pairing code, receive files
               to .dtpart temp files, then rename on completion
```

## Key decisions

- **No SMB / net share / IPC$ / Robocopy**: custom Winsock TCP on port `49321`.
- **No registry modifications**: no LSA policy changes.
- **No auto firewall rule**: users handle firewall/admin setup when needed.
- **IP config is optional**: APIPA/manual IP must work without admin. Admin static IP setup is only a helper for `192.168.88.x`.
- **Authentication**: 6-digit pairing code, not Windows credentials.
- **Resume**: receiver uses `.dtpart`, clamps invalid offsets, includes the `.dtpart` prefix SHA-256 in the plan, and the sender resets offset to `0` if the prefix hash does not match the source prefix.
- **Manifest**: sender sends JSON file list with full-file SHA-256; receiver builds `TRANSFER` / `SKIP` / `OVERWRITE` plan. `SKIP` requires both size and SHA-256 to match.
- **Protocol**: packet format is `1 byte type + 4 byte little-endian payload length + payload`.
- **Transfer timeout**: pairing uses a finite socket timeout; after pairing succeeds, file transfer and verification must not use the old fixed 30-second socket timeout.
- **Progress**: byte-level tracking via `ProgressTracker`, speed averaged over 10 samples.
- **Mirror mode**: UI label is `L"\u540c\u6b65\u955c\u50cf"` ("sync mirror"); target-only files are deleted before receiving.
- **AV false-positive mitigation**: keep VERSIONINFO resource; avoid startup firewall COM automation and subnet-directed broadcast enumeration.

## Source layout

```text
src/
|-- main.cpp                  WinMain, COM init, MainWindow
|-- AppConfig.h/.cpp          defaults and CLI overrides
|-- Utils.h/.cpp              UTF-8 helpers, timestamps, paths, hashing
|-- AuditLogger.h/.cpp        UTF-8 write-only log file
|-- MainWindow.h/.cpp         Win32 frame, page switching, owner-draw UI
|-- RoleSelectPage.h/.cpp     role picker
|-- SenderPage.h/.cpp         sender setup UI
|-- ReceiverPage.h/.cpp       receiver setup UI
|-- DirPicker.h/.cpp          IFileDialog folder picker
|-- NetworkDiscovery.h/.cpp   UDP broadcast discover/listen
|-- PairingHandler.h/.cpp     pairing code generation/verify
|-- TransferProtocol.h/.cpp   binary packet helpers
|-- TransferSession.h/.cpp    sender/receiver worker thread and protocol state
|-- FileSender.h/.cpp         lower-level send-all helper
|-- FileReceiver.h/.cpp       lower-level receive helper
|-- FileScanner.h/.cpp        recursive scan and excludes
|-- Manifest.h/.cpp           JSON manifest
|-- TransferPlanner.h/.cpp    manifest vs target comparison
|-- ResumeManager.h/.cpp      .dtpart offset detection
|-- ExcludeRules.h/.cpp       default excludes
|-- ProgressTracker.h/.cpp    speed and ETA calculation
|-- ReportGenerator.h/.cpp    UTF-8 transfer report
|-- IpConfigurator.h/.cpp     optional admin static IP/DHCP helper
`-- Resource.h                control IDs and custom WM_* messages
```

## Important implementation rules

- Include `winsock2.h` before `windows.h` in any file that needs both.
- Use `utils::ToUtf8()` and `utils::FromUtf8()` for all UTF-8/UTF-16 conversion. Do not hand-roll `WideCharToMultiByte` or `MultiByteToWideChar` into a `resize(len - 1)` buffer.
- `TransferSession::SendPacket()` must loop until the full header and payload are sent. TCP `send()` may legally write only part of a buffer.
- `TransferSession::Stop()` must close sockets under lock, release the lock, then `join()` the worker thread. Do not hold `m_sockMutex` while joining.
- Sender open-file failures happen before `FILE_HEADER` and are sent as top-level `ERROR_MSG`. Sender read failures after `FILE_HEADER` send `ERROR_MSG` followed by `FILE_DONE`; receiver consumes that `FILE_DONE` and does not send an ACK for sender-aborted files.
- Receiver-side write/create failures should drain the current file payload, receive hash/done, then send `FILE_DONE_ACK=FAIL` so the stream stays aligned.
- Receiver verification hashes data incrementally while receiving/writing `.dtpart`; do not reintroduce a mandatory second full-file read after receiving.
- `FILE_DONE_ACK` payload is path-qualified (`OK\n<relativePath>` or `FAIL\n<relativePath>`). Sender must treat missing, mismatched, or failed ACK as a session-stopping error, not continue with later files.
- Sender must send exactly the planned remaining bytes (`size - offset`) for a file. Short reads, extra data, hash mismatch, or ACK mismatch are integrity failures.
- Logs and reports are UTF-8 narrow streams; do not reintroduce `std::wofstream` for these files.
- Manual IP entry and direct receiving are first-class flows. Do not make admin IP configuration a required UI step.
- `NormalizePath()` prepends `\\?\` for absolute local paths and `\\?\UNC\` for UNC paths.
- Keep Chinese UI strings as wide literals/Unicode escapes consistent with surrounding code.
- Current CMake uses `target_link_options(DirectTransfer PRIVATE -static)` with the RC resource included.
