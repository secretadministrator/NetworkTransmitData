@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM ==========================================================
REM 两台 Windows 主机直连 Robocopy 数据传输脚本 - 兼容 Win7/Win10
REM 请在两台主机上均以“管理员身份运行”本脚本。
REM ==========================================================

set "SOURCE_IP=192.168.1.1"
set "TARGET_IP=192.168.1.2"
set "SUBNET_MASK=255.255.255.0"
set "MIN_FREE_GB=1"
set "THREAD_COUNT=16"

set "SHARE_D=SRC_D$"
set "SHARE_E=SRC_E$"
set "BACKUP_DIR=%~dp0backup"
set "LOG_DIR=%~dp0logs"
set "LOCK_FILE=%~dp0transfer.lock"
set "AUDIT_LOG=%~dp0transfer_audit.log"

for /f "tokens=1-3 delims=." %%a in ("%TARGET_IP%") do set "SUBNET_PREFIX=%%a.%%b.%%c"

if not exist "%BACKUP_DIR%" mkdir "%BACKUP_DIR%" 2>nul
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%" 2>nul

if exist "%LOCK_FILE%" (
    echo [信息] 检测到锁文件，说明上一次操作可能尚未完成恢复。
    set /p "_cont=输入 Y 立即进入恢复模式，或直接回车继续："
    if /i "!_cont!"=="Y" (
        call :require_admin
        if errorlevel 1 exit /b 1
        call :detect_nic
        if errorlevel 1 exit /b 1
        goto restore
    )
)

call :require_admin
if errorlevel 1 exit /b 1

call :detect_nic
if errorlevel 1 exit /b 1

:menu
echo.
echo ==========================================================
echo Robocopy 双机直连传输 - 兼容 Win7/Win10
echo ==========================================================
echo 源主机 IP：%SOURCE_IP%
echo 目标主机 IP：%TARGET_IP%
echo 当前选择的网卡：%NIC_NAME%
echo.
echo 1 - 配置源主机
echo 2 - 配置目标主机
echo 3 - 在目标主机上执行 Robocopy 传输
echo 4 - 恢复网络、防火墙、注册表和共享设置
echo 0 - 退出
echo ==========================================================
set /p "pass=请选择操作模式："
if /i "%pass%"=="1" goto source
if /i "%pass%"=="2" goto target
if /i "%pass%"=="3" goto robocopy
if /i "%pass%"=="4" goto restore
if /i "%pass%"=="0" exit /b 0
echo [错误] 无效的选择。
pause
goto menu

:source
echo.
echo ---------------- 源主机配置模式 ----------------
call :save_ip_config
call :save_registry
call :configure_registry
call :open_firewall_rules
call :set_static_ip "%SOURCE_IP%"
if errorlevel 1 goto fatal_exit
call :create_source_shares
echo %DATE% %TIME% > "%LOCK_FILE%"
call :write_audit "源主机配置完成。IP=%SOURCE_IP% 网卡=%NIC_NAME%"
echo.
echo [完成] 源主机配置完成。
echo [完成] 已创建共享：\\%SOURCE_IP%\%SHARE_D% 和 \\%SOURCE_IP%\%SHARE_E%
echo 请现在到目标主机上运行模式 2。
pause
exit /b 0

:target
echo.
echo ---------------- 目标主机配置模式 ----------------
call :save_ip_config
call :open_firewall_rules
call :set_static_ip "%TARGET_IP%"
if errorlevel 1 goto fatal_exit
call :wait_seconds 3
call :ping_host "%SOURCE_IP%"
if errorlevel 1 (
    echo [警告] 当前无法 ping 通源主机，请检查网线连接以及源主机是否已运行模式 1。
) else (
    echo [完成] 已成功连通源主机。
)
echo %DATE% %TIME% > "%LOCK_FILE%"
call :write_audit "目标主机配置完成。IP=%TARGET_IP% 网卡=%NIC_NAME%"
echo.
echo [完成] 目标主机配置完成。
echo 请继续在本目标主机上运行模式 3 开始传输。
pause
exit /b 0

