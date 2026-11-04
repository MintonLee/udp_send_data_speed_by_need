#include "udp_sender.h"
#include "package.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>

volatile sig_atomic_t g_stop = 0;

UdpSender::UdpSender()
    : sockfd_(-1), packets_sent_(0), send_fail_(0), payload_bytes_(0) {
    memset(&addr_, 0, sizeof(addr_));
}

UdpSender::~UdpSender() {
    stop();
}

bool UdpSender::start(const AppConfig& cfg) {
    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        perror("socket");
        return false;
    }
    memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(cfg.dest_port);
    if (inet_pton(AF_INET, cfg.dest_ip.c_str(), &addr_.sin_addr) != 1) {
        fprintf(stderr, "[ERR] 无效的目标IP: %s\n", cfg.dest_ip.c_str());
        close(sockfd_);
        sockfd_ = -1;
        return false;
    }
    // 加大发送缓冲区，避免高速率时触发内核拥塞限速导致吞吐上不去
    if (cfg.send_buffer_size > 0) {
        int sbs = cfg.send_buffer_size;
        if (setsockopt(sockfd_, SOL_SOCKET, SO_SNDBUF, &sbs, sizeof(sbs)) != 0) {
            perror("[WARN] setsockopt(SO_SNDBUF)");
        }
        int actual = 0;
        socklen_t alen = sizeof(actual);
        if (getsockopt(sockfd_, SOL_SOCKET, SO_SNDBUF, &actual, &alen) == 0) {
            printf("[OK] 发送缓冲: 请求=%d字节 实际=%d字节%s\n",
                   sbs, actual,
                   (actual < sbs) ? " (受 net.core.wmem_max 截断)" : "");
            if (actual < sbs) {
                printf("[提示] 提升发送缓冲: sudo sysctl -w net.core.wmem_max=%d\n", sbs);
            }
        }
    }
    printf("[OK] UDP 发送目标 %s:%u\n", cfg.dest_ip.c_str(), (unsigned)cfg.dest_port);
    return true;
}

void UdpSender::stop() {
    if (sockfd_ >= 0) {
        close(sockfd_);
        sockfd_ = -1;
    }
}

// 生成一个数据块内容（pattern 模式）
static void generateData(u8* out, size_t len, u32 seq, const std::string& pattern) {
    if (pattern == "random") {
        for (size_t i = 0; i < len; i++) out[i] = (u8)(rand() & 0xFF);
    } else if (pattern == "zero") {
        memset(out, 0, len);
    } else { // inc 递增
        u32 base = (u32)((uint64_t)seq * len);
        for (size_t i = 0; i < len; i++) out[i] = (u8)(base + i);
    }
}

