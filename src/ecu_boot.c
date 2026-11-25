#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#define REQUEST_ID          0x7E0U
#define RESPONSE_ID         0x7E8U
#define MAX_PAYLOAD         256U
#define FRAME_TIMEOUT_MS    1000
#define RESPONSE_TIMEOUT_MS 10000
#define RX_BLOCK_SIZE       4U
#define RX_STMIN_MS         5U

static int open_can(const char *ifname)
{
    unsigned ifindex = if_nametoindex(ifname);
    if (ifindex == 0U)
        return -1;

    int fd = socket(AF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0)
        return -1;

    struct sockaddr_can addr = {
        .can_family = AF_CAN,
        .can_ifindex = (int)ifindex
    };
    struct can_filter filter = {
        .can_id = RESPONSE_ID,
        .can_mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG
    };

    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER,
                   &filter, sizeof(filter)) < 0 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    return fd;
}

static int send_frame(int fd, const uint8_t data[8])
{
    struct can_frame frame = {
        .can_id = REQUEST_ID,
        .can_dlc = 8
    };
    memcpy(frame.data, data, 8);

    if (write(fd, &frame, sizeof(frame)) != (ssize_t)sizeof(frame)) {
        perror("CAN write");
        return -1;
    }

    return 0;
}

static int receive_frame(int fd, uint8_t data[8], int timeout_ms)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    struct can_frame frame;
    int ready = poll(&pfd, 1, timeout_ms);

    if (ready <= 0 || !(pfd.revents & POLLIN)) {
        fprintf(stderr, "CAN receive timeout/error (%d ms)\n", timeout_ms);
        return -1;
    }

    if (read(fd, &frame, sizeof(frame)) != (ssize_t)sizeof(frame) ||
        frame.can_dlc != 8U) {
        fprintf(stderr, "Invalid CAN frame or DLC\n");
        return -1;
    }

    memcpy(data, frame.data, 8);
    return 0;
}

/* 小于等于 7 字节发 SF，否则发 FF，并按接收方的 FC 分批发送 CF。 */
static int isotp_send(int fd, const uint8_t *data, size_t length)
{
    uint8_t frame[8] = {0};
    uint8_t fc[8];
    size_t offset = 6U;
    unsigned sequence = 1U;

    if (length <= 7U) {
        frame[0] = (uint8_t)length;
        memcpy(frame + 1, data, length);
        return send_frame(fd, frame);
    }

    frame[0] = (uint8_t)(0x10U | (length >> 8));
    frame[1] = (uint8_t)length;
    memcpy(frame + 2, data, 6);
    if (send_frame(fd, frame) < 0)
        return -1;

    while (offset < length) {
        if (receive_frame(fd, fc, FRAME_TIMEOUT_MS) < 0)
            return -1;

        /* 当前 Bootloader 发 CTS 0x30，超出容量时发 0x32。 */
        if (fc[0] != 0x30U) {
            fprintf(stderr, "Expected FC CTS, got %02X\n",
                    (unsigned)fc[0]);
            return -1;
        }

        unsigned delay_us;
        if (fc[2] <= 0x7FU)
            delay_us = (unsigned)fc[2] * 1000U;
        else if (fc[2] >= 0xF1U && fc[2] <= 0xF9U)
            delay_us = (unsigned)(fc[2] - 0xF0U) * 100U;
        else
            return -1;

        for (unsigned sent = 0U;
             offset < length && (fc[1] == 0U || sent < fc[1]);
             ++sent) {
            size_t count = length - offset;
            if (count > 7U)
                count = 7U;

            memset(frame, 0, sizeof(frame));
            frame[0] = (uint8_t)(0x20U | sequence);
            memcpy(frame + 1, data + offset, count);

            if (delay_us != 0U && usleep(delay_us) < 0)
                return -1;
            if (send_frame(fd, frame) < 0)
                return -1;

            offset += count;
            sequence = (sequence + 1U) & 0x0FU;
        }
    }

    return 0;
}

