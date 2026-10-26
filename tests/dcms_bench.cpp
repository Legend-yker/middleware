// dcms_bench: DCMS 通信链路性能基准
//
// 测量项：
//   1. crc32 原始吞吐（用于隔离 E2E 校验在链路中的占比）
//   2. publish<T>() 路径：发布调用耗时 + 端到端延迟（发布前 → 订阅回调）
//   3. loan/commit 路径：同上，用于对比"少一次 memcpy"的收益
//   4. burst 压测：发布侧吞吐与慢消费者丢帧
//
// 运行方式（与 dcms_smoke 相同，需要外部 Daemon）：
//   ./release_how_to/build-x86/bin/dcms/dcms_daemon &
//   ./release_how_to/build-x86/bin/tests/dcms_bench
//
// 注意：bench 启动时会按 PID 创建独立的 ShmPool（默认池只有 32MB 且同名会
// attach 到上次残留），topic shm 与 socket 由 Daemon 自行 unlink 重建。
//
// 说明：payload 内嵌 ts（发布前时间戳），回调里用 now - ts 得到端到端延迟。
// 两条路径每帧都会 memset 一遍数据区（模拟生产数据），publish 额外多一次
// 用户缓冲区 → ShmPool 的 memcpy，loan/commit 没有这次拷贝。

#include "api/node_handle.h"
#include "api/publisher.h"
#include "api/subscriber.h"
#include "common/crc32.h"

#include "dlog/dlog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t nowUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
}

