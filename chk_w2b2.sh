#!/bin/bash
# 验尸:后台 exe 跑了~60s,日志应有 10 波数据;然后清场
cd /c/Users/21954/freertos-lab/my_project || exit 1
echo "=== w2b.log 规模 ==="
wc -l w2b.log
echo "=== 横幅 ==="
grep 'MAIN' w2b.log
echo "=== EVT 事件流(前 24 行)==="
grep -E 'EVT-SRC|EVT-CONS' w2b.log | head -24
echo "=== EVT 总行数 ==="
grep -cE 'EVT-SRC' w2b.log
grep -cE 'EVT-CONS' w2b.log
echo "=== 清场 ==="
taskkill //F //IM env_monitor.exe
sleep 1
tasklist | grep -i env_monitor
echo "check-done"