:robocopy
echo.
echo ---------------- Robocopy 文件传输模式 ----------------
call :verify_target_ip
if errorlevel 1 (
    echo [警告] 当前主机似乎没有使用目标 IP：%TARGET_IP%。
    set /p "ANS=是否仍要继续？输入 Y 继续："
    if /i not "!ANS!"=="Y" goto copy_cancel
)

call :ping_host "%SOURCE_IP%"
if errorlevel 1 (
    echo [错误] 无法连接到源主机，传输已中止。
    pause
    exit /b 1
)

call :connect_source
if errorlevel 1 exit /b 1

echo.
echo 源主机上当前可见的共享资源：
net view \\%SOURCE_IP%

echo.
echo 1 - 镜像源主机 D 盘到本地 D 盘 - 目标端独有文件将被删除
echo 2 - 镜像源主机 E 盘到本地 E 盘 - 目标端独有文件将被删除
echo 3 - 复制源主机 D 盘到本地 D 盘 - 增量复制，不删除目标端文件
echo 4 - 复制源主机 E 盘到本地 E 盘 - 增量复制，不删除目标端文件
echo 5 - 自定义路径传输
echo 0 - 取消
set /p "mode=请选择传输模式："

if /i "%mode%"=="1" goto copy_d_mir
if /i "%mode%"=="2" goto copy_e_mir
if /i "%mode%"=="3" goto copy_d_inc
if /i "%mode%"=="4" goto copy_e_inc
if /i "%mode%"=="5" goto copy_custom
if /i "%mode%"=="0" goto copy_cancel
echo [错误] 无效的选择。
goto copy_cleanup

:copy_d_mir
call :run_fixed_transfer "\\%SOURCE_IP%\%SHARE_D%" "D:\" "MIR" "d_mir"
goto copy_cleanup

:copy_e_mir
call :run_fixed_transfer "\\%SOURCE_IP%\%SHARE_E%" "E:\" "MIR" "e_mir"
goto copy_cleanup

:copy_d_inc
call :run_fixed_transfer "\\%SOURCE_IP%\%SHARE_D%" "D:\" "INC" "d_inc"
goto copy_cleanup

:copy_e_inc
call :run_fixed_transfer "\\%SOURCE_IP%\%SHARE_E%" "E:\" "INC" "e_inc"
goto copy_cleanup

:copy_custom
call :run_custom_transfer
goto copy_cleanup

:copy_cancel
echo [信息] 传输已取消。
goto copy_cleanup

:copy_cleanup
net use \\%SOURCE_IP%\IPC$ /delete /y >nul 2>&1
echo [信息] 请在两台主机上都运行模式 4，用于恢复 IP、防火墙规则、注册表和共享设置。
pause
exit /b 0

:restore
echo.
echo ---------------- 恢复系统设置 ----------------
call :restore_ip_config
call :remove_firewall_rules
call :restore_registry
call :remove_source_shares
net use \\%SOURCE_IP%\IPC$ /delete /y >nul 2>&1
del "%LOCK_FILE%" 2>nul
call :write_audit "恢复操作完成"
echo [完成] 恢复流程已结束。如需人工核对，请查看 backup 备份目录。
pause
exit /b 0

:fatal_exit
echo [错误] 操作失败。你可以运行模式 4 恢复之前的设置。
pause
exit /b 1

REM ==========================================================
REM 子程序区域
REM ==========================================================

:require_admin
net session >nul 2>&1
if errorlevel 1 (
    echo [错误] 请以管理员身份运行此脚本。
    pause
    exit /b 1
)
exit /b 0

:detect_nic
set "NIC_NAME="
echo 正在检测已连接的物理有线网卡...

REM 优先使用 WMIC 检测网卡，因为它同时兼容 Windows 7 和 Windows 10。
for /f "tokens=2 delims==" %%A in ('wmic nic where "NetConnectionStatus=2" get NetConnectionID /value 2^>nul ^| find "="') do (
    set "_CAND=%%A"
    echo !_CAND! | findstr /i "wireless wi-fi wifi wlan bluetooth virtual vpn loopback" >nul
    if errorlevel 1 if not defined NIC_NAME set "NIC_NAME=!_CAND!"
)

