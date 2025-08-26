#ifndef SIGNAL_HANDLER_H
#define SIGNAL_HANDLER_H

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <signal.h>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief 信号类型枚举
 */
enum class SignalType
{
    RELIABLE,  ///< 可靠信号，如SIGTERM、SIGINT等，一旦发送就不会丢失
    UNRELIABLE ///< 不可靠信号，如SIGUSR1、SIGUSR2等，可能会在队列满时丢失
};

/**
 * @brief 信号处理器类
 *
 * 提供对可靠信号与不可靠信号的处理功能
 * 支持注册自定义信号处理函数
 * 确保框架在收到终止信号时能够优雅退出
 * 防止信号处理导致的资源泄漏
 */
class SignalHandler
{
public:
    SignalHandler();
    ~SignalHandler();

    bool initialize();
    bool registerHandler(int signal_num, std::function<void(int)> handler, bool is_async = false);
    bool removeHandler(int signal_num);
    bool blockSignal(int signal_num);
    bool unblockSignal(int signal_num);
    bool ignoreSignal(int signal_num);
    bool resetSignalToDefault(int signal_num);
    void handleSignal(int signal_num);

    static std::string getSignalName(int signal_num);
    static SignalType getSignalType(int signal_num);

    void startSignalHandlerThread();
    void stopSignalHandlerThread();
    bool isInSignalContext() const;
    void setGlobalExitFlag(bool value);
    bool getGlobalExitFlag() const;
    int waitForSignal();
    int waitForSignal(const std::set<int> &signals, int timeout_ms = 0);

private:
    void signalHandlerThreadFunc();
    bool installSignalHandler(int signal_num);
    static void globalSignalHandler(int signal_num);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    static SignalHandler *s_instance;
};

#endif // SIGNAL_HANDLER_H
