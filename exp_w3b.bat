@echo off
chcp 65001 >nul
cd /d C:\Users\21954\freertos-lab\my_project
set PATH=D:\MG_tool\mingw64\bin;%PATH%

echo === 编译 ===
mingw32-make
if errorlevel 1 goto fail

taskkill /F /IM env_monitor.exe >nul 2>&1
del /q w3b.log 2>nul

echo.
echo === 运行 16 秒(到点自动结束)===
start "" /b cmd /c "ping -n 17 127.0.0.1 >nul & taskkill /F /IM env_monitor.exe >nul 2>&1"
build\env_monitor.exe > w3b.log 2>&1
ping -n 2 127.0.0.1 >nul
taskkill /F /IM env_monitor.exe >nul 2>&1

echo.
echo === 反转三角日志 ===
findstr /C:"LOWHOLD" /C:"MIDHOG" /C:"HIGHWAIT" w3b.log

echo.
echo === 自检 ===
powershell -NoProfile -Command "[Console]::OutputEncoding=[Text.Encoding]::UTF8; if(Select-String -Path w3b.log -Pattern '共持锁 6000 ms' -Encoding UTF8){'!! 持锁 6000 ms —— 还是二值版,检查 xSemaphoreCreateMutex() 改了没、上电 Give 注释了没'} elseif(Select-String -Path w3b.log -Pattern '共持锁 3000 ms' -Encoding UTF8){'OK 持锁 3000 ms —— 优先级继承生效,插队者被压住了'} else {'?? 没找到持锁时长,先看完整日志 w3b.log'}"

echo.
echo === 最后一张状态表 ===
powershell -NoProfile -Command "[Console]::OutputEncoding=[Text.Encoding]::UTF8; Get-Content w3b.log -Tail 18 -Encoding UTF8"
echo.
echo 完整日志在: %CD%\w3b.log
goto end

:fail
echo.
echo 编译失败,把上面的报错原样发给老师。

:end
pause