if not defined NIC_NAME (
    echo [警告] 自动网卡检测失败。
    echo 当前可用的 IPv4 网络接口列表：
    netsh interface ipv4 show interface
    echo.
    set /p "NIC_NAME=请输入要使用的网卡完整名称："
)

if not defined NIC_NAME (
    echo [错误] 未选择任何网络适配器。
    pause
    exit /b 1
)

echo [完成] 已选择网卡：%NIC_NAME%
exit /b 0

:save_ip_config
set "IP_BAK=%BACKUP_DIR%\ipv4_config_dump.txt"
if exist "%IP_BAK%" exit /b 0
netsh interface ipv4 dump > "%IP_BAK%" 2>nul
if errorlevel 1 (
    echo [警告] 保存 IPv4 配置失败。
) else (
    echo [完成] IPv4 配置已备份。
)
exit /b 0

:restore_ip_config
set "IP_BAK=%BACKUP_DIR%\ipv4_config_dump.txt"
if not exist "%IP_BAK%" (
    echo [警告] 未找到 IPv4 备份文件，IP 配置未恢复。
    exit /b 0
)
set "IP_RESTORE_LOG=%BACKUP_DIR%\ipv4_restore_result.log"
netsh -f "%IP_BAK%" >"%IP_RESTORE_LOG%" 2>&1
if errorlevel 1 (
    netsh exec "%IP_BAK%" >>"%IP_RESTORE_LOG%" 2>&1
)
if errorlevel 1 (
    ipconfig | findstr /c:"%SOURCE_IP%" /c:"%TARGET_IP%" >nul
    if errorlevel 1 (
        echo [完成] IPv4 配置已恢复。netsh 返回了非零代码，详细信息：%IP_RESTORE_LOG%
        exit /b 0
    )
    echo [警告] 备份脚本直接恢复失败，正在尝试将当前网卡恢复为 DHCP 自动获取。
    if not defined NIC_NAME (
        call :detect_nic
        if errorlevel 1 (
            echo [警告] 无法确定网卡名称，IPv4 配置未恢复。详细信息：%IP_RESTORE_LOG%
            exit /b 0
        )
    )
    netsh interface ipv4 set address name="%NIC_NAME%" source=dhcp >>"%IP_RESTORE_LOG%" 2>&1
    if errorlevel 1 (
        echo [警告] DHCP 地址恢复失败，请手动检查 IPv4 设置。详细信息：%IP_RESTORE_LOG%
        exit /b 0
    )
    netsh interface ipv4 set dnsservers name="%NIC_NAME%" source=dhcp >>"%IP_RESTORE_LOG%" 2>&1
    echo [完成] IPv4 配置已通过 DHCP 回退方式恢复。
    exit /b 0
)
echo [完成] IPv4 配置已恢复。
exit /b 0

:set_static_ip
set "NEW_IP=%~1"
echo 正在将网卡 %NIC_NAME% 设置为静态 IP：%NEW_IP% ...
netsh interface ipv4 set address name="%NIC_NAME%" static %NEW_IP% %SUBNET_MASK% none >nul 2>&1
if errorlevel 1 (
    echo [错误] 设置静态 IP 失败，请检查网卡名称是否正确。
    exit /b 1
)
call :wait_seconds 2
ipconfig | findstr /c:"%NEW_IP%" >nul
if errorlevel 1 (
    echo [警告] IP 地址设置命令已执行，但暂未在检测结果中发现该 IP。
) else (
    echo [完成] IP 地址验证通过。
)
exit /b 0

