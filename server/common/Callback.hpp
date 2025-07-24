#pragma once

#include <string>
#include <functional>
#include <memory>
#include <google/protobuf/message.h>

typedef std::shared_ptr<::google::protobuf::Message> MessagePtr;

enum CallbackType
{
    kProtobufMessageTCallback = 1,
    kProtobufMessageTInt64Callback = 2,
};

class Callback
{
public:
    Callback(const Callback&) = delete;
    Callback& operator=(const Callback&) = delete;
    Callback(Callback&&) = delete;
    Callback& operator=(Callback&&) = delete;


    Callback(int32_t type, void *p, const google::protobuf::Descriptor *descriptor, const std::string &name)
        : type_(type), this_(p), descriptor_(descriptor), name_(name)
    {
    }

    virtual ~Callback() {};
    virtual MessagePtr onMessage(const MessagePtr &)
    {
        return nullptr;
    };
    virtual MessagePtr onMessage(int64_t, const MessagePtr &)
    {
        return nullptr;
    };

    inline const google::protobuf::Descriptor *descriptor(void)
    {
        return descriptor_;
    };

    inline const void *owner(void) const
    {
        return this_;
    };

    inline int type(void) const
    {
        return type_;
    };

    inline const std::string &name(void) const
    {
        return name_;
    }

protected:
    int32_t type_;
    void *this_;
    const google::protobuf::Descriptor *descriptor_;
    std::string name_;
};

template <typename T> class MsgCallback : public Callback
{
    static_assert(std::is_base_of<::google::protobuf::Message, T>::value, "only support protobuf");

public:
    typedef std::function<MessagePtr(const std::shared_ptr<T> &message)> ProtobufMessageTCallback;

    MsgCallback(const ProtobufMessageTCallback &callback, void *pthis = nullptr)
        : Callback(kProtobufMessageTCallback, pthis, T::descriptor(), T::default_instance().GetTypeName()),
          callback_(callback)
    {
    }

public:
    virtual MessagePtr onMessage(const MessagePtr &message) override final
    {
        std::shared_ptr<T> concrete = std::static_pointer_cast<T>(message);
        // assert(concrete != NULL);
        if (!callback_)
            return nullptr;
        return callback_(concrete);
    }

private:
    ProtobufMessageTCallback callback_;
};

template <typename T> class MsgInt64Callback : public Callback
{

    static_assert(std::is_base_of<::google::protobuf::Message, T>::value, "only support protobuf");

public:
    typedef std::function<MessagePtr(int64_t, const std::shared_ptr<T> &message)> ProtobufMessageTInt64Callback;

    MsgInt64Callback(const ProtobufMessageTInt64Callback &callback, void *pthis = nullptr)
        : Callback(kProtobufMessageTInt64Callback, pthis, T::descriptor(), T::default_instance().GetTypeName()),
          callback_(callback)
    {
    }

public:
    virtual MessagePtr onMessage(int64_t seq, const MessagePtr &message) override final
    {
        std::shared_ptr<T> concrete = std::static_pointer_cast<T>(message);
        // assert(concrete != NULL);
        if (!callback_)
            return nullptr;
        return callback_(seq, concrete);
    }

private:
    ProtobufMessageTInt64Callback callback_;
};
