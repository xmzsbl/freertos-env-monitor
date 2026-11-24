/*
 * 项目 A 骨架:基于 FreeRTOS 的多传感器环境监测与告警终端(Windows 仿真版)
 *
 * 与真机版的对应关系:
 *   - 传感器读数 = 模拟公式生成的数据(真机上换成 SHT30 I2C 读取)
 *   - OLED 显示   = 控制台打印(真机上换成 SPI/I2C 屏幕驱动)
 *   - MQTT 上云   = 控制台打印 publish/断网日志(真机上换成 ESP8266 AT 指令)
 *   - 告警输出   = 控制台 ALARM 日志(真机上换成蜂鸣器 + LED)
 *   - 按键        = 控制台 kbhit() 轮询(真机上换成 GPIO 中断/轮询)
 *
 * 本文件演示的 FreeRTOS 机制(面试必会):
 *   1. 任务划分与优先级(采集 3 > 告警/通信 2 > 显示/按键 1)
 *   2. Queue   —— 采集→通信 的数据流、采集→告警 的事件流
 *   3. Mutex   —— 保护共享的 latest 样本、保护 printf(多任务打印不乱)
 *   4. vTaskList / vTaskGetRunTimeStats / uxTaskGetStackHighWaterMark —— 运行时自省
 *   5. 栈溢出钩子 + malloc 失败钩子 + configASSERT —— 故障自诊断
 *   6. 二值信号量(Week 2)—— 模拟「中断唤醒任务」:平时阻塞零 CPU,事件到才醒
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <windows.h> /* SetConsoleOutputCP: 切换控制台代码页 */
#include <conio.h>   /* kbhit/_getch: 按键模拟任务 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* ------------------------------------------------------------------ */
/* 配置                                                                 */
/* ------------------------------------------------------------------ */
#define SENSOR_PERIOD_MS    1000U   /* 采集周期 1 s */
#define DISPLAY_PERIOD_MS    500U   /* 显示刷新 500 ms(打印节流到 1 Hz) */
#define STATS_PERIOD_MS    10000U   /* 任务状态表 10 s 打一次 */
#define KEY_POLL_MS          50U   /* 按键轮询周期(带延时,不空转) */
#define KEY_DEBOUNCE_MS     200U   /* 按键消抖窗口 */
#define ISR_SIM_PERIOD_MS  3000U   /* 模拟中断间隔:每 3 s 触发一次(Week 2 实验 1) */
#define WAKE_WORK_MS         50U   /* 被中断唤醒后的处理耗时 */
#define EVT_PERIOD_MS      5000U   /* 事件源:每 5 s 发一波(实验 2) */
#define EVT_BURST_N           3U   /* 每波事件个数 */
#define EVT_WORK_MS        1000U   /* 消费一个事件耗时 1 s(故意比生产慢) */
#define TEMP_ALARM_THRESHOLD 29.0f  /* 温度告警阈值(℃) */
#define W3_HOLD_START_MS 200
#define W3_WAIT_START_MS 600
#define W3_HOG_START_MS 1200
#define W3_HOLD_WORK_MS 3000
#define W3_HOG_BUSY_MS 3000
#define W3_ISOLATE          1U      /* 1 = 实验模式:只创建 Stats + 反转三角 */
#if ( W3_ISOLATE == 1 )
    #define W3_STATS_PRIO   4       /* 观测者必须高于被观察者(插队者 2) */
#else
    #define W3_STATS_PRIO   1
#endif


#define QUEUE_LEN 4

/* ------------------------------------------------------------------ */
/* 数据类型                                                             */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint32_t seq;
    float    temperature;
    float    humidity;
    TickType_t tick;
} Sample_t;

typedef struct
{
    uint32_t seq;
    float    temperature;
} AlarmEvent_t;

