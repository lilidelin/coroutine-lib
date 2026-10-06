# AGENTS.md

coroutine-lib 是分阶段实现的 Linux C++17 有栈协程学习项目。先读 [README.md](README.md) 了解项目背景；修改 `fiber_lib/6hook/` 前，按下文阅读对应实现。本文适用于整个仓库，运行时约束主要针对 `6hook`，不要默认早期阶段具有相同能力。

## 仓库布局与修改范围

```text
fiber_lib/
  1thread/       线程与同步基础
  2fiber/        ucontext 协程
  3scheduler/    协程调度
  4timer/        定时器
  5iomanager/    epoll 与调度集成
  6hook/         系统调用 hook 与完整运行时；本轮工作的主要入口
    benchmark/   sleep 吞吐、HTTP 服务及延迟对比
  epoll/         独立 epoll 示例
  libevent/      独立 libevent 示例
pics/            项目图片
```

- 各阶段包含独立源码副本。修改 `6hook` 不自动同步 `1thread` 至 `5iomanager`；跨阶段调整必须说明涉及哪些示例及原因。
- 先运行 `git status --short --branch`，保留已有修改与未跟踪文件。不要覆盖仓库内已有的 `test` 可执行文件，也不要清理用户的构建目录或编辑器配置。
- 搜索、构建和运行在 Linux 项目目录执行。通过 Windows SSH 工作时复用已授权的连接配置；不要把主机地址、私钥路径或凭据写入仓库文档。

## 6hook 阅读入口

| 修改主题 | 先读源码 | 联动检查 |
| --- | --- | --- |
| 系统调用拦截 | [hook.h](fiber_lib/6hook/hook.h)、[hook.cpp](fiber_lib/6hook/hook.cpp) | `fd_manager`、`IOManager`、定时器 |
| fd 属性与生命周期 | [fd_manager.h](fiber_lib/6hook/fd_manager.h)、[fd_manager.cpp](fiber_lib/6hook/fd_manager.cpp) | `socket/accept/close/fcntl/ioctl/setsockopt` |
| 协程切换与调度 | [fiber.cpp](fiber_lib/6hook/fiber.cpp)、[scheduler.cpp](fiber_lib/6hook/scheduler.cpp) | 对应头文件、线程局部状态、事件持有的协程 |
| epoll 注册与唤醒 | [ioscheduler.h](fiber_lib/6hook/ioscheduler.h)、[ioscheduler.cpp](fiber_lib/6hook/ioscheduler.cpp) | `do_io`、事件计数、停止条件 |
| 超时与取消 | [timer.h](fiber_lib/6hook/timer.h)、[timer.cpp](fiber_lib/6hook/timer.cpp) | `do_io`、`connect_with_timeout`、`tickle` |
| 示例与性能 | [main.cpp](fiber_lib/6hook/main.cpp)、[benchmark/CMakeLists.txt](fiber_lib/6hook/benchmark/CMakeLists.txt) | 实际请求协议、hook 开关、线程数及退出方式 |

运行路径：系统调用包装器 → 原始函数尝试 I/O → `EAGAIN` 时注册事件及可选定时器 → 当前协程 `yield()` → epoll 或超时回调通过 `scheduleLock()` 恢复协程 → 重试或返回错误。`connect` 单独处理 `EINPROGRESS`，恢复后检查 `SO_ERROR`。

## 构建与运行

需要 Linux、支持 C++17 的编译器、CMake 3.10+、pthread 与动态符号解析支持；实现依赖 `ucontext`、epoll 和 `dlsym(RTLD_NEXT, ...)`。没有原生 Windows 支持承诺。

从仓库根目录执行，首次配置选用未占用的构建目录：

```sh
cmake -S fiber_lib/6hook -B fiber_lib/6hook/build/agent-debug \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_6HOOK_BENCHMARKS=ON
cmake --build fiber_lib/6hook/build/agent-debug -j2
```

