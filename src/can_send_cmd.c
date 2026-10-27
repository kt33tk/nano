#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <poll.h>

#include "can_protocol.h"
#include "crc8.h"

#define COMMAND_CAN_ID      0x200U
#define ACK_CAN_ID          0x280U
#define ACK_FRAME_TYPE      0x02U
#define ACK_TIMEOUT_MS      1000

/* [修改1] 使用单调时钟计算ACK总超时，系统时间变化不会影响等待 */
static int64_t Monotonic_Milliseconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }

    return ((int64_t)ts.tv_sec * 1000LL) + ((int64_t)ts.tv_nsec / 1000000LL);
}

/* [修改2] 使用strtol严格检查整数，不再用atoi把非法文字默认为0 */
static int Parse_Integer(const char *text, long minimum, long maximum, long *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);

    if ((errno != 0) || (end == text) || (*end != '\0') ||
        (parsed < minimum) || (parsed > maximum)) {
        return -1;
    }

    *value = parsed;
    return 0;
}

static void Print_Usage(const char *program)
{
    printf("用法: %s [--if can0|vcan0] <mode> <value> <counter>\n", program);
    printf("  mode    : 1=AUTO, 2=MANUAL, 3=SAFE, 4=CLEAR\n");
    printf("  value   : 0~100\n");
    printf("  counter : 0~15\n");
    printf("示例: %s --if can0 2 50 1\n", program);
    printf("      %s --if vcan0 2 50 1\n", program);
}

/* [修改3] 支持 --if；不写时默认使用can0 */
static int Parse_Arguments(int argc,
                           char *argv[],
                           const char **ifname,
                           uint8_t *mode,
                           uint16_t *value,
                           uint8_t *counter)
{
    int value_index = 1;
    long parsed_mode;
    long parsed_value;
    long parsed_counter;

    *ifname = "can0";

    if ((argc >= 2) &&
        ((strcmp(argv[1], "-h") == 0) || (strcmp(argv[1], "--help") == 0))) {
        Print_Usage(argv[0]);
        return 1;
    }

    if ((argc >= 2) && (strcmp(argv[1], "--if") == 0)) {
        if (argc < 3) {
            fprintf(stderr, "[错误] --if 后面缺少接口名\n");
            return -1;
        }

        *ifname = argv[2];
        value_index = 3;
    }

    if ((argc - value_index) != 3) {
        Print_Usage(argv[0]);
        return -1;
    }

    if (strlen(*ifname) >= IFNAMSIZ) {
        fprintf(stderr, "[错误] CAN接口名过长: %s\n", *ifname);
        return -1;
    }

    if (Parse_Integer(argv[value_index], 1L, 4L, &parsed_mode) != 0) {
        fprintf(stderr, "[错误] mode必须是1~4之间的整数\n");
        return -1;
    }

    if (Parse_Integer(argv[value_index + 1], 0L, 100L, &parsed_value) != 0) {
        fprintf(stderr, "[错误] value必须是0~100之间的整数\n");
        return -1;
    }

    if (Parse_Integer(argv[value_index + 2], 0L, 15L, &parsed_counter) != 0) {
        fprintf(stderr, "[错误] counter必须是0~15之间的整数\n");
        return -1;
    }

    *mode = (uint8_t)parsed_mode;
    *value = (uint16_t)parsed_value;
    *counter = (uint8_t)parsed_counter;
    return 0;
}

/* [修改4] 打印真正写到CAN总线上的8个字节 */
static void Print_Frame_Data(const struct can_frame *frame)
{
    printf("[TX 0x%03X] DLC=%u Data=",
           (unsigned int)(frame->can_id & CAN_SFF_MASK),
           (unsigned int)frame->can_dlc);

    for (uint8_t i = 0U; i < frame->can_dlc; i++) {
        printf("%s%02X", (i == 0U) ? "" : " ", frame->data[i]);
    }

    printf("\n");
}

