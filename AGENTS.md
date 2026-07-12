# AGENTS.md

## 项目概览

DirectTransfer 是 C++17 Win32 GUI 的 Windows 点对点目录传输工具。当前主线是一个单独的 `DirectTransfer.exe`：发送端扫描源目录并连接接收端；接收端创建会话令牌、监听发现和 TCP 连接，再安全地写入目标目录。

- 界面文本为简体中文宽字符串；视觉风格由 `ConsoleTheme.h` 定义。
- 网络协议为自定义 v5，不使用 SMB、网络共享、Robocopy、IPC$ 或 Windows 凭据。
- 默认端口为 UDP/TCP `49321`，协议版本和程序版本定义在 `src/Version.h`。
- 构建系统为 CMake + Ninja；目标为 MinGW-w64 C++17，并使用 `-static` 链接。
- `external/nlohmann/json.hpp` 是随仓库提供的头文件依赖；当前 v5 数据通道不以 JSON 清单作为主协议。
- 没有自动化测试或 CI；修改后至少应完成可用的 CMake 构建验证。

## 构建与验证

```powershell
cmake -G Ninja -DCMAKE_CXX_COMPILER=g++ -B build
cmake --build build
```

需要实机验证网络功能时，使用两台可互通的 Windows 主机。允许 UDP/TCP 49321；不要让程序自动创建防火墙规则。静态 IP / DHCP 恢复是可选管理员功能，不能成为常规传输的前置条件。

## 架构与职责

```text
main.cpp
└── MainWindow
    ├── RoleSelectPage
    ├── SenderPage ── NetworkDiscovery ── TransferSession (sender)
    └── ReceiverPage ─ NetworkDiscovery ── TransferSession (receiver)
                                           ├── FileScanner / ExcludeRules
                                           ├── ProgressTracker
                                           └── AuditLogger / ReportGenerator
```

- `MainWindow` 接收 `WM_DISCOVERY_*`、`WM_TRANSFER_*` 消息并更新仪表板、日志和状态栏。工作线程不得直接操作控件。
- `NetworkDiscovery` 使用 UDP 发现接收端，手工 IP 也会通过定向 UDP 请求取得会话令牌。
- `TransferSession` 是 v5 传输状态机：握手、目录、批次或流式文件传输、提交确认与发送端重连均在此实现。
- `FileScanner` 递归扫描，跳过重解析点，并由 `ExcludeRules` 排除系统目录和分页文件。
- `IpConfigurator` 通过 WMI 设置静态 IPv4 或 DHCP；仅在用户明确操作且进程已提权时使用。
- `Manifest`、`TransferPlanner`、`ResumeManager` 是旧的 JSON 计划/续传辅助模块，不要把它们误写成 v5 主传输路径的必经步骤。当前发送页的模式下拉框没有传给 `TransferSession`；接收页传入的模式在 v5 主路径中仅用于镜像清理。

## v5 协议与传输约束

- TCP 包格式为 `1 字节类型 + 4 字节小端负载长度 + 负载`；单包负载最大 64 MiB。
- 握手顺序：`SERVER_HELLO` → `CLIENT_HELLO` → `HELLO_ACK`。双方必须验证 `DirectTransfer` 标识、协议版本和会话令牌。
- 发送端发送 `SESSION_BEGIN` 和二进制目录块；接收端以目录 CRC32 校验并返回 `SESSION_READY`。
- 小文件阈值为 64 KiB；每批最多 512 个文件或 8 MiB，窗口上限为 4 个待确认批次。
- 大文件以 1 MiB `FILE_DATA_V5` 分块传输，结束包和小文件记录均使用 CRC32。
- 接收端始终写入 `<目标>.dtpart`，成功后用 `MoveFileExW` 替换正式文件。当前 v5 会传输全部源文件；接收端仅在镜像模式下于会话成功提交后删除目标端多余文件。
- 文件 ID 是重连期间已提交状态的主键。只跳过已提交文件；不要宣称支持未提交文件的字节级续传。
- 握手 socket 超时为 30 秒；传输 I/O 等待为 5 秒，90 秒无进度判为断连。发送端重连退避为 1、2、4、8、15 秒。

## 实现规则

- 任何同时包含 Winsock 和 Win32 的文件，必须先包含 `winsock2.h`，再包含 `windows.h`。
- 所有 UTF-8 与 UTF-16 转换使用 `utils::ToUtf8()` / `utils::FromUtf8()`；日志和报告以 UTF-8 写入。
- 本地路径 I/O 使用 `utils::NormalizePath()`；绝对本地路径必须支持 `\\?\`，UNC 路径必须支持 `\\?\UNC\`。
- TCP 不是消息边界：保留 `SendAll()`、`RecvExact()` 的完整循环语义，不能以一次 `send()` 或 `recv()` 代替。
- 停止会话时，`TransferSession::Stop()` 必须在 socket 锁内关闭活动 socket，释放锁后再 `join()` 工作线程，避免死锁。
- 接收远端目录条目时必须维持 `IsSafeRelativePath()` 的校验，禁止绝对路径、盘符路径、空段和 `.` / `..` 段。
- 扫描出的源文件在读前和读后均要用大小及最后修改时间验证；不要在未重新评估完整性模型的情况下移除该检查。
- 小文件写入线程数由目标卷的 seek-penalty 查询决定：HDD 为 1，SSD 为 4，未知或网络路径为 2。不要在 UI 线程执行磁盘写入。
- 保持 DPI 适配：页面在 `Relayout()` 中通过 `MulDiv` 计算尺寸，`MainWindow` 在 `WM_DPICHANGED` 时刷新字体和布局。
- 中文 UI 文本使用宽字符串，延续周围代码的 Unicode 转义风格。新增控件必须同时处理初始布局和 `Relayout()`。

## 变更边界

- 不要引入 SMB、共享目录、注册表改写、启动时防火墙 COM 自动化或子网定向枚举。
- 不要把管理员 IP 配置改为强制步骤；自动发现与手动 IP 都应保留。
- 更新协议字段、包类型、超时、文件阈值或镜像删除时，必须同步审查发送端、接收端、UI 文案和 README。
- 不要在文档或 UI 中把“安全复制”描述成已在 v5 会话中按哈希跳过，除非同时完成该计划逻辑的协议接入。
- 修改构建文件时保留 `resources.rc` 和 VERSIONINFO；当前资源文件是可执行文件元数据的一部分。