/* 收到 FF 后发送 FC，每收满一批且还有剩余数据时再发送 FC。 */
static int isotp_receive(int fd, uint8_t data[MAX_PAYLOAD])
{
    const uint8_t fc[8] = {0x30U, RX_BLOCK_SIZE, RX_STMIN_MS};
    uint8_t frame[8];
    size_t offset = 6U;
    unsigned sequence = 1U;
    unsigned received_in_block = 0U;

    if (receive_frame(fd, frame, RESPONSE_TIMEOUT_MS) < 0)
        return -1;

    if ((frame[0] >> 4) == 0U) {
        unsigned length = frame[0] & 0x0FU;
        if (length == 0U || length > 7U)
            return -1;
        memcpy(data, frame + 1, length);
        return (int)length;
    }

    if ((frame[0] >> 4) != 1U)
        return -1;

    size_t length = ((size_t)(frame[0] & 0x0FU) << 8) | frame[1];
    if (length <= 7U || length > MAX_PAYLOAD)
        return -1;

    memcpy(data, frame + 2, 6);
    if (send_frame(fd, fc) < 0)
        return -1;

    while (offset < length) {
        if (receive_frame(fd, frame, FRAME_TIMEOUT_MS) < 0)
            return -1;

        if (frame[0] != (0x20U | sequence)) {
            fprintf(stderr, "Unexpected CF PCI: %02X\n",
                    (unsigned)frame[0]);
            return -1;
        }

        size_t count = length - offset;
        if (count > 7U)
            count = 7U;

        memcpy(data + offset, frame + 1, count);
        offset += count;
        sequence = (sequence + 1U) & 0x0FU;

        if (++received_in_block == RX_BLOCK_SIZE && offset < length) {
            if (send_frame(fd, fc) < 0)
                return -1;
            received_in_block = 0U;
        }
    }

    return (int)length;
}

int main(int argc, char **argv)
{
    uint8_t request[MAX_PAYLOAD];
    uint8_t response[MAX_PAYLOAD];
    size_t length;
    int echo;

    if (argc < 4) {
        fprintf(stderr, "Usage: %s can0 echo LENGTH | can0 req HEX ...\n",
                argv[0]);
        return 1;
    }

    echo = strcmp(argv[2], "echo") == 0;
    if (echo && argc == 4) {
        char *end;
        unsigned long count = strtoul(argv[3], &end, 10);
        if (end == argv[3] || *end != '\0' ||
            count == 0U || count > MAX_PAYLOAD)
            return 1;

        length = (size_t)count;
        for (size_t i = 0; i < length; ++i)
            request[i] = (uint8_t)i;
    } else if (strcmp(argv[2], "req") == 0 &&
               argc - 3 <= (int)MAX_PAYLOAD) {
        length = (size_t)(argc - 3);
        for (size_t i = 0; i < length; ++i) {
            const char *byte = argv[i + 3];
            if (strlen(byte) != 2U ||
                !isxdigit((unsigned char)byte[0]) ||
                !isxdigit((unsigned char)byte[1])) {
                fprintf(stderr, "Use two hex digits per byte: %s\n", byte);
                return 1;
            }
            request[i] = (uint8_t)strtoul(byte, NULL, 16);
        }
    } else {
        fprintf(stderr, "Invalid command or payload length\n");
        return 1;
    }

    int fd = open_can(argv[1]);
    if (fd < 0) {
        perror("SocketCAN");
        return 1;
    }

    int received = -1;
    if (isotp_send(fd, request, length) == 0)
        received = isotp_receive(fd, response);
    close(fd);

    if (received < 0) {
        fprintf(stderr, "ISO-TP transaction failed\n");
        return 1;
    }

    if (echo) {
        if ((size_t)received != length ||
            memcmp(request, response, length) != 0) {
            fprintf(stderr, "Echo data mismatch\n");
            return 1;
        }
        printf("ECHO OK: %zu bytes\n", length);
    } else {
        for (int i = 0; i < received; ++i)
            printf("%s%02X", i == 0 ? "" : " ", (unsigned)response[i]);
        putchar('\n');
    }

    return 0;
}