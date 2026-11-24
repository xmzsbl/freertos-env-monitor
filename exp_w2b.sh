#!/bin/bash
# Week2 实验2:计数信号量 — 编译 + 暖机 + 验证
cd /c/Users/21954/freertos-lab/my_project || exit 1
echo "=== 编译 ==="
mingw32-make 2>&1 | tail -4
echo "=== 暖机跑 3s(让杀软扫描,输出丢弃)==="
timeout 3 ./build/env_monitor.exe > /dev/null 2>&1
echo "=== 正式运行 18s ==="
timeout 18 ./build/env_monitor.exe > w2b.log
echo "run-exit=$?"
echo "=== 横幅 ==="
grep 'MAIN' w2b.log
echo "=== 事件流(全部 EVT 行)==="
grep -E 'EVT-SRC|EVT-CONS' w2b.log
echo "=== 状态表中的新老任务 ==="
grep -A11 '任务状态表' w2b.log | grep -E 'EvtSrc|EvtCons|IsrSim|Wake'
