#include <cstdio>
#include <cstring>
#include <csignal>

#include "config.h"
#include "udp_sender.h"

// Ctrl+C / kill 时置位停止标志
static void handleSignal(int) {
    g_stop = 1;
}

int main(int argc, char* argv[]) {
    const char* cfgPath = "config.ini";
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            printf("用法: %s [配置文件路径]\n", argv[0]);
            printf("  不指定参数时默认加载当前目录下的 config.ini\n");
            return 0;
        }
        cfgPath = argv[1];
    }

    AppConfig cfg;
    if (!loadConfig(cfgPath, cfg)) {
        fprintf(stderr, "[ERR] 配置加载失败: %s\n", cfgPath);
        return 1;
    }

    printf("================ 配置 ================\n");
    printf("目标地址    : %s:%u\n", cfg.dest_ip.c_str(), (unsigned)cfg.dest_port);
    printf("数据段长度  : %u 字节/包 (整包 %u 字节)\n",
           (unsigned)cfg.data_len, (unsigned)(cfg.data_len + 10));
    printf("帧协议      : 帧头\"FS\" + seq(u32) + data_len(u16) + 数据 + 帧尾\"FE\"\n");
    printf("起始包序号  : %u\n", (unsigned)cfg.start_seq);
    printf("发送包数    : %u (0=不限)\n", (unsigned)cfg.send_count);
    printf("数据来源    : %s (%s)\n", cfg.data_source.c_str(),
           cfg.data_source == "file" ? "二进制文件" : "生成测试数据");
    if (cfg.data_source == "file") {
        printf("数据文件    : %s, 循环=%d\n", cfg.data_file.c_str(), cfg.file_loop);
    } else {
        printf("数据内容    : %s (inc/random/zero)\n", cfg.data_pattern.c_str());
    }
    printf("发送缓冲    : %d 字节\n", cfg.send_buffer_size);
    printf("目标限速    : %d Mbps (0=不限速), 节奏窗口=%d ms\n",
           cfg.target_rate_mbps, cfg.pace_interval_ms);
    printf("======================================\n");

    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    UdpSender snd;
    if (!snd.start(cfg)) return 1;
    snd.run(cfg);
    snd.stop();
    return 0;
}