/* ------------------------------------------------------------------ */
/* 任务间通信对象                                                        */
/* ------------------------------------------------------------------ */
static QueueHandle_t   xSampleQueue  = NULL; /* 采集 → 通信 */
static SemaphoreHandle_t xInvSem = NULL;
static QueueHandle_t   xAlarmQueue   = NULL; /* 采集 → 告警(事件) */
static SemaphoreHandle_t xLatestMutex = NULL; /* 保护 latest(互斥量) */
static SemaphoreHandle_t xLogMutex    = NULL; /* 保护 printf(互斥量) */
static SemaphoreHandle_t xIsrSem      = NULL; /* 二值信号量:模拟「中断事件到达」 */
static SemaphoreHandle_t xEvtSem      = NULL; /* 计数信号量:累积 0..EVT_BURST_N 个事件 */

static TaskHandle_t xSensorHandle = NULL;
static TaskHandle_t xAlarmHandle  = NULL;
static TaskHandle_t xCommHandle   = NULL;
static TaskHandle_t xDisplayHandle = NULL;
static TaskHandle_t xStatsHandle  = NULL;
static TaskHandle_t xIsrSimHandle = NULL;
static TaskHandle_t xWakeHandle   = NULL;
static TaskHandle_t xLowHoldHandle  = NULL; /* 实验3:持锁者(最低优先级) */
static TaskHandle_t xMidHogHandle   = NULL; /* 实验3:插队者(纯计算,不碰锁) */
static TaskHandle_t xHighWaitHandle = NULL; /* 实验3:等锁者(最高优先级) */


/* 最新样本(显示任务读,采集任务写 → 互斥量保护的共享变量) */
static Sample_t xLatest;

/* 统计计数 */
static volatile uint32_t ulPublished    = 0;
static volatile uint32_t ulDropped      = 0; /* 队列满丢弃的样本数 */
static volatile uint32_t ulOfflineStore = 0; /* 模拟断网转存 Flash 的条数 */
static volatile uint32_t ulAlarmCount   = 0;
static volatile uint32_t ulKeyCount     = 0; /* 有效按键次数(消抖后) */
static volatile uint32_t ulEvtGiven     = 0; /* 事件源累计发出个数 */
static volatile uint32_t ulEvtTaken     = 0; /* 消费者累计处理个数 */
static volatile uint32_t ulEvtRejected  = 0; /* 满仓被 Give 拒收的事件数 */

/* ------------------------------------------------------------------ */
/* 线程安全的日志封装                                                    */
/* ------------------------------------------------------------------ */
static void vSafePrintf( const char * pcFormat, ... )
{
    char    cBuf[1200]; /* 任务表可能很长,256 会被 vsnprintf 截断 */
    va_list args;

    va_start( args, pcFormat );
    vsnprintf( cBuf, sizeof( cBuf ), pcFormat, args );
    va_end( args );

    xSemaphoreTake( xLogMutex, portMAX_DELAY );
    {
        /* 仿真环境下再套一层临界区:FreeRTOS Windows 端口里 printf 可能触发
         * 线程切换,临界区保证一条日志不会被别的任务拦腰截断。 */
        taskENTER_CRITICAL();
        {
            printf( "%s", cBuf );
            fflush( stdout );
        }
        taskEXIT_CRITICAL();
    }
    xSemaphoreGive( xLogMutex );
}

