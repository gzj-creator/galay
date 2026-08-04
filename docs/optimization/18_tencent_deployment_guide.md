# 腾讯云机器部署与测试完整指南

## 1. 环境要求

### 硬件要求
- **CPU**: 多核处理器（16 核或以上推荐）
  - 用于测试多生产者场景（1P-64P）
  - NUMA 架构机器更佳（用于 NUMA 亲和性测试）
- **内存**: 8GB 或以上
  - 建议 16GB+ 用于大规模并发测试
- **磁盘**: 10GB+ 可用空间
  - 存储源码、编译产物、测试结果

### 软件要求
- **操作系统**: Linux（推荐 Ubuntu 20.04+ / CentOS 8+）
- **编译器**:
  - GCC 10+ 或 Clang 12+
  - 支持 C++20 标准
- **构建工具**:
  - CMake 3.20+
  - Bazel 6.0+（可选）
- **性能分析工具**:
  - `perf`（Linux 内核性能分析器）
  - `numactl`（NUMA 控制工具）
- **Rust 工具链**（可选，用于 Crossbeam 对比）:
  - Rust 1.70+
  - Cargo

### 检查环境

```bash
# 检查 CPU 核心数
nproc
lscpu | grep -E "^CPU\(s\)|NUMA"

# 检查内存
free -h

# 检查编译器版本
gcc --version
g++ --version
clang --version

# 检查 CMake
cmake --version

# 检查 perf
perf --version

# 检查 NUMA 支持
numactl --hardware

# 检查 Rust（可选）
rustc --version
cargo --version
```

**预期输出示例**:
```
$ nproc
32

$ lscpu | grep -E "^CPU\(s\)|NUMA"
CPU(s):                          32
NUMA node(s):                    2
NUMA node0 CPU(s):               0-15
NUMA node1 CPU(s):               16-31

$ free -h
              total        used        free
Mem:           31Gi       2.0Gi        25Gi
```

---

## 2. 部署步骤

### 2.1 SSH 配置

```bash
# 本地配置 SSH 密钥（如果尚未配置）
ssh-keygen -t rsa -b 4096 -C "your_email@example.com"

# 将公钥复制到腾讯云机器
ssh-copy-id user@your-tencent-server-ip

# 测试连接
ssh user@your-tencent-server-ip
```

**SSH 配置优化**（可选，添加到 `~/.ssh/config`）:
```
Host tencent-test
    HostName your-tencent-server-ip
    User your-username
    IdentityFile ~/.ssh/id_rsa
    ServerAliveInterval 60
    ServerAliveCountMax 3
    Compression yes
```

连接:
```bash
ssh tencent-test
```

### 2.2 代码上传

**方式 1: Git Clone（推荐）**
```bash
# 在腾讯云机器上
git clone https://github.com/yourusername/galay.git
cd galay
git checkout main  # 或特定分支
```

**方式 2: rsync 上传本地代码**
```bash
# 在本地机器上
rsync -avz --exclude='.git' \
      --exclude='build' \
      --exclude='bazel-*' \
      --exclude='benchmark-results' \
      /path/to/local/galay/ \
      tencent-test:~/galay/
```

**方式 3: tar 打包上传**
```bash
# 本地打包
tar czf galay.tar.gz galay/ --exclude='.git' --exclude='build' --exclude='bazel-*'

# 上传
scp galay.tar.gz tencent-test:~/

# 远程解压
ssh tencent-test
tar xzf galay.tar.gz
cd galay
```

### 2.3 依赖安装

**Ubuntu/Debian**:
```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    git \
    linux-tools-common \
    linux-tools-generic \
    linux-tools-$(uname -r) \
    numactl \
    libnuma-dev

# 可选：安装 Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source $HOME/.cargo/env
```

**CentOS/RHEL**:
```bash
sudo yum groupinstall -y "Development Tools"
sudo yum install -y \
    cmake3 \
    git \
    perf \
    numactl \
    numactl-devel

# 创建 cmake 软链接（如果是 cmake3）
sudo ln -s /usr/bin/cmake3 /usr/bin/cmake
```

### 2.4 编译配置

**CMake 构建（推荐）**:
```bash
cd galay

# Release 构建（性能测试）
mkdir -p build/release
cd build/release
cmake ../.. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++ \
    -DBUILD_TESTING=ON

# 编译（使用所有核心）
make -j$(nproc)

# 验证编译成功
ls -lh benchmark/cpp/kernel/b8_mpsc_channel_throughput
ls -lh benchmark/cpp/kernel/b23_bounded_channel_throughput
```

