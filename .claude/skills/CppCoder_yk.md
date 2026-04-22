# CppCoder_yk — 项目 C++ 编码规范

当用户调用 `/CppCoder_yk` 时，你扮演严格遵守以下规范的 C++ 代码审查员和实现者。对所有代码生成、修改、审查任务，**自动应用**以下所有规则，无需用户重复提醒。

---

## 1. 命名空间规范

- 每个模块有自己的顶层命名空间：`hal`、`dcms`、`dlog` 等
- 工具类放在模块子命名空间：`hal::utils`、`dcms::utils`
- **禁止** 裸 `namespace utils { }` —— 必须带模块前缀
- 跨模块复制工具类时，**必须同时修改命名空间 AND 重命名文件**

```cpp
// 正确
namespace hal::utils { class LoopThread { ... }; }
namespace dcms::utils { class LoopThread { ... }; }

// 错误
namespace utils { class LoopThread { ... }; }
```

---

## 2. 函数返回值规范

**所有公共函数必须返回 `int`**，包括"不需要返回值"的函数：

| 返回值 | 含义 |
|--------|------|
| `0`    | 执行成功 |
| `-1`   | 通用错误 |
| `-2`   | 特定错误（需注释说明） |

```cpp
// 正确
int start();
int stop();
int requestStop();
int wait();
int addSubscriberEventfd(int fd);
int clear();

// 错误 — 禁止 void 公共函数
void stop();
void requestStop();
```

---

## 3. 生命周期函数命名

所有服务类/管理类统一使用三段式生命周期：

```cpp
int init();   // 初始化资源，不启动线程
int start();  // 启动后台线程，开始工作
int stop();   // 停止线程，释放资源
```

**禁止** 使用 `run()` 作为阻塞主循环入口（`run()` 暗示同步阻塞，破坏线程管理一致性）。

```cpp
// 正确
daemon.init();
daemon.start();
// ... 主线程等待信号 ...
daemon.stop();

// 错误
daemon.run();  // 禁止
```

---

## 4. 线程使用规范

**禁止** 直接使用 `std::thread`，必须使用对应模块 utils 下的包装类：

| 模块 | 头文件 | 命名空间 |
|------|--------|----------|
| hal  | `utils/hal_thread.h` | `hal::utils` |
| dcms | `utils/dcms_thread.h` | `dcms::utils` |

### LoopThread — 周期性任务
```cpp
// 周期执行 task，直到 requestStop()
dcms::utils::LoopThread loop_thread_;
loop_thread_ = dcms::utils::LoopThread("thread_name", [this]{ doWork(); }, 10ms);
loop_thread_.requestStop();  // 返回 int
```

### SingleThread — 单次后台任务
```cpp
// 执行一次 task，析构时自动 join
dcms::utils::SingleThread single_thread_;
single_thread_ = dcms::utils::SingleThread("thread_name", [this]{ acceptLoop(); });
single_thread_.wait();  // 阻塞等待完成，返回 int
```

**不可移动对象（如 Subscriber）用 unique_ptr 持有：**
```cpp
std::unique_ptr<dcms::utils::SingleThread> recv_thread_;
recv_thread_ = std::make_unique<dcms::utils::SingleThread>("recv_topic", [this]{ receiveLoop(); });
recv_thread_.reset();  // 析构时自动 join
```

---

## 5. 头文件命名防冲突

多个模块共享相同 include 路径时，**用模块名前缀**避免碰撞：

```
src/hal/src/utils/hal_thread.h     ✓
src/dcms/src/utils/dcms_thread.h   ✓

src/hal/src/utils/thread.h         ✗  (会被另一模块的 thread.h 覆盖)
```

CMakeLists.txt 中 include 路径是 `src/` 级别，不带子目录，所以 `utils/thread.h` 在两个模块中会冲突。

---

## 6. 构建规范

- **构建目录**：始终使用 `release_how_to/build_x86` 脚本，输出到 `release_how_to/build-x86/`
- **禁止** 在项目根目录生成 build 文件夹
- `.gitignore` 必须覆盖：`release_how_to/build-x86/`、`release_how_to/build-arm/`

```bash
# 正确构建方式
cd /path/to/middleware
bash release_how_to/build_x86

# 禁止
mkdir build && cmake .. && make   # 根目录构建
```

---

## 7. 代码风格补充

- 注释只写 **WHY**，不写 WHAT（好的命名已经说明 what）
- 不写多行 docstring 或大段注释块
- 不引入超出任务需求的抽象或功能
- 错误处理只在系统边界（用户输入、外部 API、系统调用）进行
- 内部代码信任框架保证，不加防御性检查

---

## 执行方式

收到编码任务时：
1. 先检查涉及的模块，确认正确的命名空间
2. 所有新增/修改函数默认返回 `int`
3. 新增线程一律用对应模块的 `LoopThread`/`SingleThread`
4. 涉及新类时，检查是否需要 `init/start/stop` 模式
5. 涉及头文件时，检查文件名是否会与其他模块冲突
