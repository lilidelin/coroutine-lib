// 等待任务吞吐量对比。两组均使用相同数量的实际工作线程。
// 该测试展示等待期间是否占用工作线程，不代表真实网络服务的端到端吞吐量。

#include "hook.h"
#include "ioscheduler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr int kWorkerCount = 4;
constexpr int kTaskCount = 10000;
constexpr int kSleepUs = 1000;
constexpr auto kTimeout = std::chrono::seconds(30);

class Completion {
public:
    void complete()
    {
        if (m_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cv.notify_one();
        }
    }

    bool waitForAll()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, kTimeout, [this] {
            return m_remaining.load(std::memory_order_acquire) == 0;
        });
    }

    int completed() const
    {
        return kTaskCount - m_remaining.load(std::memory_order_acquire);
    }

private:
    std::atomic<int> m_remaining{kTaskCount};
    std::mutex m_mutex;
    std::condition_variable m_cv;
};

class ThreadPool {
public:
    explicit ThreadPool(int count)
    {
        for (int i = 0; i < count; ++i)
        {
            m_workers.emplace_back([this] {
                sylar::set_hook_enable(false);
                while (true)
                {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        m_cv.wait(lock, [this] { return m_stopping || !m_tasks.empty(); });
                        if (m_stopping && m_tasks.empty())
                        {
                            return;
                        }
                        task = std::move(m_tasks.front());
                        m_tasks.pop_front();
                    }
                    task();
                }
            });
        }
    }

    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_cv.notify_all();
        for (auto& worker : m_workers)
        {
            worker.join();
        }
    }

    void submit(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_tasks.push_back(std::move(task));
        }
        m_cv.notify_one();
    }

private:
    std::deque<std::function<void()>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stopping = false;
    std::vector<std::thread> m_workers;
};

void printResult(const char* label, bool finished, int completed,
                 std::chrono::milliseconds elapsed)
{
    const auto milliseconds = elapsed.count();
    const auto throughput = milliseconds > 0
        ? static_cast<long long>(completed) * 1000 / milliseconds
        : 0;
    std::cout << '[' << label << "] workers=" << kWorkerCount
              << " submitted=" << kTaskCount
              << " completed=" << completed
              << " failed=" << (kTaskCount - completed)
              << " timed_out=" << (finished ? 0 : 1)
              << " wall_time_ms=" << milliseconds
              << " throughput_tasks_per_sec=" << throughput << '\n';
}

bool benchThreadPoolBlocking()
{
    Completion completion;
    ThreadPool pool(kWorkerCount);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kTaskCount; ++i)
    {
        pool.submit([&completion] {
            std::this_thread::sleep_for(std::chrono::microseconds(kSleepUs));
            completion.complete();
        });
    }

    const bool finished = completion.waitForAll();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    printResult("POOL+BLOCK", finished, completion.completed(), elapsed);
    return finished;
}

bool benchCoroutineHook()
{
    Completion completion;
    // use_caller=false: 只有 kWorkerCount 个工作线程参与调度。
    sylar::IOManager iom(kWorkerCount, false, "bench-coro");
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kTaskCount; ++i)
    {
        iom.scheduleLock(std::function<void()>([&completion] {
            usleep(kSleepUs);
            completion.complete();
        }));
    }

    const bool finished = completion.waitForAll();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    printResult("CORO+HOOK", finished, completion.completed(), elapsed);
    // use_caller=false 的 IOManager 需由非创建者线程执行 stop()；该线程不调度任务。
    std::thread stopper([&iom] { iom.stop(); });
    stopper.join();
    return finished;
}

} // namespace

int main()
{
    std::cout << "waiting-task benchmark: " << kWorkerCount << " workers, "
              << kTaskCount << " tasks, " << kSleepUs << "us wait each\n";
    return benchThreadPoolBlocking() && benchCoroutineHook() ? 0 : 1;
}
