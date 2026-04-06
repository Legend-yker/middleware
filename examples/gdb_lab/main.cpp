// ============================================================================
// GDB 实战练习场  —— 由易到难，共 7 关
//
// 编译: g++ -g -O0 -std=c++17 -pthread -o gdb_lab main.cpp
//
// 使用方法:
//   每次只打开 **一关** 的宏，其余保持注释。
//   例如练习第 1 关，就只保留 #define LEVEL_1 ，其余注释掉。
//   然后重新编译，用 gdb ./gdb_lab 开始调试。
// ============================================================================

// ====== 打开你要练习的关卡（同时只开一个） ======
//#define LEVEL_1
//#define LEVEL_2
//#define LEVEL_3
//#define LEVEL_4
//#define LEVEL_5
//#define LEVEL_6
#define LEVEL_7

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// 【第 1 关】 初识断点与变量查看
// ============================================================================
#ifdef LEVEL_1

int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }

int compute(int x, int y) {
    int sum = add(x, y);
    int product = mul(x, y);
    int result = sum + product;
    return result;
}

int main() {
    int a = 17;
    int b = 25;
    int c = compute(a, b);
    int d = compute(c, a);
    std::cout << "final = " << d << std::endl;
    return 0;
}

#endif // LEVEL_1

// ============================================================================
// 【第 2 关】 调用栈回溯与递归
// ============================================================================
#ifdef LEVEL_2

int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int fibonacci(int n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    int a = fibonacci(n - 1);
    int b = fibonacci(n - 2);
    return a + b;
}

int main() {
    int f5 = factorial(5);
    std::cout << "factorial(5) = " << f5 << std::endl;

    int fib7 = fibonacci(7);
    std::cout << "fibonacci(7) = " << fib7 << std::endl;
    return 0;
}

#endif // LEVEL_2

// ============================================================================
// 【第 3 关】 指针、数组与段错误
// ============================================================================
#ifdef LEVEL_3

struct Packet {
    std::uint32_t seq;
    float data[4];
    char tag[16];
};

void fill_packet(Packet *pkt, std::uint32_t seq) {
    pkt->seq = seq;
    for (int i = 0; i < 4; ++i)
        pkt->data[i] = seq * 1.1f + i;
    std::snprintf(pkt->tag, sizeof(pkt->tag), "PKT_%u", seq);
}

void process_batch(Packet *packets, int count) {
    float total = 0.0f;
    for (int i = 0; i <= count; ++i) {  // <-- 仔细看边界
        total += packets[i].data[0];
        std::cout << "  [" << i << "] tag=" << packets[i].tag
                  << " data[0]=" << packets[i].data[0] << std::endl;
    }
    std::cout << "total = " << total << std::endl;
}

Packet* create_packets(int n) {
    if (n > 100) return nullptr;
    Packet *buf = new Packet[n];
    for (int i = 0; i < n; ++i)
        fill_packet(&buf[i], i);
    return buf;
}

int main() {
    int n = 5;
    Packet *pkts = create_packets(n);
    process_batch(pkts, n);
    delete[] pkts;

    Packet *big = create_packets(200);
    fill_packet(big, 999);          // <-- 这里会怎样？
    delete[] big;

    return 0;
}

#endif // LEVEL_3

// ============================================================================
// 【第 4 关】 类、虚函数与多态
// ============================================================================
#ifdef LEVEL_4

class Sensor {
public:
    explicit Sensor(std::string name, int id) : name_(std::move(name)), id_(id) {}
    virtual ~Sensor() = default;
    virtual std::string read_data() = 0;
    int id() const { return id_; }
    const std::string& name() const { return name_; }
protected:
    std::string name_;
    int id_;
};

class TempSensor : public Sensor {
public:
    TempSensor(int id) : Sensor("Temperature", id), celsius_(22.5f) {}
    std::string read_data() override {
        celsius_ += 0.3f;
        return name_ + ": " + std::to_string(celsius_) + " C";
    }
private:
    float celsius_;
};

class PressureSensor : public Sensor {
public:
    PressureSensor(int id) : Sensor("Pressure", id), hpa_(1013.25f) {}
    std::string read_data() override {
        hpa_ -= 0.7f;
        return name_ + ": " + std::to_string(hpa_) + " hPa";
    }
private:
    float hpa_;
};