/* [修改5] 发送后等待ID、DLC、CRC和counter全部匹配的0x280 ACK */
static int Wait_For_Matching_ACK(int socket_fd, uint8_t sent_counter)
{
    int64_t start_ms = Monotonic_Milliseconds();

    if (start_ms < 0) {
        perror("[错误] clock_gettime失败");
        return -1;
    }

    for (;;) {
        int64_t now_ms = Monotonic_Milliseconds();
        if (now_ms < 0) {
            perror("[错误] clock_gettime失败");
            return -1;
        }

        int64_t elapsed_ms = now_ms - start_ms;
        int remaining_ms = ACK_TIMEOUT_MS - (int)elapsed_ms;

        if (remaining_ms <= 0) {
            fprintf(stderr,
                    "[超时] %d ms内没有收到匹配Counter=%u的0x280 ACK\n",
                    ACK_TIMEOUT_MS, (unsigned int)sent_counter);
            return 0;
        }

        struct pollfd pfd;
        pfd.fd = socket_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int poll_result = poll(&pfd, 1, remaining_ms);

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("[错误] 等待ACK时poll失败");
            return -1;
        }

        if (poll_result == 0) {
            fprintf(stderr,
                    "[超时] %d ms内没有收到匹配Counter=%u的0x280 ACK\n",
                    ACK_TIMEOUT_MS, (unsigned int)sent_counter);
            return 0;
        }

        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            fprintf(stderr, "[错误] 等待ACK时CAN接口异常，revents=0x%X\n",
                    pfd.revents);
            return -1;
        }

        if ((pfd.revents & POLLIN) != 0) {
            struct can_frame ack_frame;
            ssize_t nbytes = read(socket_fd, &ack_frame, sizeof(ack_frame));

            if (nbytes < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("[错误] 读取ACK失败");
                return -1;
            }

            if (nbytes != (ssize_t)sizeof(struct can_frame)) {
                fprintf(stderr, "[警告] 收到不完整CAN帧，已忽略\n");
                continue;
            }

            uint32_t ack_id = ack_frame.can_id & CAN_SFF_MASK;
            if (ack_id != ACK_CAN_ID) {
                continue;
            }

            /* [修改6-关键] 必须先检查DLC，再读取data[7] */
            if (ack_frame.can_dlc != 8U) {
                fprintf(stderr, "[警告] 0x280 ACK的DLC=%u，不是8，已忽略\n",
                        (unsigned int)ack_frame.can_dlc);
                continue;
            }

            uint8_t received_crc = ack_frame.data[7];
            uint8_t calculated_crc = Calculate_CRC8_J1850(ack_frame.data, 7U);

            if (received_crc != calculated_crc) {
                fprintf(stderr,
                        "[警告] 0x280 ACK CRC错误: RX=0x%02X CALC=0x%02X，已忽略\n",
                        received_crc, calculated_crc);
                continue;
            }

            uint8_t ack_type = ack_frame.data[0];
            uint8_t ack_counter = ack_frame.data[1] & 0x0FU;
            uint8_t ack_status = ack_frame.data[2];

            if (ack_type != ACK_FRAME_TYPE) {
                fprintf(stderr, "[警告] 0x280类型0x%02X不是命令ACK，已忽略\n",
                        ack_type);
                continue;
            }

            if (ack_counter != sent_counter) {
                fprintf(stderr,
                        "[提示] 收到旧ACK Counter=%u，当前等待Counter=%u，继续等待\n",
                        (unsigned int)ack_counter,
                        (unsigned int)sent_counter);
                continue;
            }

            printf("[RX 0x280 ACK] Type=0x%02X | Counter=%u | Status=%u | CRC=0x%02X\n",
                   ack_type,
                   (unsigned int)ack_counter,
                   (unsigned int)ack_status,
                   received_crc);

            if (ack_status == 0U) {
                printf("[成功] STM32已接受并处理本次命令\n");
                return 1;
            }

            fprintf(stderr, "[拒绝] STM32返回非零状态码: %u\n",
                    (unsigned int)ack_status);
            return -2;
        }
    }
}

int main(int argc, char *argv[])
{
    const char *ifname = NULL;
    uint8_t mode = 0U;
    uint16_t value = 0U;
    uint8_t counter = 0U;

    int parse_result = Parse_Arguments(argc, argv, &ifname,
                                       &mode, &value, &counter);
    if (parse_result != 0) {
        return (parse_result > 0) ? 0 : 1;
    }

    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) {
        perror("[错误] Socket creation failed");
        return 1;
    }

    /* [修改7] 只接收0x280，避免0x180状态帧干扰ACK等待 */
    struct can_filter ack_filter;
    ack_filter.can_id = ACK_CAN_ID;
    ack_filter.can_mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG;

    if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER,
                   &ack_filter, sizeof(ack_filter)) < 0) {
        perror("[错误] 设置0x280接收过滤器失败");
        close(s);
        return 1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1U);
    ifr.ifr_name[IFNAMSIZ - 1U] = '\0';

    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "[错误] 找不到CAN接口 %s，请执行: ip link show %s\n",
                ifname, ifname);
        perror("ioctl(SIOCGIFINDEX)");
        close(s);
        return 1;
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[错误] Bind failed");
        close(s);
        return 1;
    }

    struct can_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.can_id = COMMAND_CAN_ID;
    frame.can_dlc = 8U;

    ControlCmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.target_mode = mode;
    cmd.target_value = value;
    cmd.rx_counter = counter;

    Pack_Msg_0x200(&cmd, frame.data);

    /*
     * [修改8-关键] 明确保证物理counter位于data[6]低4位。
     * 因为data[0]~data[6]发生了变化，CRC必须最后重新计算。
     */
    frame.data[6] = counter & 0x0FU;
    frame.data[7] = Calculate_CRC8_J1850(frame.data, 7U);

    ssize_t written = write(s, &frame, sizeof(frame));
    if (written != (ssize_t)sizeof(frame)) {
        if (written < 0) {
            perror("[错误] CAN write failed");
        } else {
            fprintf(stderr, "[错误] CAN短写: %zd/%zu 字节\n",
                    written, sizeof(frame));
        }
        close(s);
        return 1;
    }

    printf("[已发送] 接口=%s | Mode=%u | Value=%u | Counter=%u | CRC=0x%02X\n",
           ifname,
           (unsigned int)mode,
           (unsigned int)value,
           (unsigned int)counter,
           frame.data[7]);
    Print_Frame_Data(&frame);

    int ack_result = Wait_For_Matching_ACK(s, counter);
    close(s);

    if (ack_result == 1) {
        return 0;
    }

    return 2;
}
