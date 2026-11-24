#!/bin/bash
# Week1 收尾:编译并验证 KeySim 按键任务
cd /c/Users/21954/freertos-lab/my_project || exit 1
echo "=== 编译 ==="
mingw32-make 2>&1 | tail -3
echo "=== 运行 12s ==="
timeout 12 ./build/env_monitor.exe > key.log
echo "run-exit=$?"
echo "=== 开头 3 行 ==="
head -3 key.log
echo "=== KEY / KeySim 相关 ==="
grep -E 'KEY|KeySim' key.log
