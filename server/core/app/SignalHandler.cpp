#include "SignalHandler.h"
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <queue>
#include <signal.h>
#include <atomic>
#include <map>

// 静态成员初始化
SignalHandler *SignalHandler::s_instance = nullptr;

// 信号队列大小
constexpr int SIGNAL_QUEUE_SIZE = 100;

// 辅助宏：安全信号处理
#define SAFE_SIGNAL_HANDLER(signal_num)                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if (SignalHandler::s_instance)                                                                                 \
        {                                                                                                              \
            SignalHandler::s_instance->handleSignal(signal_num);                                                       \
        }                                                                                                              \
    }                                                                                                                  \
    while (0)

/**
 * @brief SignalHandler的私有实现类
 */
class SignalHandler::Impl
{
public:
    Impl() : running(false), in_signal_context(false), global_exit_flag(false)
    {
        // 初始化信号集
        sigemptyset(&block_mask);
        sigemptyset(&orig_mask);
    }

    ~Impl()
    {
        // 清理
        stop();
    }

    // 信号处理函数映射
    std::map<int, std::function<void(int)>> handlers;

    // 异步处理的信号集合
    std::set<int> async_signals;

    // 信号队列，用于异步处理
    std::queue<int> signal_queue;

    // 线程同步相关
    std::mutex signal_mutex;
    std::condition_variable signal_cv;

    // 信号掩码
    sigset_t block_mask;
    sigset_t orig_mask;

    // 控制标志
    std::atomic<bool> running;
    std::atomic<bool> in_signal_context;
    std::atomic<bool> global_exit_flag;

    // 信号处理线程
    std::thread handler_thread;

    // 启动异步处理线程
    void start()
    {
        std::lock_guard<std::mutex> lock(signal_mutex);
        if (!running)
        {
            running = true;
            handler_thread = std::thread(&SignalHandler::signalHandlerThreadFunc, SignalHandler::s_instance);
        }
    }

    // 停止异步处理线程
    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(signal_mutex);
            if (running)
            {
                running = false;
                // 确保线程能够退出等待
                signal_cv.notify_all();
            }
        }

        if (handler_thread.joinable())
        {
            handler_thread.join();
        }
    }

    // 添加信号到队列
    void pushSignal(int signal_num)
    {
        std::lock_guard<std::mutex> lock(signal_mutex);
        // 队列大小限制，防止队列过长导致内存问题
        if (signal_queue.size() < SIGNAL_QUEUE_SIZE)
        {
            signal_queue.push(signal_num);
            signal_cv.notify_one();
        }
        else
        {
            std::cerr << "信号队列已满，丢弃信号: " << signal_num << std::endl;
        }
    }

    // 从队列获取信号
    bool popSignal(int &signal_num)
    {
        std::unique_lock<std::mutex> lock(signal_mutex);
        while (running && signal_queue.empty())
        {
            signal_cv.wait(lock);
        }

        if (!running)
        {
            return false;
        }

        signal_num = signal_queue.front();
        signal_queue.pop();
        return true;
    }

    // 同步处理信号
    void handleSignalSync(int signal_num)
    {
        // 标记正在处理信号
        in_signal_context = true;

        // 执行对应的信号处理函数
        auto it = handlers.find(signal_num);
        if (it != handlers.end() && it->second)
        {
            try
            {
                it->second(signal_num);
            }
            catch (const std::exception &e)
            {
                std::cerr << "信号处理器异常: " << e.what() << std::endl;
            }
            catch (...)
            {
                std::cerr << "信号处理器未知异常" << std::endl;
            }
        }

        // 处理完成
        in_signal_context = false;
    }

    // 异步处理信号
    void handleSignalAsync(int signal_num)
    {
        pushSignal(signal_num);
    }
};

// ========== SignalHandler实现 ==========

SignalHandler::SignalHandler() : m_impl(std::make_unique<Impl>())
{
    // 保存全局实例指针，用于静态回调
    s_instance = this;
}

SignalHandler::~SignalHandler()
{
    // 停止信号处理线程
    stopSignalHandlerThread();

    // 恢复所有信号的默认处理
    for (const auto &handler : m_impl->handlers)
    {
        resetSignalToDefault(handler.first);
    }

    // 清除全局实例指针
    s_instance = nullptr;
}

