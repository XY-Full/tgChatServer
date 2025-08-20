#pragma once
#include <atomic>
#include <cstdint>
#include <thread>
#include <new>
#include <cstring>
#include <stdexcept>
namespace shmrcu {

// 支持最多 1024 个并发读者槽（跨进程/线程共享）
// 读者：进入时把自己槽位设为 current_epoch，退出时设为 0
// 写者：退休对象时记录退休 epoch，回收条件= 全体 reader 的最小非零 epoch > retired_epoch
static constexpr uint32_t MAX_READERS = 1024;

struct ReaderSlot {
    std::atomic<uint32_t> epoch; // 0=不在读临界区；>0=活动epoch
    std::atomic<uint32_t> used;  // 0=空闲；1=已占用
    std::atomic<uint32_t> tid;   // 调试/观测（可选）
};

struct EpochHeader {
    uint32_t magic;         // 'EPOC'
    uint32_t version;       // 1
    std::atomic<uint32_t> global_epoch; // 单调递增
    ReaderSlot readers[MAX_READERS];
};

class ShmEpoch {
public:
    ShmEpoch(void* base, uint32_t offset, bool create) {
        hdr_ = reinterpret_cast<EpochHeader*>(reinterpret_cast<uint8_t*>(base)+offset);
        if (create) {
            new (hdr_) EpochHeader{};
            hdr_->magic = 0x45504F43; // 'EPOC'
            hdr_->version = 1;
            hdr_->global_epoch.store(1, std::memory_order_relaxed);
            for (auto& r : hdr_->readers) {
                r.epoch.store(0, std::memory_order_relaxed);
                r.used.store(0, std::memory_order_relaxed);
                r.tid.store(0, std::memory_order_relaxed);
            }
        } else {
            if (hdr_->magic != 0x45504F43 || hdr_->version != 1)
                throw std::runtime_error("ShmEpoch: incompatible header");
        }
    }

    // 读者注册一个槽并持有（线程本地保存 idx）
    int RegisterReader(uint32_t tid_hint=0) {
        for (uint32_t i=0;i<MAX_READERS;++i){
            uint32_t exp=0;
            if (hdr_->readers[i].used.compare_exchange_strong(exp,1,std::memory_order_acq_rel)) {
                hdr_->readers[i].tid.store(tid_hint, std::memory_order_relaxed);
                return (int)i;
            }
        }
        return -1;
    }
    void UnregisterReader(int idx){
        if (idx<0) return;
        hdr_->readers[idx].epoch.store(0,std::memory_order_relaxed);
        hdr_->readers[idx].tid.store(0,std::memory_order_relaxed);
        hdr_->readers[idx].used.store(0,std::memory_order_release);
    }

    // 读侧进入/退出（无锁开销极低）
    inline uint32_t ReaderEnter(int idx){
        uint32_t e = hdr_->global_epoch.load(std::memory_order_acquire);
        hdr_->readers[idx].epoch.store(e, std::memory_order_release);
        return e;
    }
    inline void ReaderExit(int idx){
        hdr_->readers[idx].epoch.store(0, std::memory_order_release);
    }

    // 写者：推进 epoch
    inline uint32_t BumpEpoch(){
        return hdr_->global_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    // 写者：获取活动读者的最小 epoch（非零）
    uint32_t MinActiveEpoch() const {
        uint32_t minv = UINT32_MAX;
        for (uint32_t i=0;i<MAX_READERS;++i){
            uint32_t e = hdr_->readers[i].epoch.load(std::memory_order_acquire);
            if (e != 0 && e < minv) minv = e;
        }
        if (minv == UINT32_MAX) return hdr_->global_epoch.load(std::memory_order_acquire);
        return minv;
    }

private:
    EpochHeader* hdr_;
};

} // namespace shmrcu