**Bazel 构建（可选）**:
```bash
cd galay

# 编译所有基准测试
bazel build -c opt //benchmark/cpp/kernel:all

# 验证
bazel-bin/benchmark/cpp/kernel/b8_mpsc_channel_throughput --help
```

**预期输出**:
```
$ make -j$(nproc)
[ 10%] Building CXX object galay/kernel/CMakeFiles/galay_kernel.dir/async_channel.cc.o
[ 20%] Building CXX object galay/kernel/CMakeFiles/galay_kernel.dir/ring_buffer.cc.o
...
[100%] Built target b8_mpsc_channel_throughput
[100%] Built target b23_bounded_channel_throughput
```

### 2.5 测试运行

**快速验证**:
```bash
cd build/release/benchmark/cpp/kernel

# 单生产者测试（快速验证）
./b8_mpsc_channel_throughput --producers=1 --batch-size=64 --duration=5

# 预期输出：
# Producers: 1, Batch Size: 64
# Duration: 5s
# Throughput: ~XXX Mops/s
```

---

## 3. 测试场景

### 3.1 标准性能测试（1P-64P）

**测试目标**: 测量不同生产者数量下的吞吐量

**测试脚本** (`scripts/run_mpsc_matrix.sh`):
```bash
#!/bin/bash

# 配置
BENCHMARK="./b8_mpsc_channel_throughput"
DURATION=15
BATCH_SIZE=64
OUTPUT_DIR="../../benchmark-results/tencent-$(date +%Y%m%d-%H%M%S)"

mkdir -p "$OUTPUT_DIR"

# 生产者数量范围
PRODUCERS=(1 2 4 8 16 32 64)

echo "Starting MPSC throughput matrix test..."
echo "Output: $OUTPUT_DIR"

for P in "${PRODUCERS[@]}"; do
    echo ""
    echo "=== Testing $P Producers ==="
    
    OUTPUT_FILE="$OUTPUT_DIR/mpsc_${P}p_batch${BATCH_SIZE}.txt"
    
    $BENCHMARK \
        --producers=$P \
        --batch-size=$BATCH_SIZE \
        --duration=$DURATION \
        | tee "$OUTPUT_FILE"
    
    # 提取关键指标
    THROUGHPUT=$(grep "Throughput:" "$OUTPUT_FILE" | awk '{print $2}')
    echo "$P,$THROUGHPUT" >> "$OUTPUT_DIR/summary.csv"
    
    sleep 2  # 冷却间隔
done

echo ""
echo "Test completed. Results in: $OUTPUT_DIR"
echo "Summary:"
cat "$OUTPUT_DIR/summary.csv"
```

**运行测试**:
```bash
cd build/release/benchmark/cpp/kernel
chmod +x scripts/run_mpsc_matrix.sh
./scripts/run_mpsc_matrix.sh
```

**预期输出结构**:
```
benchmark-results/tencent-20260804-143022/
├── mpsc_1p_batch64.txt
├── mpsc_2p_batch64.txt
├── mpsc_4p_batch64.txt
├── ...
└── summary.csv
```

### 3.2 NUMA 亲和性测试

**测试目标**: 验证 NUMA 绑定对性能的影响

**单 NUMA 节点测试**:
```bash
# 绑定到 NUMA node 0
numactl --cpunodebind=0 --membind=0 \
    ./b8_mpsc_channel_throughput \
    --producers=16 \
    --batch-size=64 \
    --duration=15

# 绑定到 NUMA node 1
numactl --cpunodebind=1 --membind=1 \
    ./b8_mpsc_channel_throughput \
    --producers=16 \
    --batch-size=64 \
    --duration=15
```

**跨 NUMA 测试**:
```bash
# 不绑定（默认调度）
./b8_mpsc_channel_throughput \
    --producers=32 \
    --batch-size=64 \
    --duration=15

# 对比 NUMA 统计
numastat -c qemu-system-x86  # 或相应进程名
```