bool SignalHandler::initialize()
{
    // 启动信号处理线程
    startSignalHandlerThread();

    // 默认处理以下常见信号
    registerHandler(SIGTERM, [this](int sig) {
        std::cout << "收到终止信号(SIGTERM)，准备优雅退出..." << std::endl;
        setGlobalExitFlag(true);
    });

    registerHandler(SIGINT, [this](int sig) {
        std::cout << "收到中断信号(SIGINT)，准备优雅退出..." << std::endl;
        setGlobalExitFlag(true);
    });

    registerHandler(
        SIGHUP,
        [this](int sig) {
            std::cout << "收到挂起信号(SIGHUP)，准备重新加载配置..." << std::endl;
            // 框架层会检测这个信号并调用onReload回调
        },
        true);

    registerHandler(
        SIGUSR1,
        [](int sig) {
            std::cout << "收到用户信号1(SIGUSR1)" << std::endl;
            // 用户可以根据需要自定义处理
        },
        true);

    registerHandler(
        SIGUSR2,
        [](int sig) {
            std::cout << "收到用户信号2(SIGUSR2)" << std::endl;
            // 用户可以根据需要自定义处理
        },
        true);

    // 注册SIGCHLD信号，处理子进程退出
    registerHandler(SIGCHLD, [](int sig) {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        {
            if (WIFEXITED(status))
            {
                std::cout << "子进程 " << pid << " 正常退出，状态码: " << WEXITSTATUS(status) << std::endl;
            }
            else if (WIFSIGNALED(status))
            {
                std::cout << "子进程 " << pid << " 被信号 " << WTERMSIG(status) << " 终止" << std::endl;
            }
        }
    });

    // 忽略SIGPIPE信号（防止在写入到已关闭的管道或socket时程序异常终止）
    ignoreSignal(SIGPIPE);

    return true;
}

bool SignalHandler::registerHandler(int signal_num, std::function<void(int)> handler, bool is_async)
{
    if (!handler)
    {
        return false;
    }

    // 保存处理函数
    m_impl->handlers[signal_num] = std::move(handler);

    // 如果是异步处理，添加到异步信号集合
    if (is_async)
    {
        m_impl->async_signals.insert(signal_num);
    }
    else
    {
        // 确保不在异步集合中
        m_impl->async_signals.erase(signal_num);
    }

    // 安装信号处理器
    return installSignalHandler(signal_num);
}

bool SignalHandler::removeHandler(int signal_num)
{
    auto it = m_impl->handlers.find(signal_num);
    if (it == m_impl->handlers.end())
    {
        return false;
    }

    // 移除处理函数
    m_impl->handlers.erase(it);

    // 从异步集合中移除
    m_impl->async_signals.erase(signal_num);

    // 恢复默认处理
    return resetSignalToDefault(signal_num);
}

bool SignalHandler::blockSignal(int signal_num)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, signal_num);
    return (sigprocmask(SIG_BLOCK, &mask, nullptr) == 0);
}

bool SignalHandler::unblockSignal(int signal_num)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, signal_num);
    return (sigprocmask(SIG_UNBLOCK, &mask, nullptr) == 0);
}

bool SignalHandler::ignoreSignal(int signal_num)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    return (sigaction(signal_num, &sa, nullptr) == 0);
}

bool SignalHandler::resetSignalToDefault(int signal_num)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    return (sigaction(signal_num, &sa, nullptr) == 0);
}

void SignalHandler::handleSignal(int signal_num)
{
    // 检查是否是异步处理的信号
    if (m_impl->async_signals.find(signal_num) != m_impl->async_signals.end())
    {
        m_impl->handleSignalAsync(signal_num);
    }
    else
    {
        m_impl->handleSignalSync(signal_num);
    }
}

std::string SignalHandler::getSignalName(int signal_num)
{
    static const std::map<int, std::string> signal_names = {
        {SIGHUP, "SIGHUP"},       // 终端挂起或控制进程终止
        {SIGINT, "SIGINT"},       // 键盘中断（通常是Ctrl+C）
        {SIGQUIT, "SIGQUIT"},     // 键盘退出
        {SIGILL, "SIGILL"},       // 非法指令
        {SIGTRAP, "SIGTRAP"},     // 跟踪/断点陷阱
        {SIGABRT, "SIGABRT"},     // abort()函数发出的信号
        {SIGBUS, "SIGBUS"},       // 总线错误
        {SIGFPE, "SIGFPE"},       // 浮点异常
        {SIGKILL, "SIGKILL"},     // 杀死进程（不能被捕获或忽略）
        {SIGUSR1, "SIGUSR1"},     // 用户定义信号1
        {SIGSEGV, "SIGSEGV"},     // 段违例
        {SIGUSR2, "SIGUSR2"},     // 用户定义信号2
        {SIGPIPE, "SIGPIPE"},     // 管道破裂
        {SIGALRM, "SIGALRM"},     // 闹钟
        {SIGTERM, "SIGTERM"},     // 终止
        {SIGSTKFLT, "SIGSTKFLT"}, // 栈错误
        {SIGCHLD, "SIGCHLD"},     // 子进程结束
        {SIGCONT, "SIGCONT"},     // 继续执行（从STOP开始）
        {SIGSTOP, "SIGSTOP"},     // 停止执行（不能被捕获或忽略）
        {SIGTSTP, "SIGTSTP"},     // 键盘停止
        {SIGTTIN, "SIGTTIN"},     // 后台读
        {SIGTTOU, "SIGTTOU"},     // 后台写
        {SIGURG, "SIGURG"},       // 紧急套接字条件
        {SIGXCPU, "SIGXCPU"},     // 超出CPU时间限制
        {SIGXFSZ, "SIGXFSZ"},     // 超出文件大小限制
        {SIGVTALRM, "SIGVTALRM"}, // 虚拟闹钟
        {SIGPROF, "SIGPROF"},     // 分析时钟
        {SIGWINCH, "SIGWINCH"},   // 窗口大小改变
        {SIGIO, "SIGIO"},         // I/O可用
        {SIGPWR, "SIGPWR"},       // 电源故障
        {SIGSYS, "SIGSYS"}        // 错误的系统调用
    };

    auto it = signal_names.find(signal_num);
    if (it != signal_names.end())
    {
        return it->second;
    }

    return "UNKNOWN(" + std::to_string(signal_num) + ")";
}

