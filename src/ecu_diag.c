#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#define REQ_ID 0x7E0U
#define RSP_ID 0x7E8U
#define MAX_UDS 48U

static int open_can(const char *ifname)
{
    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    struct ifreq ifr = {0};
    struct sockaddr_can addr = {0};
    struct can_filter filter = {
        .can_id = RSP_ID,
        .can_mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG
    };

    if (fd < 0) return -1;

    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER,
                   &filter, sizeof(filter)) < 0) goto fail;

    if (strlen(ifname) >= IFNAMSIZ) goto fail;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) goto fail;

    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto fail;

    return fd;

fail:
    perror("SocketCAN");
    close(fd);
    return -1;
}

static int send_frame(int fd, const uint8_t data[8])
{
    struct can_frame frame = {0};
    frame.can_id = REQ_ID;
    frame.can_dlc = 8;
    memcpy(frame.data, data, 8);
    return write(fd, &frame, sizeof(frame)) == sizeof(frame) ? 0 : -1;
}

/* 只收0x7E8；每一帧最多等待2秒。 */
static int receive_frame(int fd, struct can_frame *frame)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};

    if (poll(&pfd, 1, 2000) <= 0) return -1;
    if (read(fd, frame, sizeof(*frame)) != sizeof(*frame)) return -1;
    if ((frame->can_id & CAN_SFF_MASK) != RSP_ID ||
        frame->can_dlc != 8) return -1;
    return 0;
}

static int transact(int fd, const uint8_t *request, uint8_t req_len,
                    uint8_t response[MAX_UDS])
{
    uint8_t tx[8] = {0};
    struct can_frame rx;
    unsigned total, copied, sequence;

    tx[0] = req_len;
    memcpy(&tx[1], request, req_len);
    if (send_frame(fd, tx) < 0 || receive_frame(fd, &rx) < 0)
        return -1;

    if ((rx.data[0] >> 4) == 0) {
        total = rx.data[0] & 0x0F;
        if (total == 0 || total > 7) return -1;
        memcpy(response, &rx.data[1], total);
        return (int)total;
    }

    if ((rx.data[0] >> 4) != 1) return -1;
    total = ((unsigned)(rx.data[0] & 0x0F) << 8) | rx.data[1];
    if (total <= 7 || total > MAX_UDS) return -1;

    memcpy(response, &rx.data[2], 6);
    copied = 6;
    sequence = 1;

    memset(tx, 0, sizeof(tx));
    tx[0] = 0x30;              /* Flow Control: Continue */
    /* tx[1]=0: block size无限制；tx[2]=0: STmin为0 */
    if (send_frame(fd, tx) < 0) return -1;

    while (copied < total) {
        unsigned count;

        if (receive_frame(fd, &rx) < 0 ||
            (rx.data[0] >> 4) != 2 ||
            (rx.data[0] & 0x0F) != (sequence & 0x0F))
            return -1;

        count = total - copied;
        if (count > 7) count = 7;
        memcpy(&response[copied], &rx.data[1], count);
        copied += count;
        sequence++;
    }

    return (int)total;
}

static uint32_t read_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

int main(int argc, char **argv)
{
    const char *ifname = "can0";
    const char *command;
    uint8_t request[7] = {0};
    uint8_t response[MAX_UDS] = {0};
    uint8_t req_len = 0;
    int fd, len;

    /* 用法：ecu_diag [--if can0] <命令> [DTC十六进制] */
    if (argc >= 3 && strcmp(argv[1], "--if") == 0) {
        ifname = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc < 2) {
        fprintf(stderr, "用法: %s [--if can0] "
                "version|temp|state|can|dtc|freeze HEX_DTC|clear|session|tester\n",
                argv[0]);
        return 1;
    }

    command = argv[1];
    if      (!strcmp(command, "version")) { request[0]=0x22; request[1]=0xF1; request[2]=0x80; req_len=3; }
    else if (!strcmp(command, "temp"))    { request[0]=0x22; request[1]=0xF1; request[2]=0x81; req_len=3; }
    else if (!strcmp(command, "state"))   { request[0]=0x22; request[1]=0xF1; request[2]=0x82; req_len=3; }
    else if (!strcmp(command, "can"))     { request[0]=0x22; request[1]=0xF1; request[2]=0x83; req_len=3; }
    else if (!strcmp(command, "dtc"))     { request[0]=0x19; request[1]=0x02; request[2]=0xFF; req_len=3; }
    else if (!strcmp(command, "clear"))   { request[0]=0x14; request[1]=0xFF; request[2]=0xFF; request[3]=0xFF; req_len=4; }
    else if (!strcmp(command, "session")) { request[0]=0x10; request[1]=0x01; req_len=2; }
    else if (!strcmp(command, "tester"))  { request[0]=0x3E; request[1]=0x00; req_len=2; }
    else if (!strcmp(command, "freeze") && argc == 3) {
        char *end;
        unsigned long code = strtoul(argv[2], &end, 16);
        if (*end != '\0' || code > 0xFFFFUL) {
            fprintf(stderr, "DTC示例: 1001\n");
            return 1;
        }
        request[0]=0x19; request[1]=0x04; request[2]=0x00;
        request[3]=(uint8_t)(code >> 8);
        request[4]=(uint8_t)code;
        request[5]=0xFF;
        req_len=6;
    } else {
        fprintf(stderr, "未知命令或参数错误\n");
        return 1;
    }

    fd = open_can(ifname);
    if (fd < 0) return 1;
    len = transact(fd, request, req_len, response);
    close(fd);

    if (len < 0) {
        fprintf(stderr, "诊断响应超时或ISO-TP帧不完整\n");
        return 2;
    }
    if (len == 3 && response[0] == 0x7F) {
        printf("NRC: SID=%02X, 原因=%02X\n", response[1], response[2]);
        return 3;
    }

    if (!strcmp(command, "version") && len == 6)
        printf("ECU软件版本: %u.%u.%u\n",
               response[3], response[4], response[5]);
    else if (!strcmp(command, "temp") && len == 6) {
        int16_t t = (int16_t)((response[3] << 8) | response[4]);
        printf("温度: %.1f C, 有效=%u\n", t / 10.0, response[5]);
    }
    else if (!strcmp(command, "state") && len == 7)
        printf("模式=%u 故障位=0x%02X 输出=%u 继电器=%u\n",
               response[3], response[4], response[5], response[6]);
    else if (!strcmp(command, "can") && len == 19)
        printf("TX成功=%u TX失败=%u RX=%u BusOff=%u\n",
               read_u32(&response[3]), read_u32(&response[7]),
               read_u32(&response[11]), read_u32(&response[15]));
    else if (!strcmp(command, "dtc") && len >= 3 && (len - 3) % 4 == 0) {
        for (int i = 3; i < len; i += 4)
            printf("DTC=%02X%02X%02X 状态=%02X\n",
                   response[i], response[i+1], response[i+2], response[i+3]);
        if (len == 3) puts("当前没有活动DTC");
    }
    else if (!strcmp(command, "freeze") && len == 16) {
        int16_t t = (int16_t)((response[7] << 8) | response[8]);
        printf("DTC=%02X%02X%02X 温度=%.1f C 模式=%u 输出=%u 继电器=%u CAN错误=0x%08X\n",
               response[2], response[3], response[4], t / 10.0,
               response[9], response[10], response[11], read_u32(&response[12]));
    }
    else {
        printf("响应:");
        for (int i = 0; i < len; i++) printf(" %02X", response[i]);
        putchar('\n');
    }

    return 0;
}