**NUMA 测试脚本** (`scripts/run_numa_test.sh`):
```bash
#!/bin/bash

BENCHMARK="./b8_mpsc_channel_throughput"
PRODUCERS=16
BATCH_SIZE=64
DURATION=15

echo "=== No NUMA binding ==="
$BENCHMARK --producers=$PRODUCERS --batch-size=$BATCH_SIZE --duration=$DURATION

echo ""
echo "=== NUMA node 0 ==="
numactl --cpunodebind=0 --membind=0 \
    $BENCHMARK --producers=$PRODUCERS --batch-size=$BATCH_SIZE --duration=$DURATION

echo ""
echo "=== NUMA node 1 ==="
numactl --cpunodebind=1 --membind=1 \
    $BENCHMARK --producers=$PRODUCERS --batch-size=$BATCH_SIZE --duration=$DURATION

echo ""
echo "=== Interleaved memory ==="
numactl --interleave=all \
    $BENCHMARK --producers=$PRODUCERS --batch-size=$BATCH_SIZE --duration=$DURATION
```

### 3.3 压力测试（24 小时）

**测试目标**: 验证长时间运行稳定性、内存泄漏、性能衰减

**长时运行脚本** (`scripts/run_stress_test.sh`):
```bash
#!/bin/bash

BENCHMARK="./b8_mpsc_channel_throughput"
PRODUCERS=16
BATCH_SIZE=64
DURATION=86400  # 24 hours
OUTPUT="stress_test_$(date +%Y%m%d-%H%M%S).log"

echo "Starting 24-hour stress test..."
echo "Output: $OUTPUT"

# 后台运行，记录资源使用
(
    while true; do
        echo "=== $(date) ==="
        ps aux | grep b8_mpsc_channel_throughput | grep -v grep
        free -h
        sleep 600  # 每 10 分钟记录一次
    done
) > "resource_monitor_$OUTPUT" &

MONITOR_PID=$!

# 运行测试
$BENCHMARK \
    --producers=$PRODUCERS \
    --batch-size=$BATCH_SIZE \
    --duration=$DURATION \
    | tee "$OUTPUT"

# 停止监控
kill $MONITOR_PID

echo "Stress test completed."
```

**运行**:
```bash
nohup ./scripts/run_stress_test.sh &
tail -f stress_test_*.log
```

### 3.4 perf 性能分析

**CPU 热点分析**:
```bash
# 记录性能事件（运行 30 秒）
sudo perf record -g -F 999 -- \
    ./b8_mpsc_channel_throughput \
    --producers=16 \
    --batch-size=64 \
    --duration=30

# 生成报告
sudo perf report

# 或生成火焰图友好格式
sudo perf script > perf.data.script
```

**缓存未命中分析**:
```bash
sudo perf stat -e cache-references,cache-misses,L1-dcache-load-misses \
    ./b8_mpsc_channel_throughput \
    --producers=16 \
    --batch-size=64 \
    --duration=15
```

**预期 perf stat 输出**:
```
 Performance counter stats for './b8_mpsc_channel_throughput ...':

   12,345,678,900      cache-references
      123,456,789      cache-misses              #    1.00% of all cache refs
   98,765,432,100      L1-dcache-load-misses

      15.002345678 seconds time elapsed
```

### 3.5 Crossbeam 对比测试

**编译 Rust Crossbeam 基准**:
```bash
cd benchmark/cpp/kernel/compare/rust-channel
cargo build --release --bin mpsc_paired

# 运行对比测试
../../../../../../bazel-bin/benchmark/cpp/kernel/b8_mpsc_channel_throughput \
    --producers=16 --batch-size=64 --duration=15

./target/release/mpsc_paired 16 64 15
```

**对比脚本** (`compare/run_crossbeam_comparison.sh`):
```bash
#!/bin/bash

PRODUCERS=(1 2 4 8 16 32)
BATCH_SIZE=64
DURATION=15

echo "Producer,Galay(Mops/s),Crossbeam(Mops/s),Ratio" > comparison.csv

for P in "${PRODUCERS[@]}"; do
    # Galay
    GALAY_TP=$(./b8_mpsc_channel_throughput \
        --producers=$P --batch-size=$BATCH_SIZE --duration=$DURATION \
        | grep "Throughput:" | awk '{print $2}')
    
    # Crossbeam
    CROSS_TP=$(./compare/rust-channel/target/release/mpsc_paired \
        $P $BATCH_SIZE $DURATION \
        | grep "Throughput:" | awk '{print $2}')
    
    RATIO=$(echo "scale=2; $GALAY_TP / $CROSS_TP" | bc)
    
    echo "$P,$GALAY_TP,$CROSS_TP,$RATIO" | tee -a comparison.csv
done

echo ""
echo "Comparison completed. Results in comparison.csv"
```

---

## 4. 结果分析

### 4.1 性能数据解读

