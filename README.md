# FreeRTOS 多传感器环境监测与告警终端(仿真版)

在**无硬件条件**下,用 FreeRTOS Windows 模拟器(MinGW port)实现的 RTOS 终端:6 任务并发、双队列 + 互斥量通信、告警事件链、断网缓存补发、任务状态/CPU 统计、按键消抖轮询,以及两组调度对照实验。

## 特性

| 任务 | 周期/触发 | 职责 |
|---|---|---|
| Sensor | vTaskDelayUntil 1s | 温湿度采样 → 队列分发 + 超温事件 + latest 互斥量 |
| Alarm | 等事件队列 | 超温事件 → 告警计数与打印 |
| Comm | 等数据队列 | 每 5 条上报;断网时本地缓存,恢复后补发(丢弃=0) |
| Display | vTaskDelayUntil 0.5s | 互斥量读 latest → OLED 刷新 |
| KeySim | vTaskDelay 50ms | kbhit 轮询 + 200ms 消抖(非阻塞,CPU <1%) |
| Stats | vTaskDelay 10s | vTaskList 状态表 + 运行时 CPU 占用 + 栈高水位 |

- 温湿度波形可配置(基线 / 振幅 / 周期 / 噪声),带量程钳位
- 告警阈值 29°C:实测告警"进出切换"而非每秒风暴(见 docs/sample_run.log)

## 实验结论

| 实验 | 数据 | 结论 |
|---|---|---|
| 忙等 vs 非阻塞轮询 | 功能相同:IDLE 96% vs <1%;忙等版状态恒 R | 恒 R 不睡 = 忙等指纹;优先级决定谁能救 CPU |
| vTaskDelay vs vTaskDelayUntil | 每圈工作 3ms:每 25 圈 +75ms 线性累积,2 小时差 108 秒;Until 绝对时刻锚定,恒 +0 | 周期任务必须用绝对时刻延时 |

## 构建依赖

- FreeRTOS-Kernel(Win32-MingW port),位于仓库同级 `../Source`、`../portable`
- 源码镜像:`gitee.com/mirrors/FreeRTOS`、`gitcode.com/FreeRTOS/FreeRTOS-Kernel`(本机网络 GitHub 不可达)
- 工具链:MinGW-w64 GCC + mingw32-make
- **注意**:工作区路径含空格会导致 Makefile 解析失败,本机经 junction 访问
  `C:\Users\21954\freertos-lab`(→ 工作区目录)

## 运行

```
cd my_project
mingw32-make run
```

启动横幅 → 窗口内按任意键验证消抖 → 每 10 秒打印任务状态表 / CPU 占用 / 栈高水位。

## 目录

- `main.c` — 全部 6 任务、钩子与主函数
- `Makefile` — 构建规则
- `docs/sample_run.log` — 30 秒参考运行日志(告警进出切换 + 统计表)