/* ------------------------------------------------------------------ */
/* 任务 1:采集(优先级最高,周期 1 s)                                    */
/* ------------------------------------------------------------------ */
static void prvSensorTask( void * pvParameters )
{
    TickType_t xLastWake;
    uint32_t   ulSeq = 0;

    ( void ) pvParameters;
    xLastWake = xTaskGetTickCount();

    for( ; ; )
    {
        Sample_t xSample;

        vTaskDelayUntil( &xLastWake, pdMS_TO_TICKS( SENSOR_PERIOD_MS ) );

        ulSeq++;
        xSample.seq        = ulSeq;

        /* ===== 你的设计区(Week 1 验收项)=====
         * 把下面 temperature / humidity 两行公式换成你自己设计的形态。
         * 要求:形态自己定 —— 锯齿波、阶梯爬升、随机游走、"白天高夜里低"
         * 的作息曲线……都可以;数值范围别太离谱(温度建议 15~40,湿度 30~90)。
         * 改法:VS Code 打开 main.c → 改这两行 → 保存 → mingw32-make run
         * 验收:[SENSOR] 打出来的数字符合你设计的规律,告警还能正常触发。 */
        xSample.temperature = 25.0f + 8.0f * sinf( ( float ) ulSeq * 0.35f )
                              + ( ( float ) ( rand() % 100 ) - 50.0f ) / 100.0f;
        xSample.humidity    = 60.0f + 10.0f * sinf( ( float ) ulSeq * 0.2f + 1.0f )
                              + ( ( float ) ( rand() % 100 ) - 50.0f ) / 100.0f;
        if( xSample.humidity > 100.0f ) xSample.humidity = 100.0f;   // ← 新增:上限钳位
        if( xSample.humidity < 0.0f   ) xSample.humidity = 0.0f;     // ← 新增:下限钳位
        /* ===== 设计区结束 ===== */

        xSample.tick        = xTaskGetTickCount();

        /* 写共享变量:互斥量保护 */
        xSemaphoreTake( xLatestMutex, portMAX_DELAY );
        {
            xLatest = xSample;
        }
        xSemaphoreGive( xLatestMutex );

        /* 数据流 → 通信任务(队列满则丢弃并计数,绝不阻塞采集) */
        if( xQueueSend( xSampleQueue, &xSample, 0 ) != pdPASS )
        {
            ulDropped++;
        }

        /* 超阈值 → 事件流 → 告警任务 */
        if( xSample.temperature > TEMP_ALARM_THRESHOLD )
        {
            AlarmEvent_t xEvent = { xSample.seq, xSample.temperature };
            ( void ) xQueueSend( xAlarmQueue, &xEvent, 0 );
        }

        vSafePrintf( "[SENSOR] seq=%3lu  T=%5.2f C  H=%5.2f %%\n",
                     ( unsigned long ) xSample.seq,
                     xSample.temperature,
                     xSample.humidity );
    }
}

/* ------------------------------------------------------------------ */
/* 任务 2:告警(事件驱动,平时阻塞在队列上不占 CPU)                       */
/* ------------------------------------------------------------------ */
static void prvAlarmTask( void * pvParameters )
{
    ( void ) pvParameters;

    for( ; ; )
    {
        AlarmEvent_t xEvent;

        if( xQueueReceive( xAlarmQueue, &xEvent, portMAX_DELAY ) == pdPASS )
        {
            ulAlarmCount++;
            vSafePrintf( "[ALARM ] *** 温度超限! seq=%lu  T=%.2f C(阈值 %.1f) ***\n",
                         ( unsigned long ) xEvent.seq,
                         xEvent.temperature,
                         TEMP_ALARM_THRESHOLD );
        }
    }
}

