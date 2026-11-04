#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>

// 去掉首尾空白字符
static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 解析一行 key=value，返回 false 表示空行或注释行
static bool parseLine(const std::string& line, std::string& key, std::string& val) {
    std::string s = trim(line);
    if (s.empty() || s[0] == '#' || s[0] == ';') return false;
    size_t eq = s.find('=');
    if (eq == std::string::npos) return false;
    key = trim(s.substr(0, eq));
    val = trim(s.substr(eq + 1));
    size_t c = val.find('#');
    if (c != std::string::npos) val = trim(val.substr(0, c));
    return !key.empty();
}

bool loadConfig(const std::string& path, AppConfig& cfg) {
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) {
        fprintf(stderr, "[ERR] 无法打开配置文件 %s: %s\n",
                path.c_str(), strerror(errno));
        return false;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        std::string key, val;
        if (!parseLine(line, key, val)) continue;

        if (key == "dest_ip") {
            cfg.dest_ip = val;
        } else if (key == "dest_port") {
            cfg.dest_port = (uint16_t)strtoul(val.c_str(), NULL, 10);
        } else if (key == "data_len") {
            cfg.data_len = (uint16_t)strtoul(val.c_str(), NULL, 10);
        } else if (key == "start_seq") {
            cfg.start_seq = (uint32_t)strtoul(val.c_str(), NULL, 10);
        } else if (key == "send_count") {
            cfg.send_count = (uint32_t)strtoul(val.c_str(), NULL, 10);
        } else if (key == "target_rate_mbps") {
            cfg.target_rate_mbps = (int)strtoul(val.c_str(), NULL, 10);
        } else if (key == "pace_interval_ms") {
            cfg.pace_interval_ms = (int)strtoul(val.c_str(), NULL, 10);
        } else if (key == "send_buffer_size") {
            cfg.send_buffer_size = (int)strtoul(val.c_str(), NULL, 10);
        } else if (key == "data_source") {
            cfg.data_source = val;
        } else if (key == "data_file") {
            cfg.data_file = val;
        } else if (key == "file_loop") {
            cfg.file_loop = (int)strtoul(val.c_str(), NULL, 10);
        } else if (key == "data_pattern") {
            cfg.data_pattern = val;
        } else if (key == "verbose") {
            cfg.verbose = (strtoul(val.c_str(), NULL, 10) != 0);
        }
        // 未知键忽略
    }
    fclose(fp);
    return true;
}
