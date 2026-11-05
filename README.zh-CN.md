# udp_send_data_speed_by_need

[English](README.md) | **简体中文**

![Language](https://img.shields.io/badge/language-C%2B%2B11-blue)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20ARM64%20%7C%20x86-lightgrey)
![Build](https://img.shields.io/badge/build-CMake%20%E2%89%A53.10-green)
![Rate](https://img.shields.io/badge/rate-%E2%89%A4160%20Mbps%20(tested)-orange)
![Forks](https://img.shields.io/github/forks/MintonLee/udp_send_data_speed_by_need?style=social&label=Forks)

一个**速率可控的高吞吐 UDP 发送器**：把任意二进制文件从 Linux 主机（含嵌入式 ARM64 板卡）以**精确、可调节的码率**流式发送到接收端。它把文件切成固定大小的块，每块用一个轻量的 `FS … FE` 帧协议封装，并按目标速率对发送节奏进行精确控制。

> **当前状态**：实测 **≤ 160 Mbps**（有效数据速率）下稳定。更高速率受硬件与网络制约——实测 `target_rate_mbps=1000` 只能达到 800+ Mbps，且**高速传输必然丢包**。请在自有环境实测可达到的速率与丢包率。

---

## 目录

- [简介](#简介)
- [功能特性](#功能特性)
- [工作原理](#工作原理)
  - [帧协议](#帧协议)
  - [限速原理](#限速原理)
  - [端到端流程](#端到端流程)
- [运行环境](#运行环境)
- [快速开始](#快速开始)
- [编译](#编译)
- [运行](#运行)
- [调速后运行](#调速后运行)
- [生成测试数据](#生成测试数据)
- [配置参数一览](#配置参数一览)
- [Fork 数变化](#fork-数变化)
- [工程结构](#工程结构)
- [常见问题排查](#常见问题排查)
- [许可](#许可)

---

## 简介

`udp_send_data_speed_by_need` 把任意二进制文件读入内存（或生成测试数据），按 `data_len` 字节切块，每块加上首尾标识与序号后，通过 UDP 以精确限速发送到目标 `ip:port`。接收端按序号收集各包即可还原原始文件。

它是一个通用的**按可控速率流式发送大体积二进制数据**的工具——可用于给下游处理程序喂数据、回放采集数据，或以目标带宽对链路/接收端做压力测试。

## 功能特性

- **精确限速** —— 目标速率以 Mbps 配置（按有效数据计），默认 **160 Mbps**，`0` = 不限速（裸吞吐压测）。
- **突发 + 绝对截止时刻** —— 平均速率无累积漂移，且不受内核 timer slack 影响（见[限速原理](#限速原理)）。
- **简洁的 `FS … FE` 帧协议** —— 首尾标识 + 序号 + 长度，便于重组与丢包感知。
- **两种数据来源** —— `file`（任意 `.bin` / `.dat`）或 `pattern`（`inc` / `random` / `zero`）。
- **面向高速率** —— 加大 `SO_SNDBUF`（默认 4 MB），`sendto()` 在 `EAGAIN` / `ENOBUFS` 时重试。
- **实时吞吐报告** —— 每 2 秒打印实测 Mbps。
- **测试数据生成器** —— `scripts/gen_data.py` 可生成随机二进制文件（如 128 MB）。
- **INI 配置 + 辅助脚本** —— `config.ini`、`build.sh`、`run.sh`。
- **可移植** —— C++11，CMake ≥ 3.10，Linux（ARM64 / x86），无第三方库。

## 工作原理

### 帧协议

每个 UDP 数据报携带一个数据块。线上布局（小端），整包长度 `= 10 + data_len`：

| 偏移 | 字节 | 字段 | 说明 |
|-----:|-----:|------|------|
| 0 | 2 | `head` | 帧头标识 **`FS`**（`0x46 0x53`） |
| 2 | 4 | `seq` | `uint32` 包序号（从 `start_seq` 起） |
| 6 | 2 | `data_len` | `uint16` 数据段长度（字节） |
| 8 | `data_len` | `payload` | 文件原始字节 |
| `8+data_len` | 2 | `tail` | 帧尾标识 **`FE`**（`0x46 0x45`） |

默认 `data_len = 1024` 时，单包线上长度 **1034 B**（小于 1500 B 以太网 MTU，不触发 IP 分片）。接收端校验 `FS`/`FE` 标识、读取 `seq` 与 `data_len`，按序写入载荷即可还原文件。当文件大小不是 `data_len` 整数倍时，末块会短于 `data_len`。

### 限速原理

只需关注一个参数：`target_rate_mbps`（按**有效数据字节**计，`1 Mbps = 1e6 bit/s`）。

```
数据有效字节速率 bytes_per_sec = target_rate_mbps × 1e6 / 8      # 160 Mbps → 20,000,000 B/s
每突发包数      burst_packets  = round(bytes_per_sec × pace_interval_ms/1000 / data_len)  # 160Mbps ≈ 39
突发间隔        burst_interval = burst_packets × data_len / bytes_per_sec                 # ≈ 2.0 ms
```

核心思想：**一批包一次性突发连发，然后在批级用「绝对截止时刻」统一限速。**

```
deadline = 起始时刻
while 未停止:
    if 开启限速 and 非首批:
        if now - deadline > burst_interval:   # 落后超过一个突发窗口
            deadline = now                     # 重新对齐——不追帧突发
        else:
            sleep_until(deadline)              # 绝对截止时刻 → 无累积漂移
    deadline += burst_interval
    连发 burst_packets 个包（sendto，遇 EAGAIN/ENOBUFS 重试）
```

为什么这样设计：

1. **绝对截止时刻（`sleep_until`）而非相对 sleep** —— 每批都对齐到「起始时刻 + N × 间隔」，单次调度抖动不会随时间累积成速率漂移。
2. **批级粒度而非逐包** —— 朴素的「每包 sleep」在 160 Mbps 下需要约 51 µs 粒度，与 Linux 默认 **timer slack（约 50 µs）** 同量级，sleep 精度被稀释、实测速率会塌到约 50 Mbps；而约 2 ms 的突发间隔约为其 40 倍，限速因此精确。`pace_interval_ms` 可用于调节这一权衡。
3. **追赶保护（不追帧）** —— 若某批被拖慢（如 `sendto` 被满的内核缓冲阻塞），发送端把截止时刻重置为「现在」，而不是突发补发。UDP 无流控，突发补发会瞬间冲垮接收端造成丢包。
4. **加大 `SO_SNDBUF`（4 MB）** —— 20 MB/s ≈ 2 万包/秒，默认套接字缓冲太小会触发 `EAGAIN`/丢包。若被内核上限截断：
   ```bash
   sudo sysctl -w net.core.wmem_max=4194304
   ```
5. **`sendto()` 重试** —— 遇 `EAGAIN` / `EWOULDBLOCK` / `ENOBUFS` 时等待 200 µs 重试（最多 3 次），吸收偶发的缓冲满。

> **有效速率 vs 线上速率**：限速只按数据段字节计，但每包额外有 10 B 帧开销。160 Mbps 有效速率、`data_len=1024` 时线上 ≈ **161.6 Mbps**（约 1% 开销）。程序启动时会打印线上速率供对照。

### 端到端流程

```
gen_data.py ─► data/sample.bin ─►【整文件读入内存】
                                          │
                                          ▼
                          按 data_len 字节切块
                                          │
                                          ▼
              逐块封装帧： FS | seq | data_len | payload | FE
                                          │
                                          ▼
             突发连发 N 包 ─► sleep_until(绝对截止时刻)
                                          │            （限速）
                                          ▼
                                   UDP ─► 接收端
                                          │
                                          ▼
                       接收端按 seq 重组还原文件
```

## 运行环境

- **操作系统**：Linux（推荐 Ubuntu 18.04+）。支持 ARM aarch64（如 RK3588、Jetson）与 x86_64。
- **编译器**：支持 **C++11** 的 GCC / Clang。
- **构建工具**：**CMake ≥ 3.10**、`make`。
- **Python 3**（仅 `scripts/gen_data.py` 与 fork 追踪脚本需要；`--seed` 需 Python 3.9+）。
- **无第三方 C++ 依赖** —— 仅用 POSIX socket 与 C++ 标准库。

```bash
# Debian / Ubuntu
sudo apt-get update
sudo apt-get install -y build-essential cmake python3
```

## 快速开始

```bash
# 1) 生成一个 128MB 随机测试文件
python3 scripts/gen_data.py -o data/sample.bin -s 128MB

# 2) 在 config.ini 中填好接收端地址（按需修改 dest_ip / dest_port）
#    data_file=./data/sample.bin、target_rate_mbps=160 已是默认值

# 3) 编译
./build.sh

# 4) 运行（Ctrl+C 停止）
./run.sh
```

## 编译

**方式 A —— 一键脚本（推荐）：**

```bash
./build.sh
```

该脚本会清理 `build/`、以 `Release` 运行 CMake、编译，并把可执行文件拷贝到工程根目录（`./udp_send_data_speed_by_need`）。

**方式 B —— 手动 CMake：**

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"
```

生成物为 `build/udp_send_data_speed_by_need`。

> **交叉编译到 ARM64**（如在 x86 主机上）：传入工具链文件——
> ```bash
> cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=your-aarch64-toolchain.cmake ..
> ```

## 运行

先确认 `config.ini` 中的接收端（`dest_ip`、`dest_port`）与数据来源配置正确，然后：

```bash
./run.sh                 # 使用 ./config.ini
./run.sh my_config.ini   # 或指定配置文件
```

也可直接运行可执行文件：

```bash
./udp_send_data_speed_by_need                 # 加载 ./config.ini
./udp_send_data_speed_by_need path/to/cfg.ini # 显式指定配置
./udp_send_data_speed_by_need --help
```

随时 **Ctrl+C** 停止（已处理 SIGINT/SIGTERM，会优雅退出并打印统计）。

**还没有数据文件？** 用 pattern 模式快速自测——在 `config.ini` 中设置：

```ini
data_source = pattern
data_pattern = inc          # inc | random | zero
target_rate_mbps = 160
```

典型启动输出：

```
[OK] 发送缓冲: 请求=4194304字节 实际=4194304字节
[OK] UDP 发送目标 127.0.0.1:6000
[OK] 目标限速: 160 Mbps ≈ 20.00 MB/s(有效), 每突发 39 包(1034 B/包), 突发间隔 2.0 ms, 线上速率 ≈ 161.6 Mbps
[OK] 开始发送: 127.0.0.1:6000 data_len=1024 帧头"FS"/帧尾"FE" (send_count=0, Ctrl+C停止)
[进度] 2.0s 包=39000 有效=159.7 Mbps 失败=0
```

## 调速后运行

速率完全由 `config.ini` 中的 `target_rate_mbps` 控制。**改 → 存 → 重新运行**（无需重新编译）：

```bash
# 1) 修改速率
sed -i 's/^target_rate_mbps=.*/target_rate_mbps=100/' config.ini   # 或手动编辑

# 2) 重新运行
./run.sh
```

| 需求 | 配置 | 效果 |
|------|------|------|
| 100 Mbps | `target_rate_mbps=100` | 突发间隔变大，平均有效速率 ≈ 100 Mbps |
| 160 Mbps（默认） | `target_rate_mbps=160` | 突发间隔 ≈ 2.0 ms |
| 不限速（吞吐压测） | `target_rate_mbps=0` | 不做节奏 sleep，纯 `sendto` 压测 |
| 调节节奏粗细 | `pace_interval_ms=1..10` | 越小越贴近 timer slack、越大越平稳 |
| 只发固定包数 | `send_count=100000` | 发满 100000 包后停止 |

> **换算口诀**：`Mbps = 包速率 × data_len × 8 / 1e6`。通过 `[进度] ... 有效 xxx Mbps` 一行确认真实速率。

## 生成测试数据

`scripts/gen_data.py` 可生成任意大小的随机二进制文件（仅用标准库）：

```bash
python3 scripts/gen_data.py                        # data/sample.bin，128 MB（默认）
python3 scripts/gen_data.py -o data/test.dat -s 128MB
python3 scripts/gen_data.py -s 64M --chunk 4MB     # 64 MB，4 MB 写块
python3 scripts/gen_data.py -s 1G --seed 42        # 1 GB，可复现（需 Python 3.9+）
python3 scripts/gen_data.py -s 1048576             # 直接用字节数
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-o, --output` | `data/sample.bin` | 输出文件路径 |
| `-s, --size` | `128MB` | 大小：`128MB` / `128M` / `1G` / `512KB` / 原始字节数 |
| `--chunk` | `1MB` | 每次写入块大小 |
| `--seed` | 无 | 随机种子，用于可复现输出（需 Python 3.9+） |

不带 `--seed` 时使用 `os.urandom`（快、不可复现）。生成的 `data/*.bin` / `data/*.dat` 已被 git 忽略。

## 配置参数一览

所有选项都在 `config.ini`（`key=value`；`#` / `;` 开头为注释）。

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `dest_ip` | `127.0.0.1` | 接收端 IP |
| `dest_port` | `6000` | 接收端 UDP 端口 |
| `data_len` | `1024` | 单包数据段长度（建议 ≤ 1462 以避免 IP 分片） |
| `start_seq` | `0` | 起始包序号 |
| `send_count` | `0` | 发送包数上限，`0` = 发完即停（`file_loop=1` 时无限） |
| `data_source` | `file` | `file` = 读二进制文件；`pattern` = 生成数据 |
| `data_file` | `./data/sample.bin` | 输入文件路径（任意二进制内容） |
| `file_loop` | `0` | `1` = 循环发送；`0` = 发完即停 |
| `data_pattern` | `inc` | pattern 内容：`inc` / `random` / `zero` |
| `target_rate_mbps` | `160` | 目标有效速率；`0` = 不限速 |
| `pace_interval_ms` | `2` | 限速节奏窗口；越大高速率下越平稳精确 |
| `send_buffer_size` | `4194304` | `SO_SNDBUF` 字节数（高速率建议 ≥ 4 MB） |
| `verbose` | `0` | `1` = 逐包日志 |

## Fork 数变化

下表由 GitHub Action（[`.github/workflows/fork-tracker.yml`](.github/workflows/fork-tracker.yml)）**每天自动更新**：它把 fork 数记录到 [`data/forks_history.csv`](data/forks_history.csv) 并重写本节内容。

实时数值见顶部 **Forks** 徽章，或在本地运行：

```bash
python3 scripts/update_forks.py
```

<!-- BEGIN FORK_HISTORY (auto-generated by scripts/update_forks.py — do not edit manually) -->
| Date | Forks |
|------|-------|
| 2026-11-05 | 0 |
<!-- END FORK_HISTORY -->

> 徽章指向 `MintonLee/udp_send_data_speed_by_need` 仓库。若在其他 owner 下 fork，请相应更新徽章 URL。Action 会通过 `GITHUB_REPOSITORY` 自动识别仓库，无需手改脚本。

## 工程结构

```
udp_send_data_speed_by_need/
├── CMakeLists.txt            # CMake 构建（C++11，默认 Release）
├── build.sh                  # 一键清理 + 编译 + 拷贝可执行文件
├── run.sh                    # 用 config.ini（或自定义配置）运行
├── config.ini                # 运行时配置
├── src/
│   ├── main.cpp              # 入口：加载配置、打印横幅、信号处理
│   ├── config.{h,cpp}        # AppConfig 结构体 + INI 解析
│   ├── package.h             # FS/FE 帧协议 + 封装函数
│   └── udp_sender.{h,cpp}    # 套接字初始化、突发限速发送主循环
├── scripts/
│   ├── gen_data.py           # 生成随机二进制测试文件（如 128 MB）
│   └── update_forks.py       # 拉取 fork 数 → 更新 CSV + README 表格
├── data/                     # 测试数据（*.bin/*.dat，git 忽略）+ 历史 CSV
│   ├── sample.bin            # 生成的 128 MB 测试文件（git 忽略）
│   └── forks_history.csv     # fork 数随时间变化（自动更新）
├── .github/workflows/
│   └── fork-tracker.yml      # 每日 cron，运行 update_forks.py
└── build/                    # 本地 CMake 构建产物（git 忽略）
```

## 常见问题排查

| 现象 | 原因与处理 |
|------|-----------|
| 实测速率上不到目标 | 内核缓冲被截断——看启动日志 `实际=xxx字节`，执行 `sudo sysctl -w net.core.wmem_max=4194304`；同时确认接收端确实在收，用 `target_rate_mbps=0` 测裸吞吐上限。另注意物理上限取决于硬件与网络（如实测 `target_rate_mbps=1000` 只到 ~800+ Mbps）。 |
| 速率忽高忽低（burst 明显） | 「落后→重新对齐」分支频繁触发（CPU 竞争）。增大 `pace_interval_ms`，或减少其他负载。 |
| 接收端丢包 | UDP 无重传——加大接收端 `SO_RCVBUF` / `net.core.rmem_max`，把发送速率降到链路瓶颈以下，并确认统计里 `失败=0`。 |
| 出现 IP 分片告警 | 单包 > 1472 B——减小 `data_len`（如 1024 或 1400）。 |
| `sendto: Invalid argument` / 地址错误 | 检查 `config.ini` 里的 `dest_ip` / `dest_port`。 |
| 没发数据就退出 | `data_file` 不存在或为空——用 `scripts/gen_data.py` 生成，或切到 `data_source=pattern`。 |

## 许可

本项目采用 **MIT 协议** 发布。你可以自由使用、修改与分发，但需保留原始版权声明。
