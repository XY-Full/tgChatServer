#include <gtest/gtest.h>

#include "coroutine/CoroutineScheduler.h"
#include "network/AppMsg.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace
{

// 用最小的占位消息代替 AppMsg（这里只关心调度器的唤醒语义）
struct FakeMsg
{
    int value = 0;
};

using MsgPtr = std::shared_ptr<AppMsg>;

} // namespace

// ── 1) 唤醒早于 yield：resume 在协程挂起之前就发生 ──────────────────────
TEST(CoroutineSchedulerTest, ResumeBeforeYieldIsNotLost)
{
    CoroutineScheduler sched(2);
    sched.start();

    std::atomic<int> ok{0}, timeout{0};
    std::vector<uint64_t> ids;

    const int n = 200;
    for (int i = 0; i < n; ++i)
    {
        ids.push_back(sched.schedule([&sched, &ok, &timeout]()
        {
            const uint64_t self = CoroutineScheduler::current_id();
            CoroutineScheduler::prepare_wait();

            // 立刻从别的线程唤醒（远早于下面的 yield）
            std::thread t([&sched, self]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                sched.resume(self, std::make_shared<AppMsg>());
            });
            t.join();

            // 再 yield：唤醒若被丢弃，就会干等到 300ms 超时才返回
            const auto t0 = std::chrono::steady_clock::now();
            auto r = CoroutineScheduler::yield(std::chrono::milliseconds(300));
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();

            if (r != nullptr && ms < 100) ++ok;    // 立刻拿到唤醒结果
            else                          ++timeout; // 丢唤醒：只能等超时
        }));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (ok.load() + timeout.load() < n && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "[early-wakeup] 立即唤醒=" << ok.load() << " 丢唤醒=" << timeout.load()
              << " / " << n << std::endl;

    EXPECT_EQ(ok.load() + timeout.load(), n);
    EXPECT_EQ(timeout.load(), 0) << "出现丢失唤醒：唤醒先于 yield 时被丢弃";

    sched.stop();
}

// ── 2) 正常时序：yield 之后再 resume ────────────────────────────────────
TEST(CoroutineSchedulerTest, ResumeAfterYield)
{
    CoroutineScheduler sched(2);
    sched.start();

    std::atomic<int> done{0};
    std::vector<std::thread> wakers;

    const int n = 100;
    for (int i = 0; i < n; ++i)
    {
        const uint64_t id = sched.schedule([&done]()
        {
            CoroutineScheduler::prepare_wait();
            auto r = CoroutineScheduler::yield(std::chrono::milliseconds(3000));
            (void)r;
            ++done;
        });

        wakers.emplace_back([&sched, id]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            sched.resume(id, nullptr);
        });
    }

    for (auto& w : wakers) w.join();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (done.load() < n && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(done.load(), n);
    sched.stop();
}

// ── 3) 没人唤醒：必须靠超时返回 nullptr ─────────────────────────────────
TEST(CoroutineSchedulerTest, TimeoutReturnsNullptr)
{
    CoroutineScheduler sched(2);
    sched.start();

    std::atomic<int> done{0};
    for (int i = 0; i < 20; ++i)
    {
        sched.schedule([&done]()
        {
            CoroutineScheduler::prepare_wait();
            const auto t0 = std::chrono::steady_clock::now();
            auto r = CoroutineScheduler::yield(std::chrono::milliseconds(200));
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            EXPECT_EQ(r, nullptr);
            EXPECT_GE(ms, 150);
            EXPECT_LT(ms, 2000);
            ++done;
        });
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (done.load() < 20 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(done.load(), 20);
    sched.stop();
}

// ── 4) 业务代码抛异常：不能带崩进程，后续任务照常执行 ───────────────────
TEST(CoroutineSchedulerTest, HandlerExceptionDoesNotKillWorker)
{
    CoroutineScheduler sched(2);
    sched.start();

    std::atomic<int> done{0}, after{0};

    // 抛 std::exception
    sched.schedule([]() {
        throw std::runtime_error("boom from business code");
    });

    // 抛非标准类型
    sched.schedule([]() {
        throw 42;
    });

    // 正常任务（在异常任务之后，验证 worker 还活着）
    for (int i = 0; i < 20; ++i)
    {
        sched.schedule([&after]() { ++after; });
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (after.load() < 20 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(after.load(), 20) << "异常之后 worker 应该还在正常工作";

    sched.stop();
}

// ── 5) 等待中的协程抛异常（yield 之后被唤醒的业务代码抛）─────────────────
TEST(CoroutineSchedulerTest, ExceptionAfterResumeDoesNotHangScheduler)
{
    CoroutineScheduler sched(2);
    sched.start();

    std::atomic<int> after{0};
    std::vector<std::thread> wakers;

    for (int i = 0; i < 10; ++i)
    {
        const uint64_t id = sched.schedule([]()
        {
            CoroutineScheduler::prepare_wait();
            CoroutineScheduler::yield(std::chrono::milliseconds(500));
            throw std::runtime_error("boom after resume");
        });
        wakers.emplace_back([&sched, id]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            sched.resume(id, std::make_shared<AppMsg>());
        });
    }
    for (auto& w : wakers) w.join();

    // 异常任务之后再来一批正常任务
    for (int i = 0; i < 20; ++i)
        sched.schedule([&after]() { ++after; });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (after.load() < 20 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(after.load(), 20);
    sched.stop();
}
