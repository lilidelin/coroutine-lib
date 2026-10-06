#include "timer.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

namespace sylar {

class TestTimerManager : public TimerManager
{
public:
    using TimerManager::addTimer;
};

class TimerTestAccess
{
public:
    static std::shared_ptr<Timer> makeTimer(TestTimerManager* manager,
                                            std::chrono::time_point<std::chrono::system_clock> deadline,
                                            std::function<void()> callback)
    {
        auto timer = std::shared_ptr<Timer>(new Timer(20, std::move(callback), false, manager));
        timer->m_next = deadline;
        return timer;
    }
};

} // namespace sylar

int main()
{
    constexpr int kTimerCount = 128;
    sylar::TestTimerManager manager;
    std::atomic<int> fired{0};

    // 所有 timer 都使用严格相同的截止时间，回调对象仍必须全部保留。
    const auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(20);
    std::vector<std::shared_ptr<sylar::Timer>> timers;
    timers.reserve(kTimerCount);
    for (int i = 0; i < kTimerCount; ++i)
    {
        auto timer = sylar::TimerTestAccess::makeTimer(&manager, deadline, [&fired] {
            fired.fetch_add(1, std::memory_order_relaxed);
        });
        manager.addTimer(timer);
        timers.push_back(std::move(timer));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    std::vector<std::function<void()>> callbacks;
    manager.listExpiredCb(callbacks);
    for (auto& callback : callbacks)
    {
        callback();
    }

    if (static_cast<int>(callbacks.size()) != kTimerCount ||
        fired.load(std::memory_order_relaxed) != kTimerCount)
    {
        std::cerr << "expected " << kTimerCount << " timer callbacks, got "
                  << callbacks.size() << " callbacks and " << fired.load()
                  << " executions\n";
        return 1;
    }

    std::cout << "same-deadline timer test passed: " << fired.load() << " callbacks\n";
    return 0;
}
