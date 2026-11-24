@echo off
chcp 65001 >nul
cd /d C:\Users\21954\freertos-lab\my_project
set PATH=D:\MG_tool\mingw64\bin;%PATH%

echo === 编译 ===
mingw32-make
if errorlevel 1 goto fail

taskkill /F /IM env_monitor.exe >nul 2>&1
del /q w3a.log 2>nul

echo.
echo === 运行 16 秒(到点自动结束)===
start "" /b cmd /c "ping -n 17 127.0.0.1 >nul & taskkill /F /IM env_monitor.exe >nul 2>&1"
build\env_monitor.exe > w3a.log 2>&1
ping -n 2 127.0.0.1 >nul
taskkill /F /IM env_monitor.exe >nul 2>&1

echo.
echo === 反转三角日志 ===
findstr /C:"LOWHOLD" /C:"MIDHOG" /C:"HIGHWAIT" /C:"W3_ISOLATE" w3a.log
echo.
echo === 最后一张状态表 ===
powershell -NoProfile -Command "[Console]::OutputEncoding=[Text.Encoding]::UTF8; Get-Content w3a.log -Tail 18 -Encoding UTF8"
echo.
echo 完整日志在: %CD%\w3a.log
goto end

:fail
echo.
echo 编译失败,把上面的报错原样发给老师。

:end
pause
