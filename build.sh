#!/bin/bash
# ============================================================
#  udp_send_data_speed_by_need 通用二进制文件 UDP 限速发送程序 一键构建脚本
#  功能：先删除之前生成的文件与编译内容，再重新编译
#  用法：./build.sh
# ============================================================
set -e
cd "$(dirname "$0")"

echo "[1/3] 清理旧的构建产物..."
rm -rf build
rm -f udp_send_data_speed_by_need

echo "[2/3] 创建构建目录并编译..."
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"

echo "[3/3] 拷贝可执行文件到工程根目录..."
cp -f udp_send_data_speed_by_need ../udp_send_data_speed_by_need
cd ..

echo ""
echo "构建完成！可执行文件: ./udp_send_data_speed_by_need"
