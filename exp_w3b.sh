#!/bin/bash
# 实验 3-B:同一场景,锁换成 mutex(优先级继承)
# 预测:等待从 5600 ms 掉回 2600 ms,持锁从 6000 ms 掉回 3000 ms
cd /c/Users/21954/freertos-lab/my_project || exit 1
export MSYS2_ARG_CONV_EXCL="*"

echo "=== 编译 ==="
mingw32-make 2>&1 | grep -E "error|Error|错误" && { echo ">>> 编译失败,先修报错"; exit 1; }
echo "编译通过"

taskkill /F /IM env_monitor.exe > /dev/null 2>&1

OK=0
for try in 1 2 3; do
  echo "=== 第 $try 次尝试:跑 16s ==="
  rm -f w3b.log
  ./build/env_monitor.exe > w3b.log 2>&1 &
  sleep 16
  taskkill /F /IM env_monitor.exe > /dev/null 2>&1
  sleep 1
  if grep -q "拿到锁" w3b.log; then OK=1; echo ">>> 第 $try 次成功"; break; fi
  echo ">>> 第 $try 次没跑完(杀软扫描?),重试"; sleep 1
done

if [ "$OK" != "1" ]; then echo ">>> 三次都没跑完:"; tail -5 w3b.log; exit 1; fi

echo
echo "=== 反转三角日志 ==="
grep -E "LOWHOLD|MIDHOG|HIGHWAIT" w3b.log
echo
echo "=== 自检 ==="
if grep -q "共持锁 6000 ms" w3b.log; then
  echo "!! 持锁仍是 6000 ms —— 看起来还是二值信号量那版,检查 xSemaphoreCreateMutex() 改没改"
elif grep -q "共持锁 3000 ms" w3b.log; then
  echo "OK 持锁 3000 ms = 只有自己的活,没有人能插队(继承生效)"
fi
