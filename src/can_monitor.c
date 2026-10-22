#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <poll.h>
#include <time.h> // 用于获取 CSV 记录的时间戳

#include "can_protocol.h" 
#include "crc8.h"

// 定义统计变量
uint32_t total_frames = 0;
uint32_t crc_error_cnt = 0;
uint32_t counter_jump_cnt = 0;
uint32_t duplicate_frame_cnt = 0;
int last_rx_counter = -1; 

int main(int argc, char *argv[]) {
    int s;
    struct sockaddr_can addr;
    struct ifreq ifr;

    const char *ifname = (argc > 1) ? argv[1] : "can0";

    // 打开 CSV 文件用于追加记录
    FILE *log_file = fopen("can_log.csv", "a");
    if (log_file == NULL) {
        perror("[警告] 无法创建或打开 can_log.csv 文件");
    } else {
        // 写入 CSV 表头
        fprintf(log_file, "Timestamp,CAN_ID,Temp,Hum,Mode,AppValue,Counter,FaultBits,TotalFrames,CRC_Err,Dup_Err,Jump_Err\n");
        fflush(log_file);
    }

    if ((s = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
        perror("[错误] Socket creation failed");
        return 1;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        perror("[错误] ioctl 获取网卡索引失败 (请检查 can0 是否已创建并插好)");
        close(s);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[错误] Bind failed");
        close(s);
        return 1;
    }

    struct pollfd pfd;
    pfd.fd = s;
    pfd.events = POLLIN;

    struct can_frame frame;
    printf("Listening on %s (ifindex: %d)... 数据将同步记录到 can_log.csv\n", ifname, ifr.ifr_ifindex);
    fflush(stdout);

    while (1) 
	{
        int ret = poll(&pfd, 1, 1000); 

        if (ret < 0) {
            perror("[错误] poll 异常");
            break;
        } else if (ret == 0) {
            printf("[Poll] 1秒超时，当前总线无数据...\n");
            fflush(stdout);
        } else {
            if (pfd.revents & POLLIN) {
                int nbytes = read(s, &frame, sizeof(struct can_frame));
                if (nbytes > 0) {
                    total_frames++; // 总帧数累加
                    
                    uint32_t id = frame.can_id & CAN_SFF_MASK;
                    uint8_t rx_crc = frame.data[7];
                    uint8_t calc_crc = Calculate_CRC8_J1850(frame.data, 7);
                    
                    // 1. 拦截 CRC 错误：只统计，不继续解释为有效数据[cite: 19]
                    if (rx_crc != calc_crc) {
                        crc_error_cnt++;
                        printf("\n--- 收到 CAN ID: 0x%03X (DLC: %d) ---\n", id, frame.can_dlc);
                        printf("[致命错误] CRC 校验失败! 接收:0x%02X 计算:0x%02X (丢弃当前帧)\n", rx_crc, calc_crc);
                        printf("[统计] 总帧数:%d | CRC错误:%d | 重复帧:%d | 跳变:%d\n", 
                               total_frames, crc_error_cnt, duplicate_frame_cnt, counter_jump_cnt);
                        
                        // 记录坏帧到 CSV
                        if (log_file) {
                            fprintf(log_file, "%ld,0x%03X,ERROR_CRC,,,,,%d,%d,%d,%d\n", 
                                    time(NULL), id, total_frames, crc_error_cnt, duplicate_frame_cnt, counter_jump_cnt);
                            fflush(log_file);
                        }
                        
                        continue; // 核心修改：直接跳出本次处理，绝对不往下走到解码逻辑
                    }

                    printf("\n--- 收到 CAN ID: 0x%03X (DLC: %d) ---\n", id, frame.can_dlc);
                    printf("[CRC 校验] 接收: 0x%02X | 计算: 0x%02X -> 通过\n", rx_crc, calc_crc);

                    if (id == 0x180) {
                        SensorMsg_t decoded;
                        Unpack_Msg_0x180(frame.data, &decoded);
                        
                        // 2. 统计计数器跳变与重复帧[cite: 19]
                        if (last_rx_counter != -1) {
                            if (decoded.rx_counter == last_rx_counter) {
                                duplicate_frame_cnt++; // 收到一样的 Counter，判定为重复[cite: 19]
                            } else if (decoded.rx_counter != (last_rx_counter + 1) % 16) {
                                counter_jump_cnt++;    // 没有按顺序递增，判定为丢帧跳变[cite: 19]
                            }
                        }
                        last_rx_counter = decoded.rx_counter;

                        printf("[0x180 传感器状态] Temp: %d | Hum: %d | Mode: %d | AppValue: %d | Valid: %d\n", 
                               decoded.temperature, decoded.humidity, decoded.current_mode, decoded.app_value, decoded.valid_flag);
                        printf("[状态诊断] Counter: %d | Fault Bits: 0x%02X\n", 
                               decoded.rx_counter, decoded.fault_bits);
                        
                        // 打印实时统计信息[cite: 19]
                        printf("[统计] 总帧数:%d | CRC错误:%d | 重复帧:%d | 跳变:%d\n", 
                               total_frames, crc_error_cnt, duplicate_frame_cnt, counter_jump_cnt);

                        // 3. 将合法数据写入 CSV 记录[cite: 19]
                        if (log_file) {
                            fprintf(log_file, "%ld,0x180,%d,%d,%d,%d,%d,0x%02X,%d,%d,%d,%d\n", 
                                    time(NULL), decoded.temperature, decoded.humidity, 
                                    decoded.current_mode, decoded.app_value, decoded.rx_counter, decoded.fault_bits,
                                    total_frames, crc_error_cnt, duplicate_frame_cnt, counter_jump_cnt);
                            fflush(log_file);
                        }
                    } 
                    else if (id == 0x200) {
                        ControlCmd_t cmd;
                        Unpack_Msg_0x200(frame.data, &cmd);
                        printf("[0x200 控制指令] Target Mode: %d | Target Val: %d | Rolling Counter: %d\n",
                               cmd.target_mode, cmd.target_value, cmd.rx_counter);
                    }
                    else if (id == 0x280) {
                        printf("[0x280 应答/告警] Status/Cmd: 0x%02X | 关联 Counter: %d | Mode: %d\n", 
                               frame.data[0], frame.data[1], frame.data[2]);
                    }
                    
                    fflush(stdout);
                }
            } 
            else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                printf("[警告] 网卡状态异常 (revents=0x%X)，请检查 %s 是否已 UP！\n", pfd.revents, ifname);
                sleep(1); 
            }
        }
    }

    if (log_file) fclose(log_file);
    close(s);
    return 0;
}