void UdpSender::run(const AppConfig& cfg) {
    // ---- 基本参数校验 ----
    if (cfg.data_len == 0 || cfg.data_len > 65000) {
        fprintf(stderr, "[ERR] data_len=%u 非法，应在 1..65000 之间\n",
                (unsigned)cfg.data_len);
        return;
    }
    const size_t pkt_len = PKG_OVERHEAD + cfg.data_len;   // 线上单包长度
    // 超过以太网 MTU 载荷(1500-20-8=1472)会触发 IP 分片，给出提示（不阻止）
    if (pkt_len > 1472) {
        printf("[WARN] 单包 %zu 字节 > 1472，可能触发 IP 分片，建议减小 data_len\n", pkt_len);
    }

    // ---- 限速：突发 + 绝对截止时刻 ----
    // 逐包 sleep（高速率下每包间隔仅几十微秒）会受内核 timer slack(默认~50us)影响而不准；
    // 改为「每突发 burst_packets 个包连发，再按累计绝对时刻 sleep 校正」。
    double bytes_per_sec = 0.0;
    uint32_t burst_packets = 256;                 // 不限速时的批大小（仅用于周期性打印进度）
    std::chrono::nanoseconds burst_interval(0);
    if (cfg.target_rate_mbps > 0) {
        bytes_per_sec = (double)cfg.target_rate_mbps * 1e6 / 8.0;   // 有效字节速率
        double pace_s = (cfg.pace_interval_ms > 0) ? (cfg.pace_interval_ms / 1000.0) : 0.002;
        double bp = bytes_per_sec * pace_s / (double)cfg.data_len;  // 每个节奏窗口应发的包数
        burst_packets = (uint32_t)(bp + 0.5);
        if (burst_packets < 1) burst_packets = 1;
        double burst_bytes = (double)burst_packets * cfg.data_len;
        double ns = burst_bytes / bytes_per_sec * 1e9;              // 该批对应的绝对时间间隔
        if (ns < 1.0) ns = 1.0;
        burst_interval = std::chrono::nanoseconds((int64_t)llround(ns));
        double wire_rate = (double)(burst_packets * pkt_len) * 8.0 /
                           (burst_interval.count() / 1e9) / 1e6;
        printf("[OK] 目标限速: %d Mbps ≈ %.2f MB/s(有效), 每突发 %u 包(%zu B/包), "
               "突发间隔 %.3f ms, 线上速率 ≈ %.1f Mbps\n",
               cfg.target_rate_mbps, bytes_per_sec / 1e6,
               (unsigned)burst_packets, pkt_len,
               burst_interval.count() / 1e6, wire_rate);
    } else {
        printf("[OK] 不限速：全力发送（仅受链路与内核缓冲限制）\n");
    }

    // ---- file 模式：一次性读入整个数据文件 ----
    std::vector<u8> fileData;
    size_t fileOffset = 0;
    if (cfg.data_source == "file") {
        FILE* fp = fopen(cfg.data_file.c_str(), "rb");
        if (!fp) {
            fprintf(stderr, "[ERR] 无法打开数据文件: %s (%s)\n",
                    cfg.data_file.c_str(), strerror(errno));
            return;
        }
        fseek(fp, 0, SEEK_END);
        long fsz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (fsz <= 0) {
            fclose(fp);
            fprintf(stderr, "[ERR] 数据文件为空: %s\n", cfg.data_file.c_str());
            return;
        }
        fileData.resize((size_t)fsz);
        if (fread(fileData.data(), 1, fileData.size(), fp) != fileData.size()) {
            fclose(fp);
            fprintf(stderr, "[ERR] 读取数据文件不完整: %s\n", cfg.data_file.c_str());
            return;
        }
        fclose(fp);
        printf("[OK] 已加载数据文件: %s (%zu 字节, 约 %zu 个 %u 字节数据块), 循环=%s\n",
               cfg.data_file.c_str(), fileData.size(),
               (fileData.size() + cfg.data_len - 1) / cfg.data_len,
               (unsigned)cfg.data_len, cfg.file_loop ? "开" : "关");
    } else {
        printf("[OK] 数据来源: pattern(%s)\n", cfg.data_pattern.c_str());
    }

    std::vector<u8> payload(cfg.data_len);            // 数据块缓冲
    std::vector<u8> frame(PKG_OVERHEAD + cfg.data_len); // 整包缓冲
    u32 seq = cfg.start_seq;
    bool done = false;

    auto runStart = std::chrono::steady_clock::now();
    auto deadline = runStart;                         // 累计绝对截止时刻
    auto nextReport = runStart + std::chrono::seconds(2);
    uint32_t burst_index = 0;

    printf("[OK] 开始发送: %s:%u data_len=%u 帧头\"FS\"/帧尾\"FE\" "
           "(send_count=%u, Ctrl+C停止)\n",
           cfg.dest_ip.c_str(), (unsigned)cfg.dest_port,
           (unsigned)cfg.data_len, (unsigned)cfg.send_count);

    while (!g_stop && !done) {
        // 帧级限速：睡到本批的绝对发送时刻（首批立即发送）
        if (cfg.target_rate_mbps > 0 && burst_index > 0) {
            auto now = std::chrono::steady_clock::now();
            // 落后超过一个节奏窗口（如 sendto 被内核缓冲阻塞）时重新对齐，避免突发追帧冲垮接收端
            if (now - deadline > burst_interval) deadline = now;
            else std::this_thread::sleep_until(deadline);
        }
        deadline += burst_interval;

        // 连发一批（burst_packets 个包）
        for (uint32_t k = 0; k < burst_packets && !g_stop && !done; k++) {
            size_t n = cfg.data_len;
            if (cfg.data_source == "file") {
                if (fileOffset >= fileData.size()) {
                    if (cfg.file_loop) {
                        fileOffset = 0;                 // 循环：回到文件头
                    } else {
                        printf("[OK] 数据文件已发送完毕，结束\n");
                        done = true;
                        break;
                    }
                }
                size_t avail = fileData.size() - fileOffset;
                n = (cfg.data_len < avail) ? cfg.data_len : avail;  // 末块可能不足 data_len
                memcpy(payload.data(), fileData.data() + fileOffset, n);
                fileOffset += n;
            } else {
                n = cfg.data_len;
                generateData(payload.data(), n, seq, cfg.data_pattern);
            }

            int flen = package_data(payload.data(), (u16)n, seq, frame.data());
            ssize_t r = -1;
            for (int attempt = 0; attempt < 3; attempt++) {
                r = sendto(sockfd_, frame.data(), (size_t)flen, 0,
                           (struct sockaddr*)&addr_, sizeof(addr_));
                if (r >= 0) break;
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
                    // 内核发送缓冲暂时满：短暂让出后重试，避免丢包
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                    continue;
                }
                perror("[ERR] sendto");
                break;
            }
            if (r < 0) {
                send_fail_++;
            } else {
                packets_sent_++;
                payload_bytes_ += (unsigned long long)n;
            }
            seq++;

            if (cfg.verbose) {
                printf("[OK] 已发送包 seq=%u len=%zu\n", (unsigned)(seq - 1), n);
            }
            if (cfg.send_count > 0 && packets_sent_ >= cfg.send_count) {
                printf("[OK] 已达到设定发送包数 %u，结束\n", (unsigned)cfg.send_count);
                done = true;
                break;
            }
        }
        burst_index++;

        // 每 2s 打印一次实测速率，便于确认是否达到目标带宽
        if (std::chrono::steady_clock::now() >= nextReport) {
            double secs = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - runStart).count();
            printf("[进度] %.1fs 包=%llu 有效=%.1f Mbps 失败=%llu\n",
                   secs, (unsigned long long)packets_sent_,
                   secs > 0 ? (double)payload_bytes_ * 8.0 / secs / 1e6 : 0.0,
                   (unsigned long long)send_fail_);
            nextReport += std::chrono::seconds(2);
        }
    }

    printf("[STAT] 发送包=%llu 有效字节=%llu 失败=%llu\n",
           (unsigned long long)packets_sent_,
           (unsigned long long)payload_bytes_,
           (unsigned long long)send_fail_);
}
