# 项目 A 骨架 —— FreeRTOS Windows 仿真版
# 注意:路径不能带空格(必须经由 C:\Users\21954\freertos-lab 这个 junction 构建)

BUILD_DIR := build
CC        := gcc
CFLAGS    := -O0 -g3 -Wall -Wextra -D_WIN32_WINNT=0x0601 -DprojCOVERAGE_TEST=0
LDFLAGS   := -lwinmm

KERNEL_DIR := ../Source
INCLUDES   := -I. -I$(KERNEL_DIR)/include -I$(KERNEL_DIR)/portable/MSVC-MingW

APP_SRCS := main.c Run-time-stats-utils.c

KERNEL_SRCS := \
	$(KERNEL_DIR)/tasks.c \
	$(KERNEL_DIR)/queue.c \
	$(KERNEL_DIR)/list.c \
	$(KERNEL_DIR)/timers.c \
	$(KERNEL_DIR)/event_groups.c \
	$(KERNEL_DIR)/stream_buffer.c \
	$(KERNEL_DIR)/portable/MSVC-MingW/port.c \
	$(KERNEL_DIR)/portable/MemMang/heap_4.c

EXE := $(BUILD_DIR)/env_monitor.exe

# recipe 用哪套语法,取决于谁真正执行:
#   用"语义探测":%COMSPEC% 只有 cmd 会展开,sh 会原样打印。
#   (教训1:cmd 不认识 mkdir -p,会把 -p 当文件夹名建出来)
#   (教训2:不能用 $(SHELL) 判断——它只反映 make 的偏好,不一定是实际执行者)
ifneq (,$(findstring cmd,$(shell echo %COMSPEC%)))
  MKDIR_BUILD := if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)
  RM_BUILD    := if exist $(BUILD_DIR) rmdir /s /q $(BUILD_DIR)
else
  MKDIR_BUILD := mkdir -p $(BUILD_DIR)
  RM_BUILD    := rm -rf $(BUILD_DIR)
endif

all: $(EXE)

$(EXE): $(APP_SRCS) $(KERNEL_SRCS) FreeRTOSConfig.h
	@$(MKDIR_BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(APP_SRCS) $(KERNEL_SRCS) $(LDFLAGS) -lm

run: $(EXE)
	$(EXE)

clean:
	$(RM_BUILD)

.PHONY: all run clean
