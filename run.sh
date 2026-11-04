#!/bin/bash
# ============================================================
#  udp_send_data_speed_by_need 通用二进制文件 UDP 限速发送程序 运行脚本
#  用法：./run.sh            # 使用默认配置 config.ini
#        ./run.sh my.ini     # 使用指定配置文件
# ============================================================
set -e
cd "$(dirname "$0")"

CONFIG="config.ini"
if [ -n "$1" ]; then
    CONFIG="$1"
fi

if [ ! -f "$CONFIG" ]; then
    echo "[ERR] 配置文件不存在: $CONFIG"
    exit 1
fi

echo "使用配置文件: $CONFIG"
exec ./udp_send_data_speed_by_need "$CONFIG"
