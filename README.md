# DirectTransfer v1.1

Windows 双机直连点对点文件传输工具。C++17、Win32 GUI、暗色终端主题。自定义 Winsock TCP v5 二进制协议，不依赖 SMB、Robocopy、网络共享或 Windows 凭据。

## 使用流程

1. 两台计算机用直连网线或同一局域网连接。
2. **接收端** — 选择目标目录，工具自动生成会话标识，通过 UDP 广播等待发现。
3. **发送端** — 选择源目录，自动发现接收端或手动输入对端 IP，开始传输。
4. **传输流程**：二进制文件目录（含 CRC32）→ 小文件批量窗口 / 大文件逐流传输 → 接收端逐文件 CRC32 校验 → 提交确认。
5. **断线重连**：发送端自动指数退避重连，已提交文件不重复传输。
6. 暗色仪表板实时显示阶段、进度条、速度、ETA、文件计数。

支持三种模式：**安全复制**（同名同大小同文件跳过）、**覆盖复制**（强制覆盖）、**镜像同步**（删除目标端多余文件）。

## 网络协议 v5

- **UDP 端口 49321**：局域网自动发现（携带协议版本和会话标识）。
- **TCP 端口 49321**：握手 → 二进制目录 → 小文件批流 / 大文件流 → 提交确认。
- **包格式**：`1 字节类型 + 4 字节小端载荷长度 + 载荷`。
- **协议版本**：`5`（主版本不兼容则拒绝连接）。
- **集成校验**：读取前后比对源文件大小和修改时间；全传输流使用 CRC32。

### 协议阶段

| 阶段 | 说明 |
|---|---|
| 握手 | SERVER_HELLO → CLIENT_HELLO → HELLO_ACK，交换版本和会话标识 |
| 目录 | SESSION_BEGIN → 二进制目录块 CATALOG_CHUNK → CATALOG_DONE → SESSION_READY |
| 小文件 | BATCH_DATA（≤512 个/批，≤8 MB/批，最多 4 批在途）→ BATCH_ACK |
| 大文件 | FILE_BEGIN_V5 → FILE_DATA_V5（1 MB 块）→ FILE_END_V5（含 CRC32）→ FILE_ACK_V5 |
| 提交 | SESSION_COMMIT → SESSION_COMMIT_ACK |

## 构建

```powershell
cmake -G Ninja -DCMAKE_CXX_COMPILER=g++ -B build
cmake --build build
.\build\DirectTransfer.exe
```

依赖：MinGW-W64 g++ 16.1、CMake 4.3.2、Ninja 1.13.2、nlohmann/json v3.11.3。

## 界面特性

- 暗色终端风格（#070C10 背景，#3DFF9D 强调色）
- DPI 感知（PerMonitorV2），窗口最小 680×620
- 实时仪表板：阶段状态、进度条、速度、ETA、总耗时、文件计数
- 滚动日志面板（保留 256 条）
- 断线重连按钮（发送端连接丢失时显示）

## 系统行为

- 不使用 SMB、不修改注册表、不自动添加防火墙规则。
- 接收文件先写入 `.dtpart`，校验成功后原子替换。
- 小文件使用并发写线程池，按目标磁盘类型（HDD/SSD/网络）自适应线程数。
- 日志和报告使用 UTF-8。
- 退出时自动恢复被临时改动的网卡 DHCP 设置。

## 旧版脚本

`NetworkTransmitDataScript_V4.5_zhcn.bat` 是基于 Robocopy + SMB 共享的历史批处理版本，保留在仓库根目录作为参照。
