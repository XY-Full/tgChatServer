
#pragma once

#include <functional>
#include <memory>
#include <unordered_map>

#include "Variant.hpp"

template <typename Tuple, std::size_t N> struct TupleConvertion
{
    static void toVariant(Tuple &t, std::vector<Variant> &vars)
    {
        TupleConvertion<Tuple, N - 1>::toVariant(t, vars);
        using type = typename std::tuple_element<N - 1, Tuple>::type;
        vars.push_back((type)std::get<N - 1>(t));
    }

    static void fromVariant(Tuple &t, std::vector<Variant> &from)
    {
        TupleConvertion<Tuple, N - 1>::fromVariant(t, from);
        using type = typename std::tuple_element<N - 1, Tuple>::type;
        std::get<N - 1>(t) = from[N - 1].GetVal<typename std::remove_reference<type>::type>();
    }
};

template <typename Tuple> struct TupleConvertion<Tuple, 0>
{
    static void toVariant(Tuple &t, std::vector<Variant> &vars)
    {
        (void)t;
        (void)vars;
    };

    static void fromVariant(Tuple &t, std::vector<Variant> &from)
    {
        (void)t;
        (void)from;
    }
};

template <typename T> struct MemberFuncArgs;

template <typename RT, typename Owner, typename... Args> struct MemberFuncArgs<RT (Owner::*)(Args...)>
{
    static constexpr std::size_t ArgCount = sizeof...(Args);

    using ReturnType = RT;
    using InArgsTuple = std::tuple<Args...>;
    using OwnerType = Owner;
    using Arg0Type = typename std::tuple_element<0, InArgsTuple>::type;
    using Arg1Type = typename std::tuple_element<1, InArgsTuple>::type;
};

template <typename MFA, std::size_t... Is>
std::function<typename MFA::ReturnType(typename MFA::OwnerType *,
                                       typename std::tuple_element<Is, typename MFA::InArgsTuple>::type...)>
    ExtraFunction(std::index_sequence<Is...>);

struct CallFunctor
{
    virtual Variant run() = 0;
    std::vector<Variant> vars_;
};

template <typename MemberFunc> struct CallFunctorImpl : CallFunctor
{
    using MFA = MemberFuncArgs<MemberFunc>;
    using FunT = decltype(ExtraFunction<MFA>(std::make_index_sequence<MFA::ArgCount>{}));
    using OwnerT = typename MFA::OwnerType;

    CallFunctorImpl(MemberFunc func, OwnerT *owner) : func_(func), owner_(owner)
    {
    }

    Variant run() override
    {
        typename MFA::InArgsTuple params;
        TupleConvertion<decltype(params), MFA::ArgCount>::fromVariant(params, vars_);
        return forwardTupleToFunction(params);
    }

    template <typename... TupleArgs, std::size_t... I>
    Variant forwardTupleToFunctionImpl(std::tuple<TupleArgs...> &tuple, std::index_sequence<I...>)
    {
        // 使用std::get获取tuple中的每个元素，然后forward到acceptAnything
        // acceptAnything(std::forward<TupleArgs>(std::get<I>(tuple))...);
        return func_(owner_, std::forward<TupleArgs>(std::get<I>(tuple))...);
    }

    template <typename... TupleArgs> Variant forwardTupleToFunction(std::tuple<TupleArgs...> &tuple)
    {
        constexpr auto size = sizeof...(TupleArgs);
        // 使用std::index_sequence生成一个整数序列[0, size]}
        return forwardTupleToFunctionImpl(tuple, std::make_index_sequence<size>{});
    }

private:
    FunT func_;
    OwnerT *owner_;
};

typedef std::shared_ptr<CallFunctor> CallFunctorPtr;

class Caller
{
    typedef std::unordered_map<std::string, CallFunctorPtr> CallFunctorPtrMap;

public:
    template <typename MemberFunc>
    void Regist(const char *name, MemberFunc f, typename MemberFuncArgs<MemberFunc>::OwnerType *owner)
    {
        auto functor = std::make_shared<CallFunctorImpl<MemberFunc>>(f, owner);
        callFunctors_.insert({name, functor});
    }

    template <typename... Args> Variant Call(const char *name, Args... args)
    {
        auto it = callFunctors_.find(name);
        if (it == callFunctors_.end())
        {
            throw std::runtime_error("Function not found: " + std::string(name));
        }
        auto tuple = std::make_tuple(args...);
        TupleConvertion<std::tuple<Args...>, sizeof...(Args)>::toVariant(tuple, it->second->vars_);
        return it->second->run();
    }

private:
    CallFunctorPtrMap callFunctors_;
};
