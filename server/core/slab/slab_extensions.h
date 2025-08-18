#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

// 代际计数 + 延迟回收
class GenerationCounter
{
public:
    GenerationCounter() : gen_(0) {}
    uint64_t enter() const { return gen_.load(std::memory_order_acquire); }
    void     increment() { gen_.fetch_add(1, std::memory_order_release); }
private:
    mutable std::atomic<uint64_t> gen_;
};

// 延迟回收列表
template<typename T>
class DeferredFreeList
{
public:
    void add(T* ptr, uint64_t gen) {
        list_.push_back({ptr, gen});
    }

    template<typename FreeFunc>
    void collect(uint64_t safe_gen, FreeFunc free_func) {
        auto it = list_.begin();
        while (it != list_.end()) {
            if (it->gen < safe_gen) {
                free_func(it->ptr);
                it = list_.erase(it);
            } else {
                ++it;
            }
        }
    }
private:
    struct Node {
        T* ptr;
        uint64_t gen;
    };
    std::vector<Node> list_;
};