- 目标包括 `sylar_hook_lib`、`main_hook_demo`、`bench1_sleep`、`bench2_echo_servers` 和 `bench3_latency`。关闭 `BUILD_6HOOK_BENCHMARKS` 可只构建库与 demo。
- 优先用 CMake；旧 [readme.txt](fiber_lib/6hook/readme.txt) 只有简略的 `g++` 命令。不要把旧二进制当作本次源码的验证结果。
- Debug 保留断言，适合行为检查；性能比较另建 Release 目录，不混用两种构建结果。
- 当前 CMake 没有 `enable_testing()` / `add_test()`；构建通过或 CTest 无测试不能声称回归测试通过。

以下是按需运行的示例，不是每次修改必须执行的测试套件：

```sh
# sleep 对比；超时退出应记录为未正常完成
timeout 30s fiber_lib/6hook/build/agent-debug/benchmark/bench1_sleep

# 终端 A：0=两组，1=阻塞组，2=协程组；端口分别为 8081、8082
timeout 120s fiber_lib/6hook/build/agent-debug/benchmark/bench2_echo_servers 0
# 终端 B：需要上述服务存活；客户端会创建最高 1024 并发线程
timeout 60s fiber_lib/6hook/build/agent-debug/benchmark/bench3_latency 8081 8082
```

`main_hook_demo` 在 `0.0.0.0:8080` 提供 HTTP 示例并持续运行。服务型示例运行前检查端口及影响范围，设置运行期限，只结束本次启动的进程。HTTP demo 返回成功不足以证明阻塞 socket 的 hook 路径正确：它显式设置了用户非阻塞模式。

## Hook 与 fd 约束

- **保持 C ABI 与原始函数通路。** 新增拦截函数时同步 `hook.h` 的函数指针声明、`HOOK_FUN` 列表和包装实现；签名、返回值和可变参数类型必须匹配系统接口。底层直接调用使用对应 `*_f`，避免递归进入自身包装器。
- **Hook 开关是线程局部状态。** 当前 `Scheduler::run()` 开启 hook；需要挂起的 sleep/socket 操作必须有有效的 `IOManager` 与任务协程。不要只在普通线程或纯 `Scheduler` 上开启开关就假设可以异步等待。
- **区分用户非阻塞和内部非阻塞。** `FdCtx` 用 `m_userNonblock` 表达调用方选择，用 `m_sysNonblock` 表达内部非阻塞策略。用户选择非阻塞时，I/O 不应偷偷等待事件；调整 `fcntl` / `ioctl` 时同时验证底层标志与调用方看到的标志。
- **保留系统调用结果。** 检查 `EINTR` 重试、`EAGAIN` 等待、EOF、短读短写、错误返回及 `errno`；通用包装器不应擅自把一次读写变成读满或写满。
- **连接就绪不等于成功。** 修改连接路径时保留 `SO_ERROR` 检查，覆盖立即成功、连接拒绝、等待超时及事件注册失败。
- **fd 元数据跟随实际生命周期。** 修改创建、接收、复制或关闭路径时检查 `FdMgr` 的登记和移除，覆盖 fd 数值被复用的情况；不要假设 `dup` 或跨线程关闭已经完整支持。
- **时间单位明确。** 当前超时以毫秒保存，`uint64_t(-1)` 表示无限等待；`usleep` / `nanosleep` 转换会截断亚毫秒部分。改变精度、零值或默认超时语义时同步调用方与测试。

## 协程、事件与退出

