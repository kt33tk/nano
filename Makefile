CC = gcc
# -Iinclude 告诉编译器去 include 文件夹下找 .h 头文件
CFLAGS = -Wall -Iinclude

# 声明公共的协议与校验源文件（带上 src/ 路径）
COMMON_SRC = src/can_protocol.c src/crc8.c
COMMON_OBJ = $(COMMON_SRC:.c=.o)

# 默认目标：一次编译同时生成两个可执行程序
all: can_monitor can_send_cmd ecu_diag

ecu_diag: src/ecu_diag.o
	$(CC) -o $@ $^

# 链接生成监听工具 can_monitor
can_monitor: src/can_monitor.o $(COMMON_OBJ)
	$(CC) -o $@ $^

# 链接生成指令发送工具 can_send_cmd
can_send_cmd: src/can_send_cmd.o $(COMMON_OBJ)
	$(CC) -o $@ $^

# 将 src/ 下的 .c 编译为对应的 .o
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# 清理编译产物
clean:
	rm -f src/*.o can_monitor can_send_cmd ecu_diag