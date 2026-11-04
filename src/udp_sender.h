#pragma once
// UDP 数据发送器：把二进制文件（或生成的测试数据）按帧协议分块并通过 UDP 限速发送

#include <csignal>
#include <string>

#include <netinet/in.h>

#include "config.h"

// 全局停止标志，由 main 的信号处理函数置位
extern volatile sig_atomic_t g_stop;

class UdpSender {
public:
    UdpSender();
    ~UdpSender();

    // 创建 UDP 套接字并设置目标地址
    bool start(const AppConfig& cfg);
    void stop();

    // 按配置循环发送（Ctrl+C、文件发完或达到发送包数后退出）
    void run(const AppConfig& cfg);

private:
    int sockfd_;
    struct sockaddr_in addr_;

    uint64_t packets_sent_;    // 成功发出的包数
    uint64_t send_fail_;       // 发送失败的包数
    uint64_t payload_bytes_;   // 成功发出的有效数据字节数
};
