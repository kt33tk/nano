#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <poll.h>
#include <time.h>

#include "can_protocol.h"
#include "crc8.h"

/* 统计变量 */
static uint32_t total_frames = 0U;
static uint32_t crc_error_cnt = 0U;
static uint32_t dlc_error_cnt = 0U;       /* [修改1] 增加 DLC 错误计数 */
static uint32_t counter_jump_cnt = 0U;
static uint32_t duplicate_frame_cnt = 0U;
static int last_rx_counter = -1;

/* [修改2] 支持 ./can_monitor --if can0 和 ./can_monitor --if vcan0 */
static int Parse_Interface(int argc, char *argv[], const char **ifname)
{
    *ifname = "can0";

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--if") == 0) && ((i + 1) < argc)) {
            *ifname = argv[++i];
        } else if ((strcmp(argv[i], "-h") == 0) ||
                   (strcmp(argv[i], "--help") == 0)) {
            printf("用法: %s [--if can0|vcan0]\n", argv[0]);
            return 1;
        } else {
            fprintf(stderr, "[错误] 未知参数: %s\n", argv[i]);
            fprintf(stderr, "用法: %s [--if can0|vcan0]\n", argv[0]);
            return -1;
        }
    }

    if (strlen(*ifname) >= IFNAMSIZ) {
        fprintf(stderr, "[错误] CAN接口名过长: %s\n", *ifname);
        return -1;
    }

    return 0;
}

/* [修改3] 集中打印统计信息，并加入 DLC 错误数 */
static void Print_Statistics(void)
{
    printf("[统计] 总帧:%u | DLC错误:%u | CRC错误:%u | 重复:%u | 跳变:%u\n",
           (unsigned int)total_frames,
           (unsigned int)dlc_error_cnt,
           (unsigned int)crc_error_cnt,
           (unsigned int)duplicate_frame_cnt,
           (unsigned int)counter_jump_cnt);
}