class HumiditySensor : public Sensor {
public:
    HumiditySensor(int id) : Sensor("Humidity", id), rh_(65.0f) {}
    std::string read_data() override {
        rh_ += 1.2f;
        return name_ + ": " + std::to_string(rh_) + " %RH";
    }
private:
    float rh_;
};

int main() {
    std::vector<Sensor*> sensors;
    sensors.push_back(new TempSensor(1));
    sensors.push_back(new PressureSensor(2));
    sensors.push_back(new HumiditySensor(3));

    for (int round = 0; round < 3; ++round) {
        std::cout << "=== Round " << round << " ===" << std::endl;
        for (auto *s : sensors) {
            std::string data = s->read_data();
            std::cout << "  [" << s->id() << "] " << data << std::endl;
        }
    }

    for (auto *s : sensors) delete s;
    return 0;
}

#endif // LEVEL_4

// ============================================================================
// 【第 5 关】 条件断点与 Watchpoint
// ============================================================================
#ifdef LEVEL_5

struct Counter {
    int value;
    int step;
    const char *label;
};

void tick(Counter &c) {
    c.value += c.step;
}

void mystery_modify(Counter &c, int iteration) {
    if (iteration == 37) {
        c.step = -c.step;
    }
    if (iteration == 72) {
        c.step *= 3;
    }
}

int main() {
    Counter c{0, 7, "main_counter"};

    for (int i = 0; i < 100; ++i) {
        mystery_modify(c, i);
        tick(c);
    }

    std::cout << "label = " << c.label << std::endl;
    std::cout << "final value = " << c.value << std::endl;
    std::cout << "final step  = " << c.step  << std::endl;
    return 0;
}

#endif // LEVEL_5

// ============================================================================
// 【第 6 关】 多线程调试
// ============================================================================
#ifdef LEVEL_6

std::mutex g_mtx;
int g_shared_counter = 0;

void safe_worker(int id, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        std::lock_guard<std::mutex> lock(g_mtx);
        ++g_shared_counter;
    }
    std::lock_guard<std::mutex> lock(g_mtx);
    std::cout << "worker " << id << " done" << std::endl;
}

void unsafe_worker(int id, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        ++g_shared_counter;  // 故意不加锁
    }
    std::cout << "unsafe_worker " << id << " done" << std::endl;
}

int main() {
    // ---------- 场景 A: 有锁 ----------
    g_shared_counter = 0;
    {
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
            threads.emplace_back(safe_worker, i, 10000);
        //因为主线程可能正在等待子线程结束（例如调用了 join）
        //所有主线程会在这里等子进程执行完，会卡在这
        for (auto &t : threads) t.join();
    }
    std::cout << "[safe]   expected=40000  actual=" << g_shared_counter << std::endl;

    // ---------- 场景 B: 无锁（数据竞争） ----------
    g_shared_counter = 0;
    {
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
            threads.emplace_back(unsafe_worker, i, 10000);
        for (auto &t : threads) t.join();
    }
    std::cout << "[unsafe] expected=40000  actual=" << g_shared_counter << std::endl;

    return 0;
}

#endif // LEVEL_6

// ============================================================================
// 【第 7 关】 死锁
// ============================================================================
#ifdef LEVEL_7

std::mutex mutex_a;
std::mutex mutex_b;
int resource_a = 0;
int resource_b = 0;

void worker_alpha() {
    for (int i = 0; i < 1000; ++i) {
        std::lock_guard<std::mutex> la(mutex_a);
        std::this_thread::yield();
        std::lock_guard<std::mutex> lb(mutex_b);
        resource_a += 1;
        resource_b += 2;
    }
    std::cout << "alpha done" << std::endl;
}

void worker_beta() {
    for (int i = 0; i < 1000; ++i) {
        std::lock_guard<std::mutex> lb(mutex_b);   // 注意加锁顺序
        std::this_thread::yield();
        std::lock_guard<std::mutex> la(mutex_a);
        resource_a += 10;
        resource_b += 20;
    }
    std::cout << "beta done" << std::endl;
}

int main() {
    std::thread t1(worker_alpha);
    std::thread t2(worker_beta);
    t1.join();
    t2.join();
    std::cout << "resource_a = " << resource_a << std::endl;
    std::cout << "resource_b = " << resource_b << std::endl;
    return 0;
}

#endif // LEVEL_7