:open_firewall_rules
REM 仅添加 SMB 文件共享与 ping 所需的临时入站规则，不直接关闭整个防火墙。
netsh advfirewall firewall delete rule name="DirectTransfer SMB TCP 445" >nul 2>&1
netsh advfirewall firewall delete rule name="DirectTransfer SMB TCP 139" >nul 2>&1
netsh advfirewall firewall delete rule name="DirectTransfer NB UDP 137" >nul 2>&1
netsh advfirewall firewall delete rule name="DirectTransfer NB UDP 138" >nul 2>&1
netsh advfirewall firewall delete rule name="DirectTransfer ICMPv4" >nul 2>&1
netsh advfirewall firewall add rule name="DirectTransfer SMB TCP 445" dir=in action=allow protocol=TCP localport=445 >nul 2>&1
netsh advfirewall firewall add rule name="DirectTransfer SMB TCP 139" dir=in action=allow protocol=TCP localport=139 >nul 2>&1
netsh advfirewall firewall add rule name="DirectTransfer NB UDP 137" dir=in action=allow protocol=UDP localport=137 >nul 2>&1
netsh advfirewall firewall add rule name="DirectTransfer NB UDP 138" dir=in action=allow protocol=UDP localport=138 >nul 2>&1
netsh advfirewall firewall add rule name="DirectTransfer ICMPv4" dir=in action=allow protocol=icmpv4:8,any >nul 2>&1
echo [完成] 已添加临时防火墙入站规则。
exit /b 0

:remove_firewall_rules
netsh advfirewall firewall delete rule name="DirectTransfer SMB TCP 445" >nul 2>&1
netsh advfirewall firewall delete rule name="DirectTransfer SMB TCP 139" >nul 2>&1
netsh advfirewall firewall delete rule name="DirectTransfer NB UDP 137" >nul 2>&1
netsh advfirewall firewall delete rule name="DirectTransfer NB UDP 138" >nul 2>&1
netsh advfirewall firewall delete rule name="DirectTransfer ICMPv4" >nul 2>&1
echo [完成] 已移除临时防火墙入站规则。
exit /b 0

:save_registry
set "REG_BAK=%BACKUP_DIR%\registry_backup.dat"
if exist "%REG_BAK%" exit /b 0
set "LBP="
set "FG="
for /f "tokens=3" %%A in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\Lsa" /v LimitBlankPasswordUse 2^>nul ^| findstr /i "REG_DWORD"') do set "LBP=%%A"
for /f "tokens=3" %%A in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\Lsa" /v forceguest 2^>nul ^| findstr /i "REG_DWORD"') do set "FG=%%A"
if not defined LBP set "LBP=0x1"
if not defined FG set "FG=0x0"
(
    echo LimitBlankPasswordUse=%LBP%
    echo forceguest=%FG%
) > "%REG_BAK%"
echo [完成] 注册表相关配置已备份。
exit /b 0

:configure_registry
REM 不允许空密码远程访问；仅关闭强制 Guest 映射，以便使用认证账户进行文件共享。
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Lsa" /v LimitBlankPasswordUse /t REG_DWORD /d 1 /f >nul 2>&1
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Lsa" /v forceguest /t REG_DWORD /d 0 /f >nul 2>&1
echo [完成] 文件共享访问策略已配置。
exit /b 0

:restore_registry
set "REG_BAK=%BACKUP_DIR%\registry_backup.dat"
if not exist "%REG_BAK%" (
    echo [信息] 未找到注册表备份文件，本机可能未执行过注册表修改，跳过恢复。
    exit /b 0
)
for /f "usebackq tokens=1,2 delims==" %%A in ("%REG_BAK%") do (
    if /i "%%A"=="LimitBlankPasswordUse" reg add "HKLM\SYSTEM\CurrentControlSet\Control\Lsa" /v LimitBlankPasswordUse /t REG_DWORD /d %%B /f >nul 2>&1
    if /i "%%A"=="forceguest" reg add "HKLM\SYSTEM\CurrentControlSet\Control\Lsa" /v forceguest /t REG_DWORD /d %%B /f >nul 2>&1
)
echo [完成] 注册表相关配置已恢复。
exit /b 0

:create_source_shares
call :create_one_share D "%SHARE_D%"
call :create_one_share E "%SHARE_E%"
exit /b 0

:create_one_share
set "DRV=%~1"
set "SHR=%~2"
if not exist "%DRV%:\" (
    echo [信息] 驱动器 %DRV% 不存在，跳过共享创建。
    exit /b 0
)
net share %SHR% /delete /y >nul 2>&1
net share %SHR%=%DRV%:\ /GRANT:Everyone,READ >nul 2>&1
if errorlevel 1 (
    REM 针对本地化系统的回退处理：当 net share 无法识别 Everyone 时，尝试不指定授权参数创建共享。
    net share %SHR%=%DRV%:\ >nul 2>&1
)
if errorlevel 1 (
    echo [警告] 为驱动器 %DRV% 创建共享 %SHR% 失败。
) else (
    echo [完成] 已创建共享：%SHR% 指向 %DRV%:\
)
exit /b 0

