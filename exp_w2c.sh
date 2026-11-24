#!/bin/bash
# Week2 实验2 补丁版:满仓拒收记账 — 编译 + 暖机 + 验证
cd /c/Users/21954/freertos-lab/my_project || exit 1
echo "=== 编译 ==="
mingw32-make 2>&1 | tail -4
echo "=== 暖机跑 3s(让杀软扫描,输出丢弃)==="
timeout 3 ./build/env_monitor.exe > /dev/null 2>&1
echo "=== 正式运行 20s ==="
timeout 20 ./build/env_monitor.exe > w2c.log
echo "run-exit=$?"
echo "=== 事件流全部 EVT 行 ==="
grep -E 'EVT-SRC|EVT-CONS' w2c.log
echo "=== 统计行(看满仓拒=)==="
grep '栈高水位' w2c.log
