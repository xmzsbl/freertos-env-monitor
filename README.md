# FreeRTOS 环境监测与告警终端（Windows 仿真）

在 Windows 上用 FreeRTOS 演示模拟温湿度采样、队列通信、共享数据保护和任务调度。工程包含环境监测模式和独立的优先级反转实验，不需要开发板。

## 克隆、构建和运行

**准备环境：** Windows、Git、MinGW-w64 的 `gcc` 和 `mingw32-make`。把工具链的 `bin` 目录加入 `PATH`，并选择不含空格的克隆路径。内核源码随项目放在 `Source/`，`Makefile` 通过 `KERNEL_DIR := Source` 使用它，不需要另下内核、克隆子模块或配置本机目录链接。

在 **Windows CMD** 中执行：

```bat
git clone https://github.com/xmzsbl/freertos-env-monitor.git
cd freertos-env-monitor
where gcc
where mingw32-make
mingw32-make
build\env_monitor.exe
```

构建产物是 `build/env_monitor.exe`。在 CMD 中运行可以直接看到日志，按 `Ctrl+C` 停止。如果修改过模式或想确保未复用旧产物，用 `mingw32-make -B` 重新构建。`build/` 不提交到 Git；如需分发可执行文件，通过 GitHub Releases 提供。

## 运行模式

在 [`main.c`](main.c) 中修改 `W3_ISOLATE`，保存后重新编译。

| 设置 | 启动内容 | 验证方式 |
|---|---|---|
| `W3_ISOLATE = 1`（默认） | `LowHold`、`MidHog`、`HighWait` 和 `Stats`，默认使用 mutex | 查看 `W3_ISOLATE=1 实验模式` 和 `[HIGHWAIT]` 的等锁时长 |
| `W3_ISOLATE = 0` | 环境监测任务、二值与计数信号量演示、`Stats` | 查看 `[SENSOR]`、`[ALARM ]` 等实际任务输出；用相同 `seq` 对照采样和告警 |

启动横幅中的固定任务清单不是实际运行模式的依据。[环境监测样例日志](docs/sample_run.log)来自历史运行，不代表默认模式的输出。

## 环境监测数据流

| 路径 | 实现 |
|---|---|
| `Sensor → xSampleQueue → Comm` | `Sensor` 用 `vTaskDelayUntil` 周期生成模拟样本，`Comm` 阻塞等待并接收完整样本。 |
| `Sensor → xAlarmQueue → Alarm` | 样本温度严格大于 `29.0 °C` 时，尝试投递告警事件；`Alarm` 接收成功后打印。持续超温会持续尝试投递，不是只在越过阈值的一刻告警。 |
| `Sensor → xLatest → Display` | `xLatestMutex` 保护共享样本；`Display` 持锁复制，释放后在控制台打印温湿度。 |

`KeySim` 用控制台字符模拟按键，`Stats` 提供任务状态、累计 CPU 占比和栈高水位。`IsrSim` 是普通任务，不是真正的硬件中断。`Comm` 只用计数与日志演示正常上报和断网分支，**不联网、不保存样本、不写 Flash，也不补发**。`[OLED]` 是日志标签，不代表驱动了实体屏幕。

## 优先级反转对照

低优先级 `LowHold` 先持有同步对象，高优先级 `HighWait` 阻塞等待。中优先级 `MidHog` 不碰这个对象，但在无优先级继承时会抢占就绪的 `LowHold`，让它更晚释放。换成 mutex 后，`LowHold` 临时继承优先级 3；与优先级 2 的 `MidHog` 同时就绪时，`LowHold` 优先运行。

| 同步对象 | `HighWait` 等待时长 | 参考日志 |
|---|---:|---|
| 二值信号量 | 5600 ms | [二值信号量版](docs/实验3_参考读数_二值信号量.txt) |
| mutex | 2601 ms | [mutex 版](docs/实验3_参考读数_mutex.txt) |

复现二值信号量版时，在 `main.c` 把 `xInvSem` 的创建改为 `xSemaphoreCreateBinary()`，保留创建失败检查，并在检查通过后、创建实验任务之前先 `Give` 一次，让它初始可取。切回 mutex 时不做这次初始 `Give`。两次都保持任务优先级与启动顺序不变，重编后比较 `[HIGHWAIT]` 行的「一共等了」，不要把「拿到锁」的时间戳当作等待时长。

以上是历史 Windows 仿真读数，后续运行可能有波动，不能当作真机实时性指标。仓库中的实验脚本是早期本机辅助工具，部分含旧路径；首次构建请使用上面的 CMD 命令。

## 内核版本与许可

`Source/` 是第三方 FreeRTOS-Kernel 源码，固定于提交 [`92bfb86ce3a3f5d0255e5b2e51636e5a5732b66a`](https://github.com/FreeRTOS/FreeRTOS-Kernel/commit/92bfb86ce3a3f5d0255e5b2e51636e5a5732b66a)，头文件版本标识为 `V11.1.0+`。构建使用 `MSVC-MingW` 移植层和 `heap_4`，内核许可见 [`Source/LICENSE.md`](Source/LICENSE.md)。