:remove_source_shares
net share %SHARE_D% /delete /y >nul 2>&1
net share %SHARE_E% /delete /y >nul 2>&1
echo [完成] 已移除临时源端共享。
exit /b 0

:verify_target_ip
set "IP_CHECK=0"
for /f "tokens=2 delims=:" %%A in ('ipconfig ^| findstr /c:"%SUBNET_PREFIX%"') do (
    echo %%A | findstr /c:"%TARGET_IP%" >nul
    if not errorlevel 1 set "IP_CHECK=1"
)
if "%IP_CHECK%"=="1" exit /b 0
exit /b 1

:ping_host
set "PING_IP=%~1"
ping -n 2 %PING_IP% | findstr /i "TTL" >nul
if errorlevel 1 exit /b 1
exit /b 0

:connect_source
echo.
echo 请输入源主机账户凭据。账户必须设置非空密码。
set /p "SRC_USER=源主机用户名，默认 Administrator："
if not defined SRC_USER set "SRC_USER=Administrator"
echo %SRC_USER% | findstr /c:"\" >nul
if errorlevel 1 (
    set "NET_USER=%SOURCE_IP%\%SRC_USER%"
) else (
    set "NET_USER=%SRC_USER%"
)
net use \\%SOURCE_IP%\IPC$ /delete /y >nul 2>&1
net use \\%SOURCE_IP%\IPC$ /user:%NET_USER% *
if errorlevel 1 (
    echo [错误] 连接源主机 IPC$ 共享失败。
    echo 请检查用户名、密码、防火墙、源端共享以及文件共享服务是否正常。
    pause
    exit /b 1
)
echo [完成] 已建立源主机凭据会话。
exit /b 0

:run_fixed_transfer
set "SRC=%~1"
set "DST=%~2"
if "%SRC:~-1%"=="\" set "SRC=%SRC%."
if "%DST:~-1%"=="\" set "DST=%DST%."
set "COPYMODE=%~3"
set "LOGPREFIX=%~4"
call :check_target_drive "%DST%"
if errorlevel 1 exit /b 1
call :gen_logname "%LOGPREFIX%"
if /i "%COPYMODE%"=="MIR" (
    echo [危险] MIR 镜像模式会删除目标端中存在但源端不存在的文件。
    set /p "CONFIRM=请输入 CONFIRM 以继续执行："
    if not "!CONFIRM!"=="CONFIRM" exit /b 1
    robocopy "%SRC%" "%DST%" /MIR /R:3 /W:5 /A-:SH /MT:%THREAD_COUNT% /LOG:"%LOG_FILE%" /TEE /NP /V /XJ /XD $RECYCLE.BIN "System Volume Information" "wechat_files" "xwechat_files" "WeChat Files"  /XF "pagefile.sys" "hiberfil.sys" "swapfile.sys"
) else (
    robocopy "%SRC%" "%DST%" /E /R:3 /W:5 /A-:SH /MT:%THREAD_COUNT% /LOG:"%LOG_FILE%" /TEE /NP /V /XJ /XD $RECYCLE.BIN "System Volume Information" "wechat_files" "xwechat_files" "WeChat Files"  /XF "pagefile.sys" "hiberfil.sys" "swapfile.sys"
)
call :handle_robo_result "%LOG_FILE%"
exit /b %ROBOCOPY_RC%

