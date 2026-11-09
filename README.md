# udp_send_data_speed_by_need

**English** | [简体中文](README.zh-CN.md)

![Language](https://img.shields.io/badge/language-C%2B%2B11-blue)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20ARM64%20%7C%20x86-lightgrey)
![Build](https://img.shields.io/badge/build-CMake%20%E2%89%A53.10-green)
![Rate](https://img.shields.io/badge/rate-%E2%89%A4160%20Mbps%20(tested)-orange)
![Forks](https://img.shields.io/github/forks/MintonLee/udp_send_data_speed_by_need?style=social&label=Forks)

A **rate-controlled, high-throughput UDP sender** that streams an arbitrary binary file from a Linux host (including embedded ARM64 boards) to a receiver at a **precise, adjustable bit rate**. It chops the file into fixed-size chunks, wraps each chunk in a tiny `FS … FE` framing protocol, and paces the output so the average rate matches the configured target.

> **Status:** tested stable at **≤ 160 Mbps** (effective payload rate). Higher rates depend on hardware and the network — setting `target_rate_mbps=1000` reached only ~800+ Mbps in practice, and **UDP packet loss occurs at high rates**. Measure the achievable speed and loss rate on your own setup.

---

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [How It Works](#how-it-works)
  - [Frame Protocol](#frame-protocol)
  - [Rate Control Principle](#rate-control-principle)
  - [End-to-End Flow](#end-to-end-flow)
- [Requirements](#requirements)
- [Quick Start](#quick-start)
- [Build](#build)
- [Run](#run)
- [Adjusting the Send Rate](#adjusting-the-send-rate)
- [Generating Test Data](#generating-test-data)
- [Configuration Reference](#configuration-reference)
- [Fork History](#fork-history)
- [Project Structure](#project-structure)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## Overview

`udp_send_data_speed_by_need` reads any binary file into memory (or generates a test pattern), splits it into `data_len`-byte chunks, frames each chunk with a start/end marker and a sequence number, and transmits it over UDP to a target `ip:port` at a precisely throttled rate. A receiver collects the packets and reassembles the original file by sequence number.

It is a general-purpose tool for **streaming large binary payloads at a controlled rate** — e.g. feeding a downstream processing program, replaying captured data, or stress-testing a link/receiver at a target bandwidth.

## Features

- **Precise rate limiting** — target rate in Mbps (effective payload); default **160 Mbps**, `0` = unlimited (raw throughput test).
- **Burst + absolute-deadline pacing** — drift-free average rate, immune to kernel timer slack (see [principle](#rate-control-principle)).
- **Simple `FS … FE` framing** — start/end markers + sequence number + length for easy, loss-aware reassembly.
- **Two data sources** — `file` (any `.bin` / `.dat`) or `pattern` (`inc` / `random` / `zero`).
- **High-rate friendly** — enlarged `SO_SNDBUF` (4 MB default) and `sendto()` retry on `EAGAIN` / `ENOBUFS`.
- **Live throughput report** — measured Mbps printed every 2 s.
- **Test-data generator** — `scripts/gen_data.py` creates a random binary file (e.g. 128 MB) for testing.
- **INI config + helper scripts** — `config.ini`, `build.sh`, `run.sh`.
- **Portable** — C++11, CMake ≥ 3.10, Linux (ARM64 / x86), no third-party libraries.

## How It Works

### Frame Protocol

Each UDP datagram carries one chunk. Wire layout (little-endian), total length `= 10 + data_len`:

| Offset | Size | Field | Description |
|-------:|-----:|-------|-------------|
| 0 | 2 | `head` | Start marker **`FS`** (`0x46 0x53`) |
| 2 | 4 | `seq` | `uint32` packet sequence number (from `start_seq`) |
| 6 | 2 | `data_len` | `uint16` payload length in bytes |
| 8 | `data_len` | `payload` | Raw file bytes |
| `8+data_len` | 2 | `tail` | End marker **`FE`** (`0x46 0x45`) |

With the default `data_len = 1024`, one packet is **1034 B** on the wire (under the 1500 B Ethernet MTU, so no IP fragmentation). The receiver validates the `FS`/`FE` markers, reads `seq` and `data_len`, and writes payloads in order to reconstruct the file. The last chunk may be shorter than `data_len` when the file size isn't a multiple of `data_len`.

### Rate Control Principle

Only one knob sets the speed: `target_rate_mbps` (measured on **effective payload bytes**, `1 Mbps = 1e6 bit/s`).

```
bytes_per_sec  = target_rate_mbps × 1e6 / 8                 # 160 Mbps → 20,000,000 B/s
burst_packets  = round(bytes_per_sec × pace_interval_ms/1000 / data_len)   # ≈ 39 at 160 Mbps
burst_interval = burst_packets × data_len / bytes_per_sec    # ≈ 2.0 ms
```

The core idea: **send a burst of packets back-to-back, then pace at the burst level using an absolute deadline.**

```
deadline = start_time
while not stopped:
    if throttling and not first_burst:
        if now - deadline > burst_interval:   # fell behind by more than one burst
            deadline = now                     # realign — do NOT burst to catch up
        else:
            sleep_until(deadline)              # absolute deadline → no drift accumulation
    deadline += burst_interval
    send burst_packets chunks (sendto, retry on EAGAIN/ENOBUFS)
```

Why this design:

1. **Absolute deadline (`sleep_until`) instead of relative sleep** — every burst is aligned to `start + N × interval`, so per-burst scheduling jitter never accumulates into long-run rate drift.
2. **Burst-level granularity instead of per-packet** — a naive "sleep after each packet" needs ~51 µs granularity at 160 Mbps, the same order as the Linux default **timer slack (~50 µs)**, which destroys sleep precision and collapses the real rate to ~50 Mbps. A ~2 ms burst interval is ~40× larger than the slack, so throttling stays accurate. `pace_interval_ms` lets you tune this trade-off.
3. **Anti-catch-up realignment** — if a burst is delayed (e.g. `sendto` blocked by a full kernel buffer), the sender resets the deadline to "now" instead of firing a catch-up burst. UDP has no flow control, so such a burst would flood the receiver and cause drops.
4. **Large `SO_SNDBUF` (4 MB)** — 20 MB/s ≈ 20k packets/s; the default socket buffer is far too small and would cause `EAGAIN`/drops. Raise the kernel cap if truncated:
   ```bash
   sudo sysctl -w net.core.wmem_max=4194304
   ```
5. **`sendto()` retry** — on `EAGAIN` / `EWOULDBLOCK` / `ENOBUFS` the sender waits 200 µs and retries (up to 3×), absorbing transient buffer-full conditions.

> **Effective vs. wire rate:** throttling counts payload bytes only, but each packet adds 10 B of framing. At 160 Mbps effective with `data_len=1024`, the wire rate is ≈ **161.6 Mbps** (~1% overhead). The startup log prints the wire rate for reference.

### End-to-End Flow

```
gen_data.py ─► data/sample.bin ─► [read whole file into memory]
                                          │
                                          ▼
                         chunk into data_len-byte blocks
                                          │
                                          ▼
              frame each block:  FS | seq | data_len | payload | FE
                                          │
                                          ▼
             burst-send N packets ─► sleep_until(absolute deadline)
                                          │            (rate control)
                                          ▼
                                   UDP ─► receiver
                                          │
                                          ▼
                       receiver reassembles file by seq
```

## Requirements

- **OS:** Linux (Ubuntu 18.04+ recommended). Runs on ARM aarch64 (e.g. RK3588, Jetson) and x86_64.
- **Compiler:** GCC / Clang with **C++11** support.
- **Build tool:** **CMake ≥ 3.10**, `make`.
- **Python 3** (only for `scripts/gen_data.py` and the fork tracker; `--seed` needs Python 3.9+).
- **No third-party C++ libraries** — POSIX sockets and the C++ standard library only.

```bash
# Debian / Ubuntu
sudo apt-get update
sudo apt-get install -y build-essential cmake python3
```

## Quick Start

```bash
# 1) generate a 128 MB random test file
python3 scripts/gen_data.py -o data/sample.bin -s 128MB

# 2) point config.ini at the receiver (edit dest_ip / dest_port if needed)
#    data_file=./data/sample.bin and target_rate_mbps=160 are already the defaults

# 3) build
./build.sh

# 4) run (Ctrl+C to stop)
./run.sh
```

## Build

**Option A — one-shot script (recommended):**

```bash
./build.sh
```

This cleans `build/`, runs CMake in `Release`, compiles, and copies the executable to the project root (`./udp_send_data_speed_by_need`).

**Option B — manual CMake:**

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"
```

The binary is `build/udp_send_data_speed_by_need`.

> **Cross-compiling for ARM64** (e.g. from an x86 host): pass a toolchain file —
> ```bash
> cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=your-aarch64-toolchain.cmake ..
> ```

## Run

Make sure `config.ini` matches your receiver (`dest_ip`, `dest_port`) and data source, then:

```bash
./run.sh                 # uses ./config.ini
./run.sh my_config.ini   # or a custom config file
```

Or run the binary directly:

```bash
./udp_send_data_speed_by_need                 # loads ./config.ini
./udp_send_data_speed_by_need path/to/cfg.ini # explicit config path
./udp_send_data_speed_by_need --help
```

Stop at any time with **Ctrl+C** (SIGINT/SIGTERM are handled for a clean shutdown and final stats).

**No data file yet?** Use pattern mode for a quick self-test — set in `config.ini`:

```ini
data_source = pattern
data_pattern = inc          # inc | random | zero
target_rate_mbps = 160
```

Typical startup output:

```
[OK] 发送缓冲: 请求=4194304字节 实际=4194304字节
[OK] UDP 发送目标 127.0.0.1:6000
[OK] 目标限速: 160 Mbps ≈ 20.00 MB/s(有效), 每突发 39 包(1034 B/包), 突发间隔 2.0 ms, 线上速率 ≈ 161.6 Mbps
[OK] 开始发送: 127.0.0.1:6000 data_len=1024 帧头"FS"/帧尾"FE" (send_count=0, Ctrl+C停止)
[进度] 2.0s 包=39000 有效=159.7 Mbps 失败=0
```

## Adjusting the Send Rate

The rate is controlled entirely by `target_rate_mbps` in `config.ini`. **Edit → save → re-run** (no rebuild needed):

```bash
# 1) edit the rate
sed -i 's/^target_rate_mbps=.*/target_rate_mbps=100/' config.ini   # or edit by hand

# 2) run again
./run.sh
```

| Goal | Setting | Effect |
|------|---------|--------|
| 100 Mbps | `target_rate_mbps=100` | burst interval grows; average payload rate ≈ 100 Mbps |
| 160 Mbps (default) | `target_rate_mbps=160` | ≈ 2.0 ms burst interval |
| Unlimited (throughput test) | `target_rate_mbps=0` | no pacing sleep, pure `sendto` pressure test |
| Coarser/finer pacing | `pace_interval_ms=1..10` | smaller = tighter but closer to timer slack; larger = smoother |
| Send a fixed number of packets | `send_count=100000` | stops after 100000 packets |

> **Rule of thumb:** `Mbps = packet_rate × data_len × 8 / 1e6`. Watch the `[进度] ... 有效 xxx Mbps` line to confirm the real rate.

## Generating Test Data

`scripts/gen_data.py` creates a random binary file of any size (stdlib only):

```bash
python3 scripts/gen_data.py                        # data/sample.bin, 128 MB (defaults)
python3 scripts/gen_data.py -o data/test.dat -s 128MB
python3 scripts/gen_data.py -s 64M --chunk 4MB     # 64 MB, 4 MB write chunks
python3 scripts/gen_data.py -s 1G --seed 42        # 1 GB, reproducible (Python 3.9+)
python3 scripts/gen_data.py -s 1048576             # size in raw bytes
```

| Option | Default | Description |
|--------|---------|-------------|
| `-o, --output` | `data/sample.bin` | Output file path |
| `-s, --size` | `128MB` | Size: `128MB` / `128M` / `1G` / `512KB` / raw bytes |
| `--chunk` | `1MB` | Write chunk size |
| `--seed` | none | RNG seed for reproducible output (needs Python 3.9+) |

Without `--seed` it uses `os.urandom` (fast, non-reproducible). Generated `data/*.bin` / `data/*.dat` files are git-ignored.

## Configuration Reference

All options live in `config.ini` (`key=value`; `#` / `;` start comments).

| Key | Default | Description |
|-----|---------|-------------|
| `dest_ip` | `127.0.0.1` | Receiver IP address |
| `dest_port` | `6000` | Receiver UDP port |
| `data_len` | `1024` | Payload bytes per packet (keep ≤ 1462 to avoid IP fragmentation) |
| `start_seq` | `0` | First packet sequence number |
| `send_count` | `0` | Max packets to send; `0` = until EOF (or infinite if `file_loop=1`) |
| `data_source` | `file` | `file` = read binary file; `pattern` = generate data |
| `data_file` | `./data/sample.bin` | Input file path (any binary content) |
| `file_loop` | `0` | `1` = loop file; `0` = stop at EOF |
| `data_pattern` | `inc` | Pattern content: `inc` / `random` / `zero` |
| `target_rate_mbps` | `160` | Target effective rate; `0` = unlimited |
| `pace_interval_ms` | `2` | Pacing quantum; larger = smoother/accurate at high rate |
| `send_buffer_size` | `4194304` | `SO_SNDBUF` bytes (≥ 4 MB recommended for high rate) |
| `verbose` | `0` | `1` = per-packet logging |

## Fork History

The table below is **updated automatically every day** by a GitHub Action ([`.github/workflows/fork-tracker.yml`](.github/workflows/fork-tracker.yml)) that records the fork count into [`data/forks_history.csv`](data/forks_history.csv) and rewrites this section.

For a live count see the **Forks** badge at the top, or run locally:

```bash
python3 scripts/update_forks.py
```

<!-- BEGIN FORK_HISTORY (auto-generated by scripts/update_forks.py — do not edit manually) -->
| Date | Forks |
|------|-------|
| 2026-11-05 | 0 |
| 2026-11-06 | 0 |
| 2026-11-07 | 0 |
| 2026-11-08 | 0 |
| 2026-11-09 | 0 |
<!-- END FORK_HISTORY -->

> The badge points at the `MintonLee/udp_send_data_speed_by_need` repo. If you fork under a different owner, update the badge URL accordingly. The action detects the repo automatically from `GITHUB_REPOSITORY`.

## Project Structure

```
udp_send_data_speed_by_need/
├── CMakeLists.txt            # CMake build (C++11, Release by default)
├── build.sh                  # one-shot clean + build + copy executable
├── run.sh                    # run with config.ini (or a custom one)
├── config.ini                # runtime configuration
├── src/
│   ├── main.cpp              # entry: load config, print banner, signal handling
│   ├── config.{h,cpp}        # AppConfig struct + INI parser
│   ├── package.h             # FS/FE frame protocol + packaging function
│   └── udp_sender.{h,cpp}    # socket setup, burst-paced send loop
├── scripts/
│   ├── gen_data.py           # generate a random binary test file (e.g. 128 MB)
│   └── update_forks.py       # fetch fork count → update CSV + README table
├── data/                     # test data (*.bin/*.dat, git-ignored) + history CSVs
│   ├── sample.bin            # generated 128 MB test file (git-ignored)
│   └── forks_history.csv     # fork-count-over-time (auto-updated)
├── .github/workflows/
│   └── fork-tracker.yml      # daily cron job that runs update_forks.py
└── build/                    # local CMake build output (git-ignored)
```

## Troubleshooting

| Symptom | Cause & fix |
|---------|-------------|
| Real rate stays below target | Kernel buffer truncated — check the startup `实际=xxx字节` line, then `sudo sysctl -w net.core.wmem_max=4194304`. Also verify the receiver is draining, test the raw ceiling with `target_rate_mbps=0`, and keep in mind the physical ceiling depends on your hardware and network (e.g. `target_rate_mbps=1000` reached ~800+ Mbps in testing). |
| Rate oscillates / bursty | The "fell behind → realign" branch is triggering often (CPU contention). Increase `pace_interval_ms`, or reduce competing load. |
| Receiver drops packets | UDP has no retransmission — enlarge the receiver's `SO_RCVBUF` / `net.core.rmem_max`, lower the send rate below the link bottleneck, and confirm `失败=0` in the stats. |
| IP fragmentation warnings | Packet > 1472 B — reduce `data_len` (e.g. 1024 or 1400). |
| `sendto: Invalid argument` / bad address | Check `dest_ip` / `dest_port` in `config.ini`. |
| Nothing sent, exits immediately | `data_file` missing or empty — generate one with `scripts/gen_data.py`, or switch to `data_source=pattern`. |

## License

This project is released under the **MIT License**. You are free to use, modify, and distribute it, provided that the original copyright notice is retained.
