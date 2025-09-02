#pragma once

#include "../../public/proto_files//msg_mapping.h"
#include "../../public/proto_files//msg_mapping_ss.h"
#include "Callback.hpp"
#include "FuncResolver.hpp"
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

    template <typename MemberFunc>
    static void registEvent(EventPump *, uint32_t, MemberFunc, typename MemberFuncArgs<MemberFunc>::OwnerType *)
    {
    }
};

class EventPump
{
    typedef std::vector<std::shared_ptr<Callback>> CallbackVec;
    typedef std::unordered_map<uint32_t, CallbackVec> Callbacks;
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
    void registerEventHandle(uint32_t msg_id, MemberFunc mfn, typename MemberFuncArgs<MemberFunc>::OwnerType *owner)
    {
        using Arg1Type = typename MemberFuncArgs<MemberFunc>::Arg1Type;
        using MessageType = typename std::remove_const_sharedptr_ref<Arg1Type>::type;
        static_assert(std::is_base_of<::google::protobuf::Message, MessageType>::value,
                      "the second argv only support protobuf");
        EventRegisterImpl<MessageType, MemberFuncArgs<MemberFunc>::ArgCount>::registEvent(this, msg_id, mfn, owner);
    }

    // MessagePtr(int64_t, const std::shared_ptr<T>&)
    template <typename T>
    void registerEvent2Handle(const typename MsgInt64Callback<T>::ProtobufMessageTInt64Callback &cb, void *p = nullptr)
    {
        uint32_t msg_id = 0;
        const auto &full_name = T::descriptor()->full_name();

        if (kssMsgNameToId.count(full_name))
        {
            msg_id = kssMsgNameToId[full_name];
        }
        else if (kcsMsgNameToId.count(full_name))
        {
            msg_id = kcsMsgNameToId[full_name];
        }

        if (msg_id <= 0)
        {
            ELOG << full_name << "msg_id is error!" << "msg_id: " << msg_id;
            return;
        }

        std::shared_ptr<MsgInt64Callback<T>> pd(new MsgInt64Callback<T>(cb, p));
        callbacks_[msg_id].push_back(pd);
        if (p)
            moduleCbs_[p][msg_id].push_back(pd);
    }

    // MessagePtr(int64_t, const std::shared_ptr<T>&)
    template <typename T>
    void registerEvent3Handle(int32_t msg_id, const typename MsgInt64Callback<T>::ProtobufMessageTInt64Callback &cb,
                              void *p = nullptr)
    {
        std::shared_ptr<MsgInt64Callback<T>> pd(new MsgInt64Callback<T>(cb, p));
        callbacks_[msg_id].push_back(pd);
        if (p)
            moduleCbs_[p][msg_id].push_back(pd);
    }

    virtual void triggerEvent(int64_t uid, const MessagePtr &event, uint32_t msg_id = 0, void *p = nullptr)
    {
        if (uid <= 0 || !event)
            return;

        const auto &full_name = event->GetDescriptor()->full_name();
        if (msg_id <= 0)
        {
            uint32_t msg_id = 0;

            auto it_ss = kssMsgNameToId.find(full_name);
            if (it_ss != kssMsgNameToId.end())
            {
                msg_id = it_ss->second;
            }
            else
            {
                auto it_cs = kcsMsgNameToId.find(full_name);
                if (it_cs != kcsMsgNameToId.end())
                {
                    msg_id = it_cs->second;
                }
            }
        }

        if (msg_id <= 0)
        {
            ELOG << full_name << "msg_id is error!" << "msg_id: " << msg_id;
            return;
        }

        try
        {
            do
            {
                if (!p)
                {
                    auto it = callbacks_.find(msg_id);
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
                    auto &cbs = moduleCbs_[p][msg_id];
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
};

template <typename T> struct EventRegisterImpl<T, 2>
{
    template <typename MemberFunc>
    static void registEvent(EventPump *ep, MemberFunc mfn, typename MemberFuncArgs<MemberFunc>::OwnerType *owner)
    {
        ep->registerEvent2Handle<T>(std::bind(mfn, owner, std::placeholders::_1, std::placeholders::_2), owner);
    }
};

template <typename T> struct EventRegisterImpl<T, 3>
{
    template <typename MemberFunc>
    static void registEvent(EventPump *ep, const char *cmd, MemberFunc mfn,
                            typename MemberFuncArgs<MemberFunc>::OwnerType *owner)
    {
        ep->registerEventHandle<T>(cmd, std::bind(mfn, owner, std::placeholders::_1, std::placeholders::_2), owner);
    }

    template <typename MemberFunc>
    static void registEvent(EventPump *ep, uint32_t msg_id, MemberFunc mfn,
                            typename MemberFuncArgs<MemberFunc>::OwnerType *owner)
    {
        ep->registerEventHandle<T>(msg_id, std::bind(mfn, owner, std::placeholders::_1, std::placeholders::_2), owner);
    }
};
