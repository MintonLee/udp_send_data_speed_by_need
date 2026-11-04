#pragma once
// ============================================================
//  通用 UDP 帧协议定义与发送端封装函数（小端）
//
//  帧结构:
//    [0..1]                 帧头 magic  "FS" (0x46 0x53)  -- Frame Start
//    [2..5]   seq           uint32   包序号（从 start_seq 起递增）
//    [6..7]   data_len      uint16   数据段长度（字节）
//    [8 .. 8+data_len-1]    payload  数据段（来自二进制文件的原始字节）
//    [8+data_len .. +1]     帧尾 magic "FE" (0x46 0x45)  -- Frame End
//
//    整包长度 = PKG_OVERHEAD + data_len = 10 + data_len
//
//  说明：本协议为通用二进制分块传输协议，接收端按 seq 排序重组即可还原文件。
// ============================================================

#include <cstdint>
#include <cstddef>
#include <cstring>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

// 帧头 / 帧尾标识（ASCII "FS" / "FE"）
static const u8 PKG_HEAD_MAGIC[2] = { 'F', 'S' };   // 0x46 0x53
static const u8 PKG_TAIL_MAGIC[2] = { 'F', 'E' };   // 0x46 0x45

static const size_t PKG_HEAD_LEN  = 8;   // FS(2) + seq(4) + data_len(2)
static const size_t PKG_TAIL_LEN  = 2;   // FE(2)
static const size_t PKG_OVERHEAD  = PKG_HEAD_LEN + PKG_TAIL_LEN;  // 10

// 封装一个数据块到 out_buf，返回整包长度（10 + data_len）。
// out_buf 至少需要 (PKG_OVERHEAD + data_len) 字节空间。
static int package_data(const u8* payload, u16 data_len, u32 seq, u8* out_buf) {
    // 帧头 "FS"
    out_buf[0] = PKG_HEAD_MAGIC[0];
    out_buf[1] = PKG_HEAD_MAGIC[1];

    // 包序号 seq (uint32, 小端)
    out_buf[2] = (u8)( seq        & 0xFF);
    out_buf[3] = (u8)((seq >>  8) & 0xFF);
    out_buf[4] = (u8)((seq >> 16) & 0xFF);
    out_buf[5] = (u8)((seq >> 24) & 0xFF);

    // 数据段长度 data_len (uint16, 小端)
    out_buf[6] = (u8)( data_len        & 0xFF);
    out_buf[7] = (u8)((data_len >> 8)  & 0xFF);

    // 数据段
    memcpy(out_buf + PKG_HEAD_LEN, payload, data_len);

    // 帧尾 "FE"
    out_buf[PKG_HEAD_LEN + data_len + 0] = PKG_TAIL_MAGIC[0];
    out_buf[PKG_HEAD_LEN + data_len + 1] = PKG_TAIL_MAGIC[1];

    return (int)(PKG_OVERHEAD + (size_t)data_len);
}