**吞吐量指标**:
- **Mops/s** (Million operations per second): 百万次操作/秒
- **延迟**: P50/P99/P999 延迟（如果测试提供）

**典型性能范围**（参考）:
| 生产者数 | 预期吞吐量 (Mops/s) | 备注 |
|---------|---------------------|------|
| 1P      | 200-400             | 单生产者基线 |
| 2P      | 350-700             | 接近线性扩展 |
| 4P      | 600-1200            | 竞争开始显现 |
| 8P      | 900-1800            | 明显竞争 |
| 16P     | 1200-2400           | 重度竞争 |
| 32P+    | 1500-3000           | 饱和/衰减 |

**扩展效率计算**:
```
扩展效率 = (N 生产者吞吐量) / (1 生产者吞吐量 × N) × 100%
```

示例:
- 1P: 300 Mops/s
- 8P: 1800 Mops/s
- 扩展效率 = 1800 / (300 × 8) = 75%

**理想目标**:
- 1P-4P: 扩展效率 > 80%
- 8P-16P: 扩展效率 > 60%
- 32P+: 扩展效率 > 40%

### 4.2 瓶颈识别

**症状 1: 吞吐量随生产者增加而下降**
```
1P: 300 Mops/s
2P: 250 Mops/s  ← 异常
4P: 180 Mops/s
```

**可能原因**:
- 锁竞争过度（检查 `perf` 中的 `futex` 系统调用）
- False sharing（相邻缓存行竞争）
- 内存分配瓶颈

**诊断**:
```bash
# 检查锁等待
sudo perf record -e syscalls:sys_enter_futex -a -g -- \
    ./b8_mpsc_channel_throughput --producers=8 --duration=10

sudo perf report
```

**症状 2: NUMA 跨节点性能显著下降**
```
Node 0 only: 1800 Mops/s
No binding:  1200 Mops/s  ← 33% 下降
```

**可能原因**:
- 远程内存访问延迟
- 缓存一致性协议开销

**诊断**:
```bash
numastat -c <pid>
```

**症状 3: 缓存未命中率高**
```bash
sudo perf stat -e cache-misses ./benchmark
# cache-misses: 10%+  ← 过高
```

**可能原因**:
- 数据结构布局不佳
- 工作集超过 LLC 大小
- 预取策略不当

### 4.3 优化建议

**针对锁竞争**:
- 使用无锁数据结构（已采用）
- 减少原子操作频率（批处理）
- 使用 `std::atomic` 而非 mutex

**针对 NUMA**:
- 绑定生产者到特定 NUMA 节点
- 使用 NUMA-aware 内存分配
- 考虑 per-NUMA-node 队列设计

**针对缓存未命中**:
- 对齐关键数据结构到缓存行（64 字节）
- 避免 false sharing（padding）
- 预取即将访问的数据

**批处理优化**:
- 增加批大小以分摊同步成本
- 测试不同 batch size（16, 32, 64, 128）
- 权衡延迟与吞吐量

---

## 5. 故障排查

### 5.1 编译错误

**错误: `cmake: command not found`**
```bash
# Ubuntu
sudo apt install cmake

# CentOS
sudo yum install cmake3
sudo ln -s /usr/bin/cmake3 /usr/bin/cmake
```

**错误: `C++ compiler does not support C++20`**
```bash
# 检查编译器版本
g++ --version  # 需要 10+

# 升级 GCC (Ubuntu)
sudo apt install gcc-11 g++-11
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-11 100
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-11 100

# 或指定编译器
cmake .. -DCMAKE_CXX_COMPILER=g++-11
```

**错误: `undefined reference to std::expected`**
```bash
# 确保使用 C++23 或提供了 std::expected polyfill
# 检查 CMakeLists.txt 中的 CMAKE_CXX_STANDARD
grep CMAKE_CXX_STANDARD CMakeLists.txt
```

### 5.2 运行时错误

**错误: `Segmentation fault` 启动时**

**诊断**:
```bash
# 使用 gdb 调试
gdb ./b8_mpsc_channel_throughput
(gdb) run --producers=1 --batch-size=64 --duration=5
(gdb) bt  # 查看堆栈
```

**可能原因**:
- 未初始化的指针
- 栈溢出（增加栈大小: `ulimit -s unlimited`）
- 内存对齐问题

**错误: `Permission denied` 运行 perf**

```bash
# 临时允许
sudo sysctl -w kernel.perf_event_paranoid=-1

# 永久配置
echo "kernel.perf_event_paranoid = -1" | sudo tee -a /etc/sysctl.conf
sudo sysctl -p
```

