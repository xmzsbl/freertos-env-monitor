#!/bin/bash
# Week2 实验1:二值信号量唤醒任务 — 编译并验证
cd /c/Users/21954/freertos-lab/my_project || exit 1
echo "=== 编译 ==="
mingw32-make 2>&1 | tail -4
echo "=== 运行 14s ==="
timeout 14 ./build/env_monitor.exe > w2a.log
echo "run-exit=$?"
echo "=== 横幅 + 信号量事件 ==="
grep -E 'MAIN|ISRSIM|WAKEUP' w2a.log
echo "=== 10s 任务状态表 ==="
grep -A11 '任务状态表' w2a.log | head -13
echo "=== CPU 表 ==="
grep -A11 'CPU 占用' w2a.log | head -13
