#pragma once

#include "FuncResolver.hpp"
#include "Callback.hpp"
#include "Log.h"

class EventPump;

template <typename T, std::size_t N> struct EventRegisterImpl
{
    template <typename MemberFunc>
    static void registEvent(EventPump *, MemberFunc, typename MemberFuncArgs<MemberFunc>::OwnerType *)
    {
    }

    template <typename MemberFunc>
    static void registEvent(EventPump *, const char *, MemberFunc, typename MemberFuncArgs<MemberFunc>::OwnerType *)
    {
    }
};

class EventPump
{
    typedef std::vector<std::shared_ptr<Callback>> CallbackVec;
    typedef std::unordered_map<const google::protobuf::Descriptor *, CallbackVec> Callbacks;
    typedef std::unordered_map<void *, Callbacks> ModuleCallbacks;

public:
    EventPump(void)
    {
    }
    virtual ~EventPump(void)
    {
    }

    template <typename MemberFunc>
    void registerEventHandle(MemberFunc mfn, typename MemberFuncArgs<MemberFunc>::OwnerType *owner)
    {
        using Arg1Type = typename MemberFuncArgs<MemberFunc>::Arg1Type;
        using MessageType = typename std::remove_const_sharedptr_ref<Arg1Type>::type;
        static_assert(std::is_base_of<::google::protobuf::Message, MessageType>::value,
                      "the second argv only support protobuf");
        EventRegisterImpl<MessageType, MemberFuncArgs<MemberFunc>::ArgCount>::registEvent(this, mfn, owner);
    }

    template <typename MemberFunc>
    void registerEventHandle(const char *cmd, MemberFunc mfn, typename MemberFuncArgs<MemberFunc>::OwnerType *owner)
    {
        using Arg1Type = typename MemberFuncArgs<MemberFunc>::Arg1Type;
        using MessageType = typename std::remove_const_sharedptr_ref<Arg1Type>::type;
        static_assert(std::is_base_of<::google::protobuf::Message, MessageType>::value,
                      "the second argv only support protobuf");
        EventRegisterImpl<MessageType, MemberFuncArgs<MemberFunc>::ArgCount>::registEvent(this, cmd, mfn, owner);
    }

    template <typename MemberFunc>
    void registerEventHandle(int64_t adId, MemberFunc mfn, typename MemberFuncArgs<MemberFunc>::OwnerType *owner)
    {
        using Arg1Type = typename MemberFuncArgs<MemberFunc>::Arg1Type;
        using MessageType = typename std::remove_const_sharedptr_ref<Arg1Type>::type;
        static_assert(std::is_base_of<::google::protobuf::Message, MessageType>::value,
                      "the second argv only support protobuf");
        EventRegisterImpl<MessageType, MemberFuncArgs<MemberFunc>::ArgCount>::registEvent(this, adId, mfn, owner);
    }

    // MessagePtr(int64_t, const std::shared_ptr<T>&)
    template <typename T>
    void registerEvent2Handle(const typename MsgInt64Callback<T>::ProtobufMessageTInt64Callback &cb, void *p = nullptr)
    {
        t_proto_set_.insert(T::descriptor()->full_name());
        std::shared_ptr<MsgInt64Callback<T>> pd(new MsgInt64Callback<T>(cb, p));
        callbacks_[T::descriptor()].push_back(pd);
        if (p)
            moduleCbs_[p][T::descriptor()].push_back(pd);
    }

    virtual void triggerEvent(int64_t uid, const MessagePtr &event, void *p = nullptr)
    {
        if (uid <= 0 || !event)
            return;
        try
        {
            do
            {
                if (!p)
                {
                    auto it = callbacks_.find(event->GetDescriptor());
                    if (it == callbacks_.end())
                        break;
                    for (auto &cb : it->second)
                    {
                        if (cb->type() == kProtobufMessageTInt64Callback)
                        {
                            cb->onMessage(uid, event);
                        }
                    }
                }
                else
                {
                    auto &cbs = moduleCbs_[p][event->GetDescriptor()];
                    for (auto &cb : cbs)
                    {
                        if (cb->type() == kProtobufMessageTInt64Callback)
                        {
                            cb->onMessage(uid, event);
                        }
                    }
                }
            }
            while (0);
        }
        catch (const std::exception &ex)
        {
            ELOG << "exception: " << ex.what() << ", event: " << event->GetTypeName();
            throw ex;
        }
    }

    virtual void unregister(void *p)
    {
        for (auto &pair : callbacks_)
        {
            for (auto it = pair.second.begin(); it != pair.second.end();)
            {
                if ((*it)->owner() == p)
                {
                    t_proto_set_.erase(pair.first->full_name());
                    it = pair.second.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        moduleCbs_.erase(p);
    }

private:
    Callbacks callbacks_;
    ModuleCallbacks moduleCbs_;
    std::unordered_set<std::string> t_proto_set_;
};

template <typename T> struct EventRegisterImpl<T, 2>
{
    template <typename MemberFunc>
    static void registEvent(EventPump *ep, MemberFunc mfn, typename MemberFuncArgs<MemberFunc>::OwnerType *owner)
    {
        ep->registerEvent2Handle<T>(std::bind(mfn, owner, std::placeholders::_1, std::placeholders::_2), owner);
    }
};