- **遵守协程状态。** `resume()` 要求 `READY`；`yield()` 接受 `RUNNING` 或 `TERM`；`reset()` 要求有独立栈且已 `TERM`。不能释放仍可能恢复的协程栈，也不能让多个线程同时恢复同一协程。
- **明确挂起后的所有者。** 等待 I/O 或定时器时必须有对象持有协程；恢复、取消和退出路径应释放相应引用。使用 `weak_ptr` 的条件定时器只解决存活判断，不自动解决数据竞争。
- **事件删除与取消不同。** `delEvent()` 移除事件而不调度等待者；`cancelEvent()` / `cancelAll()` 移除并调度等待者。不要用删除替代需要唤醒的取消。
- **事件计数与注册状态一致。** 同一 fd 的同一方向不能重复注册；注册、触发、取消和失败路径要同步维护 epoll、`FdContext` 与 `m_pendingEventCount`。当前使用 `EPOLLET`，新增显式非阻塞循环要处理读写到 `EAGAIN` 后的重新等待。
- **就绪与超时只能完成一次等待。** 修改竞争路径时覆盖两者同时发生、取消后回调已入队和 fd 关闭后的回调；不能只依靠一个未同步的标志证明线程安全。
- **停止需要排空工作。** `IOManager::stopping()` 同时检查定时器、待处理事件和调度器任务。长驻 accept、循环定时器或遗留事件都可能阻止退出；新增资源必须有结束路径。
- **锁的保护范围要具体。** 区分任务队列锁、单个协程锁、fd 容器锁、事件锁与定时器锁；不要新增持有共享容器锁调用用户回调或跨协程挂起的路径。

## 当前实现中需要核查的限制

这些是源码现状与修改时的检查点，不是要求保持不变的设计：

- `Timer::Comparator` 只比较到期时间；相同时间的不同定时器可能被 `std::set` 视为等价。修改定时器时覆盖相同截止时间，不能宣称回调已保证不丢失。
- `timer_info::cancelled` 是普通整数；`FdCtx` 的状态访问也没有独立同步。容器上有锁不等于所有状态都线程安全。
- `ioctl(FIONBIO)` 直接转发请求；需要核查关闭用户非阻塞时是否破坏内部非阻塞状态。`F_DUPFD*` 当前只转发，没有同步复制 fd 元数据。
- `m_isClosed` 有读取接口，但当前关闭流程没有将旧上下文标记为关闭；`close` 在 hook 关闭时直接调用原函数。修改关闭与 fd 复用必须测试旧等待者和残留元数据。
- 定时器当前使用 `system_clock`；调整时钟来源时同时考虑绝对截止时间、重置与时钟回退逻辑。
- `bench1_sleep` 包含监控线程 `std::exit(0)` 路径；它不适合作为正常析构和退出的证明。性能结论必须核对实际线程数、完成任务数、错误数与负载参数，不能直接引用源码注释中的估算。

## 验证与交付

- 文档修改检查路径、目标名、命令和 diff；不默认运行网络压测。代码修改先构建受影响目标，再执行对应行为验证。
- Hook 修改覆盖：关闭 hook 的原始通路、用户非阻塞通路、协程等待与恢复、错误及超时。使用受控本地 socket；若使用未拦截的 `socketpair`，需要显式登记待测 fd。
- 调度、事件或定时器修改覆盖：单线程与多线程、同截止时间多个 timer、取消与就绪竞争、关闭等待中的 fd、正常排空退出。缺少现成测试时为变更补可独立运行的最小回归用例，并记录构建及运行命令。
- 性能修改先通过行为验证，再在相同编译模式、线程数、请求和并发条件下比较；报告机器环境、参数、失败数与测量结果，不把 benchmark 当作正确性断言。
- 只报告实际执行过的检查，区分编译成功、运行成功、超时及未验证内容。已有告警如实记录，不为本次小改动顺便关闭全局告警。
- 提交前查看 `git diff --check` 与 `git status --short`；新文档尚未跟踪时另查内容和格式，不把空 diff 当作已检查。不要自动提交、推送或修改无关源码。

## 编码与文档规范

- 沿用 `sylar` 命名空间、现有 `.h/.cpp` 划分和邻近代码风格；不做无关的全文件格式化或跨阶段去重。
- 接口变更同步声明、定义、demo 与 benchmark 调用方。新增源文件或可执行目标时同步所属 CMake 文件。
- 注释说明线程要求、所有权、时间单位、返回值与失败行为；避免复述语句或把期望行为写成已经验证的事实。
- 修改影响构建、用法或生命周期时更新对应说明；同一事实尽量只有一个维护位置，其余位置链接过去。Markdown 使用 UTF-8，文件以一个换行结束。
- 更新本文时以当前源码为依据；删除过期限制，保留可执行规则，不复制其他项目的包管理、发布或覆盖率制度。
