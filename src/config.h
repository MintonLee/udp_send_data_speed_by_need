#pragma once
// 发送端配置结构体定义（通用二进制文件 UDP 发送）

#include <string>
#include <cstdint>

struct AppConfig {
    std::string dest_ip        = "127.0.0.1";  // 目标IP（接收端地址）
    uint16_t    dest_port      = 6000;         // 目标端口

    uint16_t    data_len       = 1024;         // 单个UDP包的数据段长度（字节）
    uint32_t    start_seq      = 0;            // 起始包序号
    uint32_t    send_count     = 0;            // 发送包数上限，0=不限（文件模式发完即停；file_loop=1 时无限）

    // 数据来源
    std::string data_source    = "file";       // file=从二进制文件发送 / pattern=生成测试数据
    std::string data_file      = "./data/sample.bin"; // 数据文件路径（任意二进制内容）
    int         file_loop      = 0;            // 文件循环发送: 1=循环 0=发完即停
    std::string data_pattern   = "inc";        // pattern模式的数据内容: inc/random/zero

    // 发送限速（按有效数据字节速率计算，1Mbps = 1e6 bit/s）
    int         target_rate_mbps = 160;        // 目标速率，0=不限速全力发送
    int         pace_interval_ms = 2;          // 限速节奏窗口（毫秒），每发送约该时长的数据后统一 sleep 校正
    int         send_buffer_size = 4 * 1024 * 1024; // 套接字发送缓冲区大小（字节），高速率建议 >= 4MB
    bool        verbose        = false;        // 详细日志
};

// 从配置文件加载参数，成功返回 true
bool loadConfig(const std::string& path, AppConfig& cfg);