double elapsedUs(Clock::time_point t0, Clock::time_point t1) {
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

template <std::size_t N>
struct Payload {
    std::uint64_t ts;
    std::uint64_t seq;
    std::uint8_t  data[N - 16];
};
static_assert(sizeof(Payload<64>) == 64, "payload layout");

std::uint64_t percentile(std::vector<std::uint64_t> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    return v[idx];
}

void printStats(const char *label, const std::vector<std::uint64_t> &v) {
    std::printf("  %-12s p50=%8llu us  p99=%8llu us  max=%8llu us\n", label,
                static_cast<unsigned long long>(percentile(v, 0.50)),
                static_cast<unsigned long long>(percentile(v, 0.99)),
                v.empty() ? 0ULL : static_cast<unsigned long long>(
                    *std::max_element(v.begin(), v.end())));
}

// ── CRC 原始吞吐 ────────────────────────────────────────────────────────────
void crcBench(std::size_t size) {
    std::vector<std::uint8_t> buf(size, 0x5a);
    const std::size_t iters = std::max<std::size_t>(1, (64u << 20) / size);

    const auto t0 = Clock::now();
    std::uint32_t acc = 0;
    for (std::size_t i = 0; i < iters; ++i) acc ^= dcms::crc32(buf.data(), size);
    const auto t1 = Clock::now();

    const double us      = elapsedUs(t0, t1);
    const double seconds = us / 1e6;
    const double mbps    = static_cast<double>(size) * iters / 1e6 / seconds;
    std::printf("  crc32 size=%8zu B  throughput=%9.1f MB/s  per-frame=%10.1f us  (acc=%u)\n",
                size, mbps, us / iters, acc);
}

// ── 延迟测量（ping-pong：每帧等回调，避免排队影响）──────────────────────────
template <std::size_t N>
void runLatency(dcms::NodeHandle &pub_node, dcms::NodeHandle &sub_node,
                const char *size_name, int frames, std::uint32_t ring_slots,
                bool use_loan) {
    const std::string topic = std::string("bench/") + size_name +
                              (use_loan ? "_loan" : "_pub");
    auto pub = pub_node.advertise<Payload<N>>(topic, ring_slots);
    if (!pub.valid()) {
        std::printf("  [%s] advertise failed\n", topic.c_str());
        return;
    }

    std::atomic<std::uint32_t> received{0};
    std::atomic<std::uint64_t> last_lat{0};
    auto sub = sub_node.subscribe<Payload<N>>(
        topic, [&](const Payload<N> &m) {
            last_lat.store(nowUs() - m.ts, std::memory_order_relaxed);
            received.fetch_add(1, std::memory_order_release);
        });
    if (!sub.valid()) {
        std::printf("  [%s] subscribe failed\n", topic.c_str());
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    static Payload<N> msg;
    std::vector<std::uint64_t> lat, pub_cost;
    lat.reserve(frames);
    pub_cost.reserve(frames);

    const int warmup = std::min(frames / 10, 20);
    int         done = 0;
    for (int i = 0; i < frames + warmup; ++i) {
        const std::uint32_t before = received.load(std::memory_order_acquire);
        int rc = dcms::kDcmsOk;

        if (use_loan) {
            auto loan = pub.template loan<Payload<N>>();
            if (!loan.payload) {
                std::printf("  [%s] loan failed at frame %d\n", topic.c_str(), i);
                break;
            }
            std::memset(loan.payload->data, 0x5a, sizeof(loan.payload->data));
            const std::uint64_t t0 = nowUs();
            loan.payload->ts  = t0;
            loan.payload->seq = static_cast<std::uint64_t>(i);
            rc = pub.commit(loan);
            if (i >= warmup) pub_cost.push_back(nowUs() - t0);
        } else {
            std::memset(msg.data, 0x5a, sizeof(msg.data));
            const std::uint64_t t0 = nowUs();
            msg.ts  = t0;
            msg.seq = static_cast<std::uint64_t>(i);
            rc = pub.publish(msg);
            if (i >= warmup) pub_cost.push_back(nowUs() - t0);
        }
        if (rc != dcms::kDcmsOk) {
            std::printf("  [%s] publish rc=%d at frame %d\n", topic.c_str(), rc, i);
            dcms::ShmPool::instance().dump();
            break;
        }

        // 等待这一帧被订阅回调消费
        bool ok = false;
        for (int spin = 0; spin < 300000; ++spin) {  // 最长约 3s
            if (received.load(std::memory_order_acquire) > before) { ok = true; break; }
            if ((spin & 1023) == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        if (!ok) {
            std::printf("  [%s] recv timeout at frame %d\n", topic.c_str(), i);
            break;
        }
        if (i >= warmup) {
            lat.push_back(last_lat.load(std::memory_order_relaxed));
            ++done;
        }
    }

    std::printf("[%s] path=%s frames=%d ring_slots=%u\n", size_name,
                use_loan ? "loan/commit" : "publish", done, ring_slots);
    printStats("e2e", lat);
    printStats("publish()", pub_cost);
}

// ── 突发压测：发布侧吞吐 + 慢消费者丢帧 ─────────────────────────────────────
template <std::size_t N>
void runBurst(dcms::NodeHandle &pub_node, dcms::NodeHandle &sub_node,
              const char *size_name, int frames, std::uint32_t ring_slots) {
    const std::string topic = std::string("bench/burst_") + size_name;
    auto pub = pub_node.advertise<Payload<N>>(topic, ring_slots);
    if (!pub.valid()) return;

    std::atomic<std::uint32_t> received{0};
    auto sub = sub_node.subscribe<Payload<N>>(
        topic, [&](const Payload<N> &) { received.fetch_add(1, std::memory_order_relaxed); });
    if (!sub.valid()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    static Payload<N> msg;
    std::memset(&msg, 0x5a, sizeof(msg));

    const auto t0 = Clock::now();
    int         ok = 0;
    for (int i = 0; i < frames; ++i) {
        msg.ts  = nowUs();
        msg.seq = static_cast<std::uint64_t>(i);
        if (pub.publish(msg) == dcms::kDcmsOk) ++ok;
    }
    const auto   t1  = Clock::now();
    const double us  = elapsedUs(t0, t1);
    const double mbps = static_cast<double>(N) * ok / 1e6 / (us / 1e6);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::printf("[burst %s] published=%d/%d  publish-side=%.1f MB/s  avg=%.1f us/frame  received=%u (drop=%d)\n",
                size_name, ok, frames, mbps, us / frames,
                received.load(), ok - static_cast<int>(received.load()));
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // 崩溃时也不丢日志
    dlog::init("/tmp/dcms_logs", 5, /*console_echo=*/false);

    // 独立命名的内存池：避免 attach 到上次运行残留的 /dcms_pool，
    // 同时把容量从默认 32MB 提高到 128MB，容纳大块消息的多个槽位。
    char pool_name[64];
    std::snprintf(pool_name, sizeof(pool_name), "/dcms_pool_bench_%d", ::getpid());
    if (!dcms::ShmPool::instance().create(pool_name, 128u << 20)) {
        std::fprintf(stderr, "ShmPool::create(%s) failed\n", pool_name);
        return 1;
    }

    dcms::NodeHandle pub_node, sub_node;
    if (dcms::NodeHandle::create("bench_pub", pub_node) != dcms::kDcmsOk ||
        dcms::NodeHandle::create("bench_sub", sub_node) != dcms::kDcmsOk) {
        std::fprintf(stderr, "NodeHandle::create failed: is dcms_daemon running?\n");
        return 1;
    }

    std::printf("===== DCMS baseline bench =====\n");
    std::printf("pool=%zu MB  slot_payload=%u B\n",
                dcms::ShmPool::instance().poolSize() >> 20,
                dcms::kRingSlotPayloadSize);

    std::printf("\n-- CRC32 raw throughput --\n");
    crcBench(64);
    crcBench(4096);
    crcBench(1u << 20);
    crcBench(4u << 20);

    std::printf("\n-- Latency (E2E on, includes payload CRC) --\n");
    runLatency<64>(pub_node, sub_node, "64B", 2000, 64, false);
    runLatency<64>(pub_node, sub_node, "64B", 2000, 64, true);
    runLatency<4096>(pub_node, sub_node, "4KB", 2000, 64, false);
    runLatency<4096>(pub_node, sub_node, "4KB", 2000, 64, true);
    runLatency<1u << 20>(pub_node, sub_node, "1MB", 200, 8, false);
    runLatency<1u << 20>(pub_node, sub_node, "1MB", 200, 8, true);
    runLatency<4u << 20>(pub_node, sub_node, "4MB", 50, 4, false);
    runLatency<4u << 20>(pub_node, sub_node, "4MB", 50, 4, true);

    std::printf("\n-- Burst throughput --\n");
    runBurst<64>(pub_node, sub_node, "64B", 5000, 64);
    runBurst<4096>(pub_node, sub_node, "4KB", 5000, 64);
    runBurst<1u << 20>(pub_node, sub_node, "1MB", 100, 8);

    std::printf("\nbench done\n");
    dlog::shutdown();
    return 0;
}
