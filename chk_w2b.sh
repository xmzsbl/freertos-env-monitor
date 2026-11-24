#!/bin/bash
# 实验2 健康检查:强杀残留 -> 后台起 exe -> 8s 后 taskkill 硬杀 -> 看日志
cd /c/Users/21954/freertos-lab/my_project || exit 1
echo "=== 清场:杀残留 ==="
taskkill //F //IM env_monitor.exe 2>/dev/null
echo "=== 后台起新 exe ==="
./build/env_monitor.exe > w2b.log 2>&1 &
EXE_PID=$!
sleep 8
taskkill //F //PID $EXE_PID 2>/dev/null
wait $EXE_PID 2>/dev/null
echo "killed at 8s"
echo "=== 横幅 ==="
grep 'MAIN' w2b.log
echo "=== EVT 事件流 ==="
grep -E 'EVT-SRC|EVT-CONS' w2b.log
echo "=== SENSOR 前 3 条 ==="
grep SENSOR w2b.log | head -3
