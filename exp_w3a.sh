#!/bin/bash
# 实验 3-A:优先级反转复现(二值信号量 = 无优先级继承)
# 预测:对照(无插队)= 2600 ms | 本实验(二值)= 5600 ms | mutex(继承)= 2600 ms
cd /c/Users/21954/freertos-lab/my_project || exit 1
export MSYS2_ARG_CONV_EXCL="*"        # 让 taskkill 的 /F 不被 MSYS 当路径改写

echo "=== 编译 ==="
mingw32-make 2>&1 | grep -E "error|Error|错误" && { echo ">>> 编译失败,先修报错"; exit 1; }
echo "编译通过(defined but not used 的警告是隔离模式的正常现象)"

taskkill /F /IM env_monitor.exe > /dev/null 2>&1

# 新编译的 exe 首次启动会被杀软扫描,可能十几秒零输出 ——
# 所以失败的尝试直接丢掉重试,最多 3 次,取第一次真正跑完的
OK=0
for try in 1 2 3; do
  echo "=== 第 $try 次尝试:跑 16s ==="
  rm -f w3a.log
  ./build/env_monitor.exe > w3a.log 2>&1 &
  sleep 16
  taskkill /F /IM env_monitor.exe > /dev/null 2>&1
  sleep 1

  if grep -q "拿到锁" w3a.log; then
    OK=1
    echo ">>> 第 $try 次成功"
    break
  else
    echo ">>> 第 $try 次没跑完(多半是杀软扫描卡住),重试"
  fi
  sleep 1
done

if [ "$OK" != "1" ]; then
  echo ">>> 三次都没跑完,先看 w3a.log 最后几行:"; tail -5 w3a.log; exit 1
fi

echo
echo "=== 反转三角日志 ==="
grep -E "LOWHOLD|MIDHOG|HIGHWAIT|W3_ISOLATE" w3a.log
echo
echo "=== 抓到的反转现场(状态表 R/B + CPU)==="
grep -B2 -A2 "LowHold" w3a.log | grep -E "Stats|MidHog|LowHold|HighWait|IDLE|CPU" | head -24
