// ============================================================
// Bench 1: Sleep 吞吐量（公平对比）
//
// 【控制变量】工作线程数固定 = NUM_WORKERS，任务数固定 NUM_TASKS
// 【唯一变量】是否用 hook 协程包装阻塞 sleep
//
// A 组（blocking）：线程池里的线程调真正的阻塞 sleep
//     线程卡在 sleep 期间不能干别的 → 4 线程 × 1ms sleep ≈ 4000 tasks/sec
// B 组（coroutine + hook）：调被 hook 的 sleep
//     协程挂起，线程马上去跑下一个协程 → 总耗时接近 1ms
// ============================================================

#include "ioscheduler.h"
#include "hook.h"

#include <iostream>
#include <chrono>
#include <atomic>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <vector>

static const int NUM_WORKERS = 4;
static const int NUM_TASKS   = 10000;
static const int TASK_SLEEP_US = 1000; // 每个任务 sleep 1ms

static std::atomic<int> g_done{0};
static std::chrono::high_resolution_clock::time_point g_t0;

// ============================================================
// A 组：线程池 + 真阻塞
// ============================================================
struct ThreadPool {
    std::deque<std::function<void()>> q;
    std::mutex mtx;
    std::condition_variable cv;
    bool stopped = false;
    std::vector<std::thread> workers;

    explicit ThreadPool(int n) {
        for (int i = 0; i < n; ++i) {
            workers.emplace_back([this](){
                // ★ 关键：关掉 hook，用真正的阻塞 sleep！
                sylar::set_hook_enable(false);
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lk(mtx);
                        cv.wait(lk, [this]{ return stopped || !q.empty(); });
                        if (stopped && q.empty()) return;
                        task = std::move(q.front());
                        q.pop_front();
                    }
                    task();
                }
            });
        }
    }

    void submit(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            q.push_back(std::move(f));
        }
        cv.notify_one();
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mtx);
            stopped = true;
        }
        cv.notify_all();
        for (auto &t : workers) if (t.joinable()) t.join();
    }
};

static void blocking_sleep_task() {
    // ★ 用真正的系统阻塞 sleep（std::this_thread，不走 hook）
    std::this_thread::sleep_for(std::chrono::microseconds(TASK_SLEEP_US));
    int done = g_done.fetch_add(1) + 1;
    if (done == NUM_TASKS) {
        auto t1 = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - g_t0).count();
        std::cout << "[POOL+BLOCK] threads=" << NUM_WORKERS
                  << " tasks=" << NUM_TASKS
                  << " each=" << (TASK_SLEEP_US/1000) << "ms"
                  << "  =>  total=" << ms << "ms"
                  << "  QPS≈" << (long long)NUM_TASKS * 1000 / (ms ? ms : 1)
                  << std::endl;
        long long theory = (long long)NUM_WORKERS * 1000000 / TASK_SLEEP_US;
        std::cout << "  (理论阻塞型上限: " << theory << " tasks/sec)\n";
    }
}

static void bench_thread_pool_blocking() {
    g_done = 0;
    std::cout << "\n===== A. Thread Pool + Blocking IO =====" << std::endl;
    std::cout << "workers=" << NUM_WORKERS
              << " (sleep 期间线程被阻塞，无法处理其他任务)\n";
    ThreadPool pool(NUM_WORKERS);
    g_t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_TASKS; ++i) {
        pool.submit(blocking_sleep_task);
    }
    while (g_done.load() < NUM_TASKS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ============================================================
// B 组：协程 + hook（同线程数）
// ============================================================
static void coro_sleep_task() {
    // ★ 用被 hook 的 sleep：协程挂起，线程去跑其他协程
    usleep(TASK_SLEEP_US);
    int done = g_done.fetch_add(1) + 1;
    if (done == NUM_TASKS) {
        auto t1 = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - g_t0).count();
        std::cout << "[CORO+HOOK  ] threads=" << NUM_WORKERS
                  << " tasks=" << NUM_TASKS
                  << " each=" << (TASK_SLEEP_US/1000) << "ms"
                  << "  =>  total=" << ms << "ms"
                  << "  QPS≈" << (long long)NUM_TASKS * 1000 / (ms ? ms : 1)
                  << std::endl;
    }
}

static void bench_coro_hook() {
    g_done = 0;
    std::cout << "\n===== B. Coroutine + Hook IO =====" << std::endl;
    std::cout << "workers=" << NUM_WORKERS
              << " (sleep 期间线程不阻塞，可腾挪跑其他协程)\n";

    sylar::IOManager iom(NUM_WORKERS + 1, true, "bench-coro");
    g_t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_TASKS; ++i) {
        iom.scheduleLock(std::function<void()>(coro_sleep_task));
    }
    // 用监控线程等完成后退出
    std::thread([](){
        while (g_done.load() < NUM_TASKS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::cout << "\n===== All done. Ctrl+C to exit (iom cleanup). =====" << std::endl;
        std::exit(0);
    }).detach();
}

int main() {
    std::cout << "================================================\n";
    std::cout << "  Bench1 吞吐量公平对比：" << NUM_WORKERS
              << " 线程 + " << NUM_TASKS << " 个慢 IO 任务\n";
    std::cout << "  唯一变量：是否用 hook 协程把阻塞 sleep 转成协程挂起\n";
    std::cout << "================================================\n";

    bench_thread_pool_blocking();
    bench_coro_hook();
    return 0;
}