int main(int argc, char *argv[])
{
    int s = -1;
    struct sockaddr_can addr;
    struct ifreq ifr;
    const char *ifname = NULL;

    int parse_result = Parse_Interface(argc, argv, &ifname);
    if (parse_result != 0) {
        return (parse_result > 0) ? 0 : 1;
    }

    /* [修改4] 使用 a+，只在空文件中写一次表头 */
    FILE *log_file = fopen("can_log.csv", "a+");
    if (log_file == NULL) {
        perror("[警告] 无法创建或打开 can_log.csv");
    } else {
        if ((fseek(log_file, 0L, SEEK_END) == 0) && (ftell(log_file) == 0L)) {
            fprintf(log_file,
                    "Timestamp,CAN_ID,Temp,Hum,Mode,AppValue,Counter,"
                    "FaultBits,TotalFrames,DLC_Err,CRC_Err,Dup_Err,Jump_Err\n");
            fflush(log_file);
        }
    }

    s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) {
        perror("[错误] Socket creation failed");
        if (log_file != NULL) {
            fclose(log_file);
        }
        return 1;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1U);
    ifr.ifr_name[IFNAMSIZ - 1U] = '\0';

    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "[错误] 找不到CAN接口 %s，请先执行 ip link show %s\n",
                ifname, ifname);
        perror("ioctl(SIOCGIFINDEX)");
        if (log_file != NULL) {
            fclose(log_file);
        }
        close(s);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[错误] Bind failed");
        if (log_file != NULL) {
            fclose(log_file);
        }
        close(s);
        return 1;
    }

    struct pollfd pfd;
    pfd.fd = s;
    pfd.events = POLLIN;
    pfd.revents = 0;

    printf("Listening on %s (ifindex: %d)... 数据将记录到 can_log.csv\n",
           ifname, ifr.ifr_ifindex);
    fflush(stdout);

    for (;;) {
        int ret = poll(&pfd, 1, 1000);

        if (ret < 0) {
            /* [修改5] 被信号中断时继续监听，不误判为CAN故障 */
            if (errno == EINTR) {
                continue;
            }
            perror("[错误] poll 异常");
            break;
        }

        if (ret == 0) {
            printf("[Poll] 1秒超时，当前总线无数据...\n");
            fflush(stdout);
            continue;
        }

        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            printf("[警告] 网卡状态异常 (revents=0x%X)，请检查 %s 是否为 UP\n",
                   pfd.revents, ifname);
            sleep(1);
            continue;
        }

        if ((pfd.revents & POLLIN) != 0) {
            struct can_frame frame;
            ssize_t nbytes = read(s, &frame, sizeof(frame));

            /* [修改6] 必须确认 read() 得到了完整的 struct can_frame */
            if (nbytes < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("[错误] CAN read failed");
                break;
            }

            if (nbytes != (ssize_t)sizeof(struct can_frame)) {
                fprintf(stderr, "[错误] CAN短读取: %zd/%zu 字节，已丢弃\n",
                        nbytes, sizeof(struct can_frame));
                continue;
            }

            total_frames++;
            uint32_t id = frame.can_id & CAN_SFF_MASK;

            printf("\n--- 收到 CAN ID: 0x%03X (DLC: %u) ---\n",
                   (unsigned int)id, (unsigned int)frame.can_dlc);

            /* [修改7-关键] 先检查 DLC，再读取 data[7] 和计算CRC */
            if (frame.can_dlc != 8U) {
                dlc_error_cnt++;
                printf("[错误] 协议帧DLC应为8，实际为%u，已丢弃\n",
                       (unsigned int)frame.can_dlc);
                Print_Statistics();

                if (log_file != NULL) {
                    fprintf(log_file,
                            "%ld,0x%03X,ERROR_DLC,,,,,,%u,%u,%u,%u,%u\n",
                            (long)time(NULL), (unsigned int)id,
                            (unsigned int)total_frames,
                            (unsigned int)dlc_error_cnt,
                            (unsigned int)crc_error_cnt,
                            (unsigned int)duplicate_frame_cnt,
                            (unsigned int)counter_jump_cnt);
                    fflush(log_file);
                }
                continue;
            }

            uint8_t rx_crc = frame.data[7];
            uint8_t calc_crc = Calculate_CRC8_J1850(frame.data, 7U);

            if (rx_crc != calc_crc) {
                crc_error_cnt++;
                printf("[错误] CRC失败: 接收=0x%02X 计算=0x%02X，已丢弃\n",
                       rx_crc, calc_crc);
                Print_Statistics();

                if (log_file != NULL) {
                    fprintf(log_file,
                            "%ld,0x%03X,ERROR_CRC,,,,,,%u,%u,%u,%u,%u\n",
                            (long)time(NULL), (unsigned int)id,
                            (unsigned int)total_frames,
                            (unsigned int)dlc_error_cnt,
                            (unsigned int)crc_error_cnt,
                            (unsigned int)duplicate_frame_cnt,
                            (unsigned int)counter_jump_cnt);
                    fflush(log_file);
                }
                continue;
            }

            printf("[CRC校验] 接收=0x%02X 计算=0x%02X -> 通过\n",
                   rx_crc, calc_crc);

            if (id == 0x180U) {
                SensorMsg_t decoded;
                Unpack_Msg_0x180(frame.data, &decoded);

                int current_counter = (int)(decoded.rx_counter & 0x0FU);

                if (last_rx_counter != -1) {
                    int expected_counter = (last_rx_counter + 1) & 0x0F;

                    if (current_counter == last_rx_counter) {
                        duplicate_frame_cnt++;
                    } else if (current_counter != expected_counter) {
                        counter_jump_cnt++;
                    }
                }
                last_rx_counter = current_counter;

                /* [修改8] 0.1摄氏度整数转成人能读懂的摄氏度 */
                double temperature_c = (double)decoded.temperature / 10.0;

                printf("[0x180 传感器状态] Temp: %.1f C | Hum: %u | "
                       "Mode: %u | AppValue: %u | Valid: %u\n",
                       temperature_c,
                       (unsigned int)decoded.humidity,
                       (unsigned int)decoded.current_mode,
                       (unsigned int)decoded.app_value,
                       (unsigned int)decoded.valid_flag);
                printf("[状态诊断] Counter: %d | Fault Bits: 0x%02X\n",
                       current_counter, (unsigned int)decoded.fault_bits);
                Print_Statistics();

                if (log_file != NULL) {
                    /* [修改9] CSV中的温度也写成24.5，而不是原始整数245 */
                    fprintf(log_file,
                            "%ld,0x180,%.1f,%u,%u,%u,%d,0x%02X,"
                            "%u,%u,%u,%u,%u\n",
                            (long)time(NULL), temperature_c,
                            (unsigned int)decoded.humidity,
                            (unsigned int)decoded.current_mode,
                            (unsigned int)decoded.app_value,
                            current_counter,
                            (unsigned int)decoded.fault_bits,
                            (unsigned int)total_frames,
                            (unsigned int)dlc_error_cnt,
                            (unsigned int)crc_error_cnt,
                            (unsigned int)duplicate_frame_cnt,
                            (unsigned int)counter_jump_cnt);
                    fflush(log_file);
                }
            } else if (id == 0x200U) {
                ControlCmd_t cmd;
                Unpack_Msg_0x200(frame.data, &cmd);
                printf("[0x200 控制指令] Target Mode: %u | Target Val: %u | "
                       "Rolling Counter: %u\n",
                       (unsigned int)cmd.target_mode,
                       (unsigned int)cmd.target_value,
                       (unsigned int)(cmd.rx_counter & 0x0FU));
            } else if (id == 0x280U) {
                /* [修改10] 按STM32的ACK协议重新标注三个字段 */
                uint8_t ack_type = frame.data[0];
                uint8_t ack_counter = frame.data[1] & 0x0FU;
                uint8_t ack_status = frame.data[2];

                printf("[0x280 ACK] Type: 0x%02X | Counter: %u | Status: %u\n",
                       ack_type,
                       (unsigned int)ack_counter,
                       (unsigned int)ack_status);
            } else {
                /* [修改11] 明确提示未解析的合法协议帧 */
                printf("[提示] 该CAN ID当前没有对应的解析器\n");
            }

            fflush(stdout);
        }
    }

    if (log_file != NULL) {
        fclose(log_file);
    }
    close(s);
    return 0;
}