:run_custom_transfer
echo.
set /p "source_path=请输入源路径，例如 \\%SOURCE_IP%\%SHARE_D%\Folder："
set /p "target_path=请输入目标路径，例如 D:\Folder："
if not defined source_path (
    echo [错误] 源路径不能为空。
    exit /b 1
)
if not defined target_path (
    echo [错误] 目标路径不能为空。
    exit /b 1
)
if "!source_path:~-1!"=="\" set "source_path=!source_path!."
if "!target_path:~-1!"=="\" set "target_path=!target_path!."
echo(!target_path! | findstr /r "^[A-Za-z]:\\" >nul
if errorlevel 1 (
    echo [错误] 目标路径必须以本地盘符开头，例如 D:\Folder。
    exit /b 1
)
if not exist "%target_path%" mkdir "%target_path%" 2>nul
call :check_target_drive "%target_path%"
if errorlevel 1 exit /b 1
call :gen_logname "custom"
set /p "MIRROR=是否使用 MIR 镜像模式并删除目标端独有文件？输入 Y 启用 MIR："
if /i "%MIRROR%"=="Y" (
    echo [危险] MIR 镜像模式会删除目标端独有文件。
    set /p "CONFIRM=请输入 CONFIRM 以继续执行："
    if not "!CONFIRM!"=="CONFIRM" exit /b 1
    robocopy "%source_path%" "%target_path%" /MIR /R:3 /W:5 /A-:SH /MT:%THREAD_COUNT% /LOG:"%LOG_FILE%" /TEE /NP /V /XJ /XD $RECYCLE.BIN "System Volume Information" "wechat_files" "xwechat_files" "WeChat Files"  /XF "pagefile.sys" "hiberfil.sys" "swapfile.sys"
) else (
    robocopy "%source_path%" "%target_path%" /E /R:3 /W:5 /A-:SH /MT:%THREAD_COUNT% /LOG:"%LOG_FILE%" /TEE /NP /V /XJ /XD $RECYCLE.BIN "System Volume Information" "wechat_files" "xwechat_files" "WeChat Files"  /XF "pagefile.sys" "hiberfil.sys" "swapfile.sys"
)
call :handle_robo_result "%LOG_FILE%"
exit /b %ROBOCOPY_RC%

:check_target_drive
set "DSTPATH=%~1"
set "DRIVE=%DSTPATH:~0,1%"
if not exist "%DRIVE%:\" (
    echo [错误] 目标驱动器 %DRIVE% 不存在。
    exit /b 1
)
set "FREEGB="
for /f "tokens=*" %%A in ('powershell -NoProfile -Command "try { [int]((Get-PSDrive -Name '%DRIVE%').Free/1GB) } catch { }" 2^>nul') do set "FREEGB=%%A"
if not defined FREEGB (
    echo [警告] 无法检查剩余空间，将在没有空间保护的情况下继续。
    exit /b 0
)
echo [信息] 目标驱动器 %DRIVE% 剩余空间：%FREEGB% GB
if %FREEGB% LSS %MIN_FREE_GB% (
    echo [错误] 目标驱动器剩余空间低于 %MIN_FREE_GB% GB。
    exit /b 1
)
exit /b 0

:gen_logname
set "LOGPREFIX=%~1"
set "STAMP="
for /f "tokens=2 delims==" %%A in ('wmic os get LocalDateTime /value 2^>nul ^| find "="') do set "STAMP=%%A"
if defined STAMP (
    set "STAMP=%STAMP:~0,8%_%STAMP:~8,6%"
) else (
    set "STAMP=%DATE:/=-%_%TIME::=-%"
    set "STAMP=%STAMP: =0%"
)
set "LOG_FILE=%LOG_DIR%\robocopy_%LOGPREFIX%_%STAMP%.log"
exit /b 0

:handle_robo_result
set "ROBOCOPY_RC=%errorlevel%"
echo.
echo Robocopy 退出代码：%ROBOCOPY_RC%
if %ROBOCOPY_RC% LSS 8 (
    echo [完成] Robocopy 已完成，或仅存在非致命差异。
    call :write_audit "Robocopy 已完成。返回码=%ROBOCOPY_RC% 日志=%~1"
) else (
    echo [错误] Robocopy 执行失败，请检查日志：%~1
    call :write_audit "Robocopy 执行失败。返回码=%ROBOCOPY_RC% 日志=%~1"
)
exit /b 0

:wait_seconds
set "SEC=%~1"
timeout /t %SEC% /nobreak >nul 2>&1
if errorlevel 1 ping -n %SEC% 127.0.0.1 >nul 2>&1
exit /b 0

:write_audit
>>"%AUDIT_LOG%" echo [%DATE% %TIME%] %~1
exit /b 0
