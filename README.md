# DirectTransfer

Windows 双机直连点对点文件传输工具。Win32 API 自绘 GUI，Winsock 自定义 TCP 协议，无需 SMB/Robocopy 依赖。

## 使用流程

1. 两台计算机用直连网线连接（或同一局域网）
2. 以管理员身份运行 `DirectTransfer.exe`
3. **角色选择** — 源机选「发送」，目标机选「接收」
4. **步骤引导** — 按 UI 提示逐步完成：
   - 选择源/目标目录
   - 配置 IP 地址（可选）
   - 发送端自动发现对端，交换 6 位配对码
   - 选择传输模式（安全复制 / 覆盖 / 镜像同步）
5. 开始传输，进度条 + 实时日志跟踪
6. 传输完成自动生成报告

## 构建

```powershell
cmake -G Ninja -DCMAKE_CXX_COMPILER=g++ -B build
cmake --build build
.\build\DirectTransfer.exe
```

### 依赖

- **MinGW-W64 g++ 16.1** + **Ninja 1.13.2** + **CMake 4.3.2**
- **nlohmann/json v3.11.3** — CMake 自动下载到 `external/nlohmann/json.hpp`
- 仅需系统 DLL，无额外运行时

### CMake 参数

| 参数 | 值 |
|---|---|
| C++ 标准 | C++17 |
| 目标平台 | Windows 7+ (`_WIN32_WINNT=0x0601`) |
| 链接 | `-static-libstdc++ -static-libgcc` |
| 编码 | UTF-8 源文件，UTF-8 运行时编码 |

## 网络协议

- **发现**: UDP 广播端口 49321，自动发现局域网对端
- **传输**: 单 TCP 连接端口 49321
  - 自定义二进制包格式: `1 字节类型 + 4 字节小端长度 + 载荷`
  - 配对码 → 文件清单 (JSON) → 传输计划 → 逐文件发送
- **断点续传**: `.dtpart` 临时文件，检查已有大小确定偏移
- **速度控制**: `SO_RCVTIMEO`/`SO_SNDTIMEO` 30s 超时；滑动窗口速度采样

## 安全说明

- **不修改注册表、不创建共享、不调用 SMB**
- 6 位配对码验证，不依赖 Windows 凭据
- 不自动添加防火墙规则；如需放行 49321 端口，手动运行一次管理员模式
- 可选 IP 配置：默认 APIPA (169.254.x.x)；管理员模式可设静态 IP 192.168.88.x
- 退出时自动恢复 DHCP（如有配置过）