**错误: `Cannot allocate memory`**

```bash
# 检查可用内存
free -h

# 检查 ulimit
ulimit -a

# 增加限制
ulimit -v unlimited
```

### 5.3 性能异常

**问题: 吞吐量远低于预期（< 100 Mops/s）**

**检查清单**:
1. **是否为 Release 构建**?
   ```bash
   file ./b8_mpsc_channel_throughput
   # 应包含 "not stripped" 或优化符号
   
   # 重新编译
   cmake .. -DCMAKE_BUILD_TYPE=Release
   ```

2. **CPU 频率调节**?
   ```bash
   cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
   # 应为 "performance"
   
   # 设置性能模式
   echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
   ```

3. **系统负载**?
   ```bash
   top
   # 检查其他进程是否占用 CPU
   ```

4. **超线程影响**?
   ```bash
   lscpu | grep -E "Thread|Core"
   # 物理核心数 vs 逻辑核心数
   
   # 禁用超线程（需要 root）
   echo off | sudo tee /sys/devices/system/cpu/smt/control
   ```

**问题: 性能随时间衰减**

**诊断**:
```bash
# 监控温度
sensors  # 需要 lm-sensors 包

# 检查 CPU throttling
dmesg | grep -i throttl

# 监控频率
watch -n 1 "cat /proc/cpuinfo | grep MHz"
```

**解决**:
- 改善散热
- 降低测试强度
- 使用更短的测试周期

### 5.4 NUMA 问题

**问题: 无法使用 numactl**

```bash
# 检查 NUMA 支持
numactl --hardware

# 如果提示 "No NUMA support available"
# 可能原因：
# 1. 单 NUMA 节点系统（正常，跳过 NUMA 测试）
# 2. NUMA 未在内核中启用（检查 /boot/config-$(uname -r) | grep NUMA）
```

**问题: NUMA 绑定无效**

**验证绑定**:
```bash
# 启动测试，获取 PID
./b8_mpsc_channel_throughput --producers=16 --duration=300 &
PID=$!

# 检查 NUMA 策略
numactl --show | grep cpubind

# 检查线程分布
ps -eLo pid,tid,psr,comm | grep $PID
# psr 列显示 CPU 编号
```

---

## 附录

### A. 完整测试检查清单

- [ ] 环境配置完成（编译器、工具）
- [ ] 代码成功编译（Release 模式）
- [ ] 快速验证通过（1P 测试）
- [ ] 标准性能矩阵（1P-64P）
- [ ] NUMA 测试（如适用）
- [ ] perf 分析完成
- [ ] Crossbeam 对比（如需要）
- [ ] 压力测试（可选）
- [ ] 结果已保存并备份

### B. 常用命令速查

```bash
# 编译
cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)

# 快速测试
./b8_mpsc_channel_throughput --producers=8 --batch-size=64 --duration=10

# NUMA 测试
numactl --cpunodebind=0 --membind=0 ./b8_mpsc_channel_throughput ...

# perf 分析
sudo perf record -g ./b8_mpsc_channel_throughput ...
sudo perf report

# 性能模式
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# 监控
htop
watch -n 1 'ps aux | grep b8_mpsc'
```

### C. 结果上传与分享

```bash
# 打包结果
cd benchmark-results
tar czf tencent-results-$(date +%Y%m%d).tar.gz tencent-*/

# 下载到本地
scp tencent-test:~/galay/benchmark-results/tencent-results-*.tar.gz .

# 或使用 rsync
rsync -avz tencent-test:~/galay/benchmark-results/ ./local-results/
```

---

## 总结

本指南涵盖了在腾讯云机器上部署和测试 galay 并发通道的完整流程：

1. **环境准备**: 确保硬件、软件、工具链满足要求
2. **部署**: SSH 配置、代码上传、依赖安装、编译
3. **测试**: 标准性能、NUMA、压力、perf、Crossbeam 对比
4. **分析**: 性能指标解读、瓶颈识别、优化建议
5. **排查**: 编译、运行时、性能、NUMA 常见问题

按照本指南操作，可以系统性地评估 galay 在高性能服务器上的表现，识别瓶颈，并为后续优化提供数据支持。

**下一步**:
- 根据测试结果更新 `docs/optimization/` 中的性能分析文档
- 将关键发现记录到 `MEMORY.md`
- 针对识别的瓶颈启动优化迭代（参考 Phase 1-3 计划）