SignalType SignalHandler::getSignalType(int signal_num)
{
    // 不可靠信号（传统信号）：1-31
    // 可靠信号（实时信号）：SIGRTMIN - SIGRTMAX
    if (signal_num >= SIGRTMIN && signal_num <= SIGRTMAX)
    {
        return SignalType::RELIABLE;
    }
    return SignalType::UNRELIABLE;
}

void SignalHandler::startSignalHandlerThread()
{
    m_impl->start();
}

void SignalHandler::stopSignalHandlerThread()
{
    m_impl->stop();
}

bool SignalHandler::isInSignalContext() const
{
    return m_impl->in_signal_context;
}

void SignalHandler::setGlobalExitFlag(bool value)
{
    m_impl->global_exit_flag = value;
}

bool SignalHandler::getGlobalExitFlag() const
{
    return m_impl->global_exit_flag;
}

int SignalHandler::waitForSignal()
{
    // 阻塞所有信号
    sigset_t mask, old_mask;
    sigfillset(&mask);
    if (sigprocmask(SIG_BLOCK, &mask, &old_mask) == -1)
    {
        std::cerr << "sigprocmask failed: " << strerror(errno) << std::endl;
        return -1;
    }

    // 等待任何信号
    int received_signal = sigwaitinfo(&mask, nullptr);

    // 恢复原始信号掩码
    sigprocmask(SIG_SETMASK, &old_mask, nullptr);

    if (received_signal == -1)
    {
        std::cerr << "sigwaitinfo failed: " << strerror(errno) << std::endl;
        return -1;
    }

    return received_signal;
}

int SignalHandler::waitForSignal(const std::set<int> &signals, int timeout_ms)
{
    if (signals.empty())
    {
        return -2; // 无效参数
    }

    // 创建信号集
    sigset_t wait_mask;
    sigemptyset(&wait_mask);
    for (int sig : signals)
    {
        sigaddset(&wait_mask, sig);
    }

    // 临时阻塞这些信号
    sigset_t orig_mask;
    if (sigprocmask(SIG_BLOCK, &wait_mask, &orig_mask) == -1)
    {
        std::cerr << "sigprocmask failed: " << strerror(errno) << std::endl;
        return -2;
    }

    // 使用 sigtimedwait 等待信号
    siginfo_t siginfo;
    struct timespec timeout;
    struct timespec *ptimeout = nullptr;

    if (timeout_ms > 0)
    {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_nsec = (timeout_ms % 1000) * 1000000;
        ptimeout = &timeout;
    }

    int received_signal = sigtimedwait(&wait_mask, &siginfo, ptimeout);

    // 恢复原始信号掩码
    sigprocmask(SIG_SETMASK, &orig_mask, nullptr);

    if (received_signal == -1)
    {
        if (errno == EAGAIN)
        {
            return -1; // 超时
        }
        std::cerr << "sigtimedwait failed: " << strerror(errno) << std::endl;
        return -2; // 错误
    }

    return received_signal;
}

bool SignalHandler::installSignalHandler(int signal_num)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    // 设置信号处理函数
    sa.sa_handler = &SignalHandler::globalSignalHandler;

    // 在信号处理期间，阻塞所有信号
    sigfillset(&sa.sa_mask);

    // 设置标志
    sa.sa_flags = SA_RESTART; // 自动重启被中断的系统调用

    // 安装信号处理器
    if (sigaction(signal_num, &sa, nullptr) != 0)
    {
        std::cerr << "安装信号处理器失败：" << strerror(errno) << std::endl;
        return false;
    }

    return true;
}

void SignalHandler::globalSignalHandler(int signal_num)
{
    SAFE_SIGNAL_HANDLER(signal_num);
}

void SignalHandler::signalHandlerThreadFunc()
{
    int signal_num;

    std::cout << "信号处理线程已启动" << std::endl;

    while (m_impl->running)
    {
        // 从队列中获取信号
        if (m_impl->popSignal(signal_num))
        {
            // 处理信号
            auto it = m_impl->handlers.find(signal_num);
            if (it != m_impl->handlers.end() && it->second)
            {
                try
                {
                    std::cout << "异步处理信号: " << getSignalName(signal_num) << std::endl;
                    it->second(signal_num);
                }
                catch (const std::exception &e)
                {
                    std::cerr << "异步信号处理器异常: " << e.what() << std::endl;
                }
                catch (...)
                {
                    std::cerr << "异步信号处理器未知异常" << std::endl;
                }
            }
        }
    }

    std::cout << "信号处理线程已退出" << std::endl;
}
