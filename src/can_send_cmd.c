#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#include "can_protocol.h"
#include "crc8.h"

int main(int argc, char *argv[]) {
    // 1. 严格对齐最新的控制协议枚举说明
    if (argc < 4) {
        printf("用法: %s <mode: 1-AUTO / 2-MANUAL / 3-SAFE / 4-CLEAR> <value: 0-100> <counter: 0~15>\n", argv[0]);
        printf("示例: %s 2 50 1  (代表: MANUAL模式，风扇占空比50%%，计数器1)\n", argv[0]);
        return 1;
    }

    uint8_t mode = (uint8_t)atoi(argv[1]);
    uint16_t val = (uint16_t)atoi(argv[2]);
    uint8_t cnt  = (uint8_t)atoi(argv[3]);

    // 2. 增加输入参数的安全拦截：拒绝 1~4 之外的无效模式
    if (mode < 1 || mode > 4) {
        printf("[错误] 非法的模式值！只能输入 1(AUTO), 2(MANUAL), 3(SAFE), 4(CLEAR)\n");
        return 1;
    }

    int s;
    struct sockaddr_can addr;
    struct ifreq ifr;

    if ((s = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
        perror("Socket creation failed");
        return 1;
    }

    strcpy(ifr.ifr_name, "vcan0");
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        perror("ioctl failed (请检查 can0 是否存在)");
        close(s);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Bind failed");
        close(s);
        return 1;
    }

    struct can_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.can_id = 0x200;
    frame.can_dlc = 8;

    // 组装 0x200 控制指令
    ControlCmd_t cmd = {0};
    cmd.target_mode = mode;  // 这里传入的 1,2,3,4 刚好对应 STM32 强制赋初值的枚举
    cmd.target_value = val;
    cmd.rx_counter = cnt & 0x0F;

    Pack_Msg_0x200(&cmd, frame.data);

    if (write(s, &frame, sizeof(struct can_frame)) != sizeof(struct can_frame)) {
        perror("Write failed");
        close(s);
        return 1;
    }

    // 3. 打印实际发送的底层二进制数据，修正显示与实际发包不一致的问题[cite: 19]
    // 假设你们打包函数将 Mode 放在了 data[0]
    printf("[TX 0x200] 实际发往总线 -> Mode: %d | Val: %d | 物理Counter: %d | CRC: 0x%02X\n",
           frame.data[0], val, (frame.data[6] & 0x0F), frame.data[7]);

    close(s);
    return 0;
}