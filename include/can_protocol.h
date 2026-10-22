#ifndef __CAN_PROTOCOL_H
#define __CAN_PROTOCOL_H

#include <stdint.h>
#include "crc8.h"

typedef enum{
	AUTO = 1,
	MANUAL = 2,
	SAFE = 3,
	CLEAR_FAULT = 4
}Control_Status_t;

typedef struct {
    int16_t temperature;
    int16_t humidity;
    uint8_t valid_flag;
    uint8_t app_value;
    uint8_t current_mode;
    uint8_t fault_bits;
    uint8_t rx_counter; // 接收时提取出的对方 counter
} SensorMsg_t;

//0x200
typedef struct {
    uint8_t target_mode;
    uint16_t target_value;
    uint8_t rx_counter;
} ControlCmd_t;

typedef struct{
	uint32_t id;
	uint8_t dlc;
	uint8_t data[8];
}Can_Frame_t;

typedef enum {
    RX_VALID_NEW = 0,   // 全新且合法的指令
    RX_DUPLICATE,       // 重复指令（需回复 0x280）
    RX_ERR_ID,          // ID 错误
    RX_ERR_DLC,         // 长度错误
    RX_ERR_CRC          // 校验码错误
} Rx_Filter_Status_t;

typedef struct {
    uint8_t echoed_id;      // 产生重复冲突的原始目标 ID (如 0x20)
    uint8_t duplicate_cnt;  // 被拦截的重复 Counter 值
    uint8_t status_code;    // 状态码 (如 0x01 代表 Counter 相同拦截)
} AckMsg_t;

void Pack_Msg_0x180(SensorMsg_t *data, uint8_t *payload);
void Unpack_Msg_0x180(uint8_t *payload, SensorMsg_t *data);
void Pack_Msg_0x200(ControlCmd_t *cmd, uint8_t *payload);
void Unpack_Msg_0x200(uint8_t *payload, ControlCmd_t *cmd);
Rx_Filter_Status_t Filter_And_Decode_0x200(Can_Frame_t *rx_frame, ControlCmd_t *out_cmd);
void Unpack_Msg_0x181(uint8_t *payload, SensorMsg_t *data);
void Unpack_Msg_0x280(uint8_t *payload, AckMsg_t *data);

#endif
