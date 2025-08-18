#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <string>
#include <new>
#include <cstring>
#include <thread>
#include "shm_slab.h"
#include "shm_epoch.h"

// ============ 无锁读 HashMap in Shared Memory ==============
// - slab 分配器提供节点内存
// - 读：RCU/epoch，无锁遍历
// - 写：条带锁保护链操作；删除放退休链，epoch 充分后回收
// - 渐进 rehash：双表（old/new），读查两表，写入新表，写路径/调用方协助搬迁
// ===========================================================

namespace shmrcu_map {

static inline uint32_t Align8(uint32_t x){ return (x + 7u) & ~7u; }

struct Spin {
    std::atomic<uint32_t> s;
    void init(){ s.store(0,std::memory_order_relaxed); }
    void lock(){
        for (uint32_t n=1;;){
            uint32_t e=0;
            if (s.compare_exchange_weak(e,1,std::memory_order_acquire)) return;
            for(uint32_t i=0;i<n;++i) std::this_thread::yield();
            if (n < (1u<<10)) n<<=1;
        }
    }
    void unlock(){ s.store(0,std::memory_order_release); }
};

struct HMHeader {
    uint32_t magic;           // 'HMR2'
    uint32_t version;         // 2
    uint32_t total_size;
    uint32_t header_size;

    uint32_t bucket_count_old; // 旧表桶数（0=无旧表）
    uint32_t bucket_count_new; // 新表桶数（活动写入）
    uint32_t lock_count;       // 条带锁数（power of two）
    uint32_t item_count;

    // rehash 进度
    std::atomic<uint32_t> rehash_cursor; // 0..bucket_count_old
    std::atomic<uint32_t> rehash_active; // 0/1

    // 结构偏移
    uint32_t locks_off;       // Spin[lock_count]
    uint32_t buckets_old_off; // uint32_t[bucket_count_old] 偏移链表头
    uint32_t buckets_new_off; // uint32_t[bucket_count_new]
    uint32_t slab_off;        // ShmSlab header offset
    uint32_t epoch_off;       // Epoch header offset

    // 统计
    std::atomic<uint64_t> gets_ok, gets_not_found;
    std::atomic<uint64_t> puts_ok, puts_overwrite, puts_fail;
    std::atomic<uint64_t> erases_ok, erases_not_found;

    // 退休链（延迟回收）
    std::atomic<uint32_t> retire_head; // offset of RetireNode
};

struct Node {
    std::atomic<uint32_t> next; // offset Node
    uint64_t hash;
    uint32_t key_len;
    uint32_t val_len;
    // followed by key bytes then val bytes
};

struct RetireNode {
    uint32_t next;       // offset RetireNode
    uint32_t node_off;   // offset Node
    uint32_t bytes;      // node 总大小（便于 slab 归还）
    uint32_t retire_epoch;
};

class ShmHashMap {
public:
    ShmHashMap(void* base, uint32_t total_bytes, bool create,
                  uint32_t bucket_new = 4096, uint32_t lock_count = 64);

    // 线程/进程级：注册/注销读者槽（建议 TLS 保存 idx）
    int  RegisterReader(uint32_t tid_hint=0) { return epoch_.RegisterReader(tid_hint); }
    void UnregisterReader(int idx) { epoch_.UnregisterReader(idx); }

    // API
    bool Put(const void* key, uint32_t klen, const void* val, uint32_t vlen);
    bool Get(const void* key, uint32_t klen, std::string& out, int reader_idx);
    bool Exists(const void* key, uint32_t klen, int reader_idx);
    bool Erase(const void* key, uint32_t klen);

    // 热点 key 移桶头（在写路径内部自动做；也可单独调用尝试）
    bool TouchMoveToFront(const void* key, uint32_t klen);

    // 渐进 rehash
    bool RehashStart(uint32_t new_bucket_count);
    uint32_t RehashStep(uint32_t steps); // 返回已搬迁桶数
    bool RehashInProgress() const { return hdr_->rehash_active.load(std::memory_order_acquire) != 0; }

    // 统计
    uint32_t Size() const { return hdr_->item_count; }

private:
    // 工具
    inline uint8_t* base8() const { return reinterpret_cast<uint8_t*>(base_); }
    inline void* off2ptr(uint32_t off) const { return off ? base8()+off : nullptr; }
    inline uint32_t ptr2off(const void* p) const {
        if (!p) return 0; return static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(p)-base8());
    }
    static uint64_t HashBytes(const void* data, uint32_t len);

    inline uint32_t* buckets_old() const { return reinterpret_cast<uint32_t*>(off2ptr(hdr_->buckets_old_off)); }
    inline uint32_t* buckets_new() const { return reinterpret_cast<uint32_t*>(off2ptr(hdr_->buckets_new_off)); }
    inline Spin* locks() const { return reinterpret_cast<Spin*>(off2ptr(hdr_->locks_off)); }

    inline uint32_t lock_index(uint64_t h) const { return (uint32_t)(h & (hdr_->lock_count-1)); }
    inline uint32_t bucket_index(uint64_t h, bool use_new) const {
        uint32_t n = use_new ? hdr_->bucket_count_new : hdr_->bucket_count_old;
        return (uint32_t)(h & (n-1));
    }

    // 节点内存
    uint32_t NodeSizeBytes(uint32_t k, uint32_t v) const {
        return Align8(sizeof(Node)+k+v);
    }
    uint32_t AllocNode(uint64_t h, const void* k, uint32_t kl, const void* v, uint32_t vl);
    void     RetireNode(uint32_t node_off); // 放到退休链
    void     Reclaim(); // 根据 epoch 回收退休节点

    // 链操作（在写锁内）
    bool     MoveToFront(uint32_t* head, uint64_t h, const void* key, uint32_t klen);
    bool     EraseFromList(uint32_t* head, uint64_t h, const void* key, uint32_t klen, uint32_t& erased_off);

    // rehash 相关
    void     HelpRehashStep(); // 写路径结束时协助一步
    uint32_t MigrateOneBucket(uint32_t b); // 把旧表的第 b 桶搬到新表
    void     EnsureBuckets(uint32_t buckets, bool is_new);

private:
    void* base_;
    uint32_t total_;
    HMHeader* hdr_;
    shmslab::ShmSlab slab_;
    shmrcu::ShmEpoch epoch_;
};

} // namespace shmrcu_map