/* ------------------------------------------------------------------ */
/* 任务 3:通信(消费采集数据,模拟 MQTT 上报与断网转存)                   */
/* ------------------------------------------------------------------ */
static void prvCommTask( void * pvParameters )
{
    ( void ) pvParameters;

    for( ; ; )
    {
        Sample_t xSample;

        if( xQueueReceive( xSampleQueue, &xSample, portMAX_DELAY ) == pdPASS )
        {
            /* 每 20 条模拟一次"断网",数据转存本地 Flash 缓存 */
            if( ( xSample.seq % 20U ) == 0U )
            {
                ulOfflineStore++;
                vSafePrintf( "[COMM  ] 网络异常,seq=%lu 转存本地 Flash 缓存(累计 %lu 条)\n",
                             ( unsigned long ) xSample.seq,
                             ( unsigned long ) ulOfflineStore );
            }
            else
            {
                ulPublished++;
                if( ( xSample.seq % 5U ) == 0U ) /* 节流打印,避免刷屏 */
                {
                    vSafePrintf( "[COMM  ] publish seq=%lu -> broker(累计 %lu 条)\n",
                                 ( unsigned long ) xSample.seq,
                                 ( unsigned long ) ulPublished );
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 任务 4:显示(优先级最低——本地显示永远可以等)                          */
/* ------------------------------------------------------------------ */
static void prvDisplayTask( void * pvParameters )
{
    TickType_t xLastWake;
    uint32_t   ulTicks = 0;

    ( void ) pvParameters;
    xLastWake = xTaskGetTickCount();

    for( ; ; )
    {
        Sample_t xCopy;

        vTaskDelayUntil( &xLastWake, pdMS_TO_TICKS( DISPLAY_PERIOD_MS ) );

        /* 真机 OLED 每 500 ms 刷新;控制台每 2 次(1 Hz)打印一次,防止刷屏 */
        ulTicks++;
        if( ( ulTicks % 2U ) != 0U )
        {
            continue;
        }

        xSemaphoreTake( xLatestMutex, portMAX_DELAY );
        {
            xCopy = xLatest;
        }
        xSemaphoreGive( xLatestMutex );

        vSafePrintf( "[OLED  ] T=%5.2fC  H=%5.2f%%  alarm=%lu  pub=%lu\n",
                     xCopy.temperature,
                     xCopy.humidity,
                     ( unsigned long ) ulAlarmCount,
                     ( unsigned long ) ulPublished );
    }
}

/* ------------------------------------------------------------------ */
/* 任务 5:按键模拟(Week 1 验收项)                                       */
/*   真机 = GPIO 按键;主机上没有,用 kbhit() 轮询控制台输入等价模拟:     */
/*   - 50 ms 周期轮询,循环里带 vTaskDelay —— 绝不空转(忙等实验的教训)  */
/*   - 按下后 200 ms 消抖窗口,窗口内连击只算一次(真机按键同理)          */
/* ------------------------------------------------------------------ */
static void prvKeySimTask( void * pvParameters )
{
    ( void ) pvParameters;

    vSafePrintf( "[KEY   ] 按键任务就绪:在本窗口按任意键 → 打印一行事件\n" );

    for( ; ; )
    {
        vTaskDelay( pdMS_TO_TICKS( KEY_POLL_MS ) ); /* 周期轮询,阻塞不占 CPU */

        if( kbhit() != 0 )
        {
            ( void ) _getch(); /* 吃掉字符,避免残留在输入缓冲区 */

            ulKeyCount++;
            vSafePrintf( "[KEY   ] 按键触发!有效第 %lu 次\n",
                         ( unsigned long ) ulKeyCount );

            /* 消抖:窗口内的连击(长按自动重复)只算一次 */
            vTaskDelay( pdMS_TO_TICKS( KEY_DEBOUNCE_MS ) );
            while( kbhit() != 0 )
            {
                ( void ) _getch();
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 任务 6:运行时自省(vTaskList / 运行时间统计 / 栈高水位)                */
/* ------------------------------------------------------------------ */
static void prvStatsTask( void * pvParameters )
{
    char cTaskList[ 1024 ];
    char cRunTime[ 1024 ];

    ( void ) pvParameters;

    for( ; ; )
    {
        #if ( W3_ISOLATE == 1 )//vTaskDelay( pdMS_TO_TICKS( STATS_PERIOD_MS ) );
            vTaskDelay( pdMS_TO_TICKS( 2000U ) );   /* 实验模式 2 s 一张,才抓得到反转中段 */
        #else
            vTaskDelay( pdMS_TO_TICKS( STATS_PERIOD_MS ) );
        #endif


        cTaskList[ 0 ] = '\0';
        vTaskList( cTaskList ); /* 需要 configUSE_TRACE_FACILITY=1 */

        vSafePrintf( "[STATS ] ---- 任务状态表(status/优先级/栈剩余) ----\n%s", cTaskList );

        cRunTime[ 0 ] = '\0';
        vTaskGetRunTimeStats( cRunTime ); /* 需要 configGENERATE_RUN_TIME_STATS=1 */
        vSafePrintf( "[STATS ] ---- CPU 占用(%%) ----\n%s", cRunTime );
    #if ( W3_ISOLATE == 0 )
        vSafePrintf( "[STATS ] 栈高水位(最小剩余,单位=栈深度单位):"
                     " Sensor=%lu Alarm=%lu Comm=%lu Display=%lu Stats=%lu"
                     " IsrSim=%lu Wake=%lu"
                     " | 丢弃=%lu 断网缓存=%lu 按键=%lu 满仓拒=%lu\n",
                     ( unsigned long ) uxTaskGetStackHighWaterMark( xSensorHandle ),
                     ( unsigned long ) uxTaskGetStackHighWaterMark( xAlarmHandle ),
                     ( unsigned long ) uxTaskGetStackHighWaterMark( xCommHandle ),
                     ( unsigned long ) uxTaskGetStackHighWaterMark( xDisplayHandle ),
                     ( unsigned long ) uxTaskGetStackHighWaterMark( xStatsHandle ),
                     ( unsigned long ) uxTaskGetStackHighWaterMark( xIsrSimHandle ),
                     ( unsigned long ) uxTaskGetStackHighWaterMark( xWakeHandle ),
                     ( unsigned long ) ulDropped,
                     ( unsigned long ) ulOfflineStore,
                     ( unsigned long ) ulKeyCount,
                     ( unsigned long ) ulEvtRejected );
    #endif
    }
}

/* ------------------------------------------------------------------ */
/* 任务 7:模拟中断源(Week 2 实验 1:二值信号量唤醒任务)                */
/*   真机 = 外设中断服务函数(ISR);主机上没有硬件中断,用一个最高优先级  */
/*   任务等价模拟 ISR 的行为:                                          */
/*   - 中断比所有任务都急 → 优先级 4(高于采集 3)                       */
/*   - ISR 职责极小:给一下信号量就走(真 ISR 里连 printf 都不该有)     */
/* ------------------------------------------------------------------ */
static void prvIsrSimTask( void * pvParameters )
{
    uint32_t ulIrq = 0;

    ( void ) pvParameters;

    for( ; ; )
    {
        vTaskDelay( pdMS_TO_TICKS( ISR_SIM_PERIOD_MS ) ); /* "中断"每 3 s 来一次 */

        ulIrq++;
        vSafePrintf( "[ISRSIM] (模拟中断) 第 %lu 次触发 -> xSemaphoreGive\n",
                     ( unsigned long ) ulIrq );
        ( void ) xSemaphoreGive( xIsrSem ); /* 真正的 ISR 里,干活的就是这一下 */
    }
}

/* ------------------------------------------------------------------ */
/* 任务 8:被中断唤醒的处理任务                                          */
/*   平时阻塞在信号量上:不轮询、零 CPU;事件到才醒(与实验 A 相反)     */
/* ------------------------------------------------------------------ */
static void prvWakeTask( void * pvParameters )
{
    uint32_t ulWoke = 0;

    ( void ) pvParameters;

    for( ; ; )
    {
        /* portMAX_DELAY = 没有事件就永远睡,不忙等 */
        if( xSemaphoreTake( xIsrSem, portMAX_DELAY ) == pdPASS )
        {
            ulWoke++;
            vSafePrintf( "[WAKEUP ] 被中断唤醒!第 %lu 次 -> 处理 %u ms\n",
                         ( unsigned long ) ulWoke, ( unsigned int ) WAKE_WORK_MS );
            vTaskDelay( pdMS_TO_TICKS( WAKE_WORK_MS ) ); /* 模拟一段处理工作 */
        }
    }
}

/* ------------------------------------------------------------------ */
/* 任务 9:事件源(Week 2 实验 2:计数信号量)—— 每 5 s 连发 3 个事件     */
/*   对照点:同样的 3 连发,二值信号量只留 1 个;计数信号量记满 3 个     */
/* ------------------------------------------------------------------ */
static void prvEvtSrcTask( void * pvParameters )
{
    uint32_t ulWave = 0;
    uint32_t i;

    ( void ) pvParameters;

    for( ; ; )
    {
        vTaskDelay( pdMS_TO_TICKS( EVT_PERIOD_MS ) );

        ulWave++;
        for( i = 0; i < EVT_BURST_N; i++ )
        {
            /* 只有真进账才记"已发";满仓被拒单独记账,不许账面说谎 */
            if( xSemaphoreGive( xEvtSem ) == pdPASS )
            {
                ulEvtGiven++;
                vSafePrintf( "[EVT-SRC] 波次 %lu:发事件 %lu/%u(计数=%d)\n",
                             ( unsigned long ) ulWave,
                             ( unsigned long ) ( i + 1 ),
                             ( unsigned int ) EVT_BURST_N,
                             ( int ) uxSemaphoreGetCount( xEvtSem ) );
            }
            else
            {
                ulEvtRejected++;
                vSafePrintf( "[EVT-SRC] 波次 %lu:第 %lu 个事件满仓被拒(累计拒 %lu)\n",
                             ( unsigned long ) ulWave,
                             ( unsigned long ) ( i + 1 ),
                             ( unsigned long ) ulEvtRejected );
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 任务 10:事件消费者 —— 每个处理 1 s,故意比生产慢(让积压可见)        */
/* ------------------------------------------------------------------ */
static void prvEvtConsTask( void * pvParameters )
{
    ( void ) pvParameters;

    for( ; ; )
    {
        if( xSemaphoreTake( xEvtSem, portMAX_DELAY ) == pdPASS )
        {
            ulEvtTaken++;
            vSafePrintf( "[EVT-CONS] 消费事件(累计 %lu,给方入账 %lu)→ 处理 %u ms\n",
                         ( unsigned long ) ulEvtTaken,
                         ( unsigned long ) ulEvtGiven,
                         ( unsigned int ) EVT_WORK_MS );
            vTaskDelay( pdMS_TO_TICKS( EVT_WORK_MS ) );
        }
    }
}

/* ------------------------------------------------------------------ */
/* 实验 3-A:优先级反转三角(错误示范版:锁用二值信号量,无优先级继承)  */
/* ------------------------------------------------------------------ */

/* 持锁者:三者里优先级最低 —— 反转的地基 */
static void prvLowHoldTask( void * pvParameters )
{
    uint32_t   i;
    TickType_t tTake = 0;
    TickType_t tGive = 0;

    ( void ) pvParameters;

    vTaskDelay( pdMS_TO_TICKS( W3_HOLD_START_MS ) );   /* 错峰①:最先醒,先把锁拿到手 */

    vSafePrintf( "[LOWHOLD] t=%lu ms 准备拿锁\n",
                 ( unsigned long ) ( xTaskGetTickCount() * portTICK_PERIOD_MS ) );

    ( void ) xSemaphoreTake( xInvSem, portMAX_DELAY );
    tTake = xTaskGetTickCount();                       /* 只记时刻,不当场打印:持锁时抢日志锁=两把锁嵌套 */

    for( i = 0; i < ( W3_HOLD_WORK_MS / 100U ); i++ )   /* 30 段 × 100 ms = 3 s 长活 */
    {
        vTaskDelay( pdMS_TO_TICKS( 100 ) );            /* 干一段、等一次外设 */
    }

    tGive = xTaskGetTickCount();
    ( void ) xSemaphoreGive( xInvSem );                /* 还锁:等锁者此刻才会醒 */

    vSafePrintf( "[LOWHOLD] 拿锁 t=%lu ms -> 还锁 t=%lu ms,共持锁 %lu ms\n",
                 ( unsigned long ) ( tTake * portTICK_PERIOD_MS ),
                 ( unsigned long ) ( tGive * portTICK_PERIOD_MS ),
                 ( unsigned long ) ( ( tGive - tTake ) * portTICK_PERIOD_MS ) );

    for( ; ; )                                          /* 干完挂住;任务不删,句柄才一直有效 */
    {
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }
}

/* 插队者:优先级居中,只要 CPU、全程不碰锁 —— 反转的发动机 */
static void prvMidHogTask( void * pvParameters )
{
    TickType_t tStart;
    uint32_t   ulSpin = 0;

    ( void ) pvParameters;

    vTaskDelay( pdMS_TO_TICKS( W3_HOG_START_MS ) );    /* 错峰③:最后醒,一就绪就抢占持锁者 */

    vSafePrintf( "[MIDHOG ] t=%lu ms 开始占 CPU %u ms(全程不碰锁)\n",
                 ( unsigned long ) ( xTaskGetTickCount() * portTICK_PERIOD_MS ),
                 ( unsigned int ) W3_HOG_BUSY_MS );

    tStart = xTaskGetTickCount();
    while( ( xTaskGetTickCount() - tStart ) < pdMS_TO_TICKS( W3_HOG_BUSY_MS ) )
    {
        ulSpin++;                                      /* 纯计算:循环里没有 vTaskDelay、没有打印 */
    }

    vSafePrintf( "[MIDHOG ] 占完 CPU:自旋 %lu 次,让出\n", ( unsigned long ) ulSpin );

    for( ; ; )
    {
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }
}

/* 等锁者:优先级最高 —— 反转的受害者,它的等待时间就是实验读数 */
static void prvHighWaitTask( void * pvParameters )
{
    TickType_t t0;
    TickType_t t1;

    ( void ) pvParameters;

    vTaskDelay( pdMS_TO_TICKS( W3_WAIT_START_MS ) );   /* 错峰②:第二个醒,来撞墙 */

    t0 = xTaskGetTickCount();
    vSafePrintf( "[HIGHWAIT] t=%lu ms 开始等锁\n",
                 ( unsigned long ) ( t0 * portTICK_PERIOD_MS ) );

    ( void ) xSemaphoreTake( xInvSem, portMAX_DELAY ); /* 拿不到就睡(B 态),不占 CPU */

    t1 = xTaskGetTickCount();
    vTaskDelay( pdMS_TO_TICKS( 50 ) );                 /* 拿到锁后干点短活 */
    ( void ) xSemaphoreGive( xInvSem );

    vSafePrintf( "[HIGHWAIT] 拿到锁 t=%lu ms,一共等了 %lu ms  <<< 实验读数\n",
                 ( unsigned long ) ( t1 * portTICK_PERIOD_MS ),
                 ( unsigned long ) ( ( t1 - t0 ) * portTICK_PERIOD_MS ) );

    for( ; ; )
    {
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }
}


/* ------------------------------------------------------------------ */
/* FreeRTOS 钩子与 configASSERT                                         */
/* ------------------------------------------------------------------ */
void vAssertCalled( unsigned long ulLine, const char * const pcFileName )
{
    /* 仿真环境:打印后直接退出,方便定位;真机上通常做软复位并记录原因 */
    fprintf( stderr, "\n[ASSERT] failed at line %lu, file %s\n", ulLine, pcFileName );
    fflush( stderr );
    exit( 1 );
}

void vApplicationIdleHook( void )
{
    /* 空闲任务里不做任何可能阻塞的事 */
}

void vApplicationDaemonTaskStartupHook( void )
{
    /* 定时器守护任务第一次运行时调用一次(当前无需初始化) */
}

/* configSUPPORT_STATIC_ALLOCATION=1 时内核要求用户静态提供
 * 空闲任务与定时器任务的 TCB/栈内存 */
static StaticTask_t xIdleTaskTCB;
static StackType_t  uxIdleTaskStack[ configMINIMAL_STACK_SIZE ];
static StaticTask_t xTimerTaskTCB;
static StackType_t  uxTimerTaskStack[ configTIMER_TASK_STACK_DEPTH ];

void vApplicationGetIdleTaskMemory( StaticTask_t ** ppxIdleTaskTCBBuffer,
                                    StackType_t ** ppxIdleTaskStackBuffer,
                                    uint32_t * pulIdleTaskStackSize )
{
    *ppxIdleTaskTCBBuffer   = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *pulIdleTaskStackSize   = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory( StaticTask_t ** ppxTimerTaskTCBBuffer,
                                     StackType_t ** ppxTimerTaskStackBuffer,
                                     uint32_t * pulTimerTaskStackSize )
{
    *ppxTimerTaskTCBBuffer   = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *pulTimerTaskStackSize   = configTIMER_TASK_STACK_DEPTH;
}

void vApplicationTickHook( void )
{
}

void vApplicationMallocFailedHook( void )
{
    fprintf( stderr, "\n[MALLOC] heap exhausted, task creation failed!\n" );
    fflush( stderr );
    exit( 1 );
}

void vApplicationStackOverflowHook( TaskHandle_t xTask, char * pcTaskName )
{
    /* 中断/钩子里不能用可能阻塞的 API,直接 fprintf */
    ( void ) xTask;
    fprintf( stderr, "\n[STACK] overflow in task '%s'!\n", pcTaskName );
    fflush( stderr );
    exit( 1 );
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */
int main( void )
{
    /* 源码字符串是 UTF-8,而中文版 Windows 控制台默认是 GBK(936)代码页,
     * 不切换就会显示乱码。重定向到文件时不受影响,文件内容仍是 UTF-8。*/
    SetConsoleOutputCP( CP_UTF8 );

    srand( ( unsigned ) time( NULL ) );

    xSampleQueue  = xQueueCreate( QUEUE_LEN, sizeof( Sample_t ) );
    xAlarmQueue   = xQueueCreate( QUEUE_LEN, sizeof( AlarmEvent_t ) );
    xLatestMutex  = xSemaphoreCreateMutex();
    xLogMutex     = xSemaphoreCreateMutex();
    xIsrSem       = xSemaphoreCreateBinary(); /* 二值信号量:创建时为「空」= 暂无事件 */
    xEvtSem       = xSemaphoreCreateCounting( EVT_BURST_N, 0 ); /* 计数:上限 3,初始 0 */

    if( ( xSampleQueue == NULL ) || ( xAlarmQueue == NULL ) ||
        ( xLatestMutex == NULL ) || ( xLogMutex == NULL ) ||
        ( xIsrSem == NULL ) || ( xEvtSem == NULL ) )
    {
        fprintf( stderr, "create queue/mutex failed\n" );
        return 1;
    }

    /* 优先级:采集(3) > 告警/通信(2) > 显示/按键/统计(1)
     * 栈大小:仿真环境下真栈是 Windows 线程栈,这里给的是内核记账值,取大些无妨 */
#if ( W3_ISOLATE == 0 )
    xTaskCreate( prvSensorTask, "Sensor",  1024, NULL, 3, &xSensorHandle );
    xTaskCreate( prvAlarmTask,  "Alarm",   1024, NULL, 2, &xAlarmHandle );
    xTaskCreate( prvCommTask,   "Comm",    1024, NULL, 2, &xCommHandle );
    xTaskCreate( prvDisplayTask,"Display", 1024, NULL, 1, &xDisplayHandle );
    xTaskCreate( prvKeySimTask, "KeySim",  1024, NULL, 1, NULL );
    xTaskCreate( prvIsrSimTask, "IsrSim",  1024, NULL, 4, &xIsrSimHandle ); /* 中断源:最高优先级 */
    xTaskCreate( prvWakeTask,   "Wake",    1024, NULL, 1, &xWakeHandle );
    xTaskCreate( prvEvtSrcTask, "EvtSrc",  1024, NULL, 2, NULL ); /* 事件源:波次生产者 */
    xTaskCreate( prvEvtConsTask,"EvtCons", 1024, NULL, 1, NULL ); /* 消费者:慢速处理 */
#else
    vSafePrintf( "[MAIN  ] W3_ISOLATE=1 实验模式:只跑 Stats + 反转三角\n" );

    xInvSem = xSemaphoreCreateMutex();   /* ← 实验 3-B 只改这一行:xSemaphoreCreateMutex()   binary → mutex*/
    //( void ) xSemaphoreGive( xInvSem );   /* ★上电初始化:二值信号量创建时是空的,不补这句 Take 会永远阻塞 */
    if( xInvSem == NULL )
    {
        fprintf( stderr, "create xInvSem failed\n" );
        return 1;
    }

    xTaskCreate( prvLowHoldTask,  "LowHold",  1024, NULL, 1, &xLowHoldHandle );
    xTaskCreate( prvMidHogTask,   "MidHog",   1024, NULL, 2, &xMidHogHandle );
    xTaskCreate( prvHighWaitTask, "HighWait", 1024, NULL, 3, &xHighWaitHandle );
#endif
    xTaskCreate( prvStatsTask,  "Stats",   1024, NULL, W3_STATS_PRIO, &xStatsHandle );


    vSafePrintf( "[MAIN  ] starting FreeRTOS scheduler, 10 tasks: "
                 "Sensor/Alarm/Comm/Display/KeySim/Stats/IsrSim/Wake/EvtSrc/EvtCons\n" );

    vTaskStartScheduler();

    /* 走到这里说明堆不够创建空闲任务/定时器任务 */
    fprintf( stderr, "scheduler returned unexpectedly\n" );
    return 1;
}
