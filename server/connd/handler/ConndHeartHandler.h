#pragma once
#include "network/AppMsg.h"
#include "network/IListener.h"
#include <functional>
#include <vector>

/**
 * @brief connd 本地心跳处理器
 *
 * 直接在 connd 内回包，不透传给 logic，降低 logic 压力。
 * 复用现有 CS_HEART_BEAT / gate.pb.h 消息格式。
 */
class ConndHeartHandler
{
public:
    // findListener: 根据 conn_id 返回对应的 IListener* (可为 nullptr)
    using FindListenerFn = std::function<IListener*(uint64_t)>;

    explicit ConndHeartHandler(FindListenerFn find_listener)
        : find_listener_(std::move(find_listener)) {}

    /**
     * @brief 处理 CS_HEART_BEAT 消息，直接回 SC 心跳
     * @param msg 收到的 AppMsg
     */
    void onHeart(const AppMsg& msg);

private:
    FindListenerFn find_listener_;
};
