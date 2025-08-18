#include "shm_hashmap.h"
#include <cassert>
#include <cstring>

namespace shmrcu_map {

static inline bool is_pow2(uint32_t x){ return (x & (x-1))==0; }

uint64_t ShmHashMap::HashBytes(const void* data, uint32_t len){
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i=0;i<len;++i){ h^=p[i]; h*=1099511628211ull; }
    h ^= h>>33; h*=0xff51afd7ed558ccdULL; h^=h>>33; h*=0xc4ceb9fe1a85ec53ULL; h^=h>>33;
    return h;
}

ShmHashMap::ShmHashMap(void* base, uint32_t total_bytes, bool create,
                             uint32_t bucket_new, uint32_t lock_count)
    : base_(base), total_(total_bytes),
      slab_(base, total_bytes, /*region_off*/sizeof(HMHeader)+sizeof(Spin)*lock_count
                         + sizeof(uint32_t)*bucket_new /*buckets_new*/
                         + sizeof(uint32_t)*0 /*buckets_old initially 0*/,
            /*region_size*/ total_bytes - (sizeof(HMHeader)+sizeof(Spin)*lock_count + sizeof(uint32_t)*bucket_new),
            create),
      epoch_(base, /*offset later set*/ 0, /*create=*/create) // 先占位，稍后修正
{
    if (!base || total_bytes < 1<<20) throw std::runtime_error("ShmHashMap: shm too small");

    if (create) {
        if (!is_pow2(bucket_new)) { uint32_t b=8; while (b<bucket_new) b<<=1; bucket_new=b; }
        if (!is_pow2(lock_count)) { uint32_t l=4; while (l<lock_count) l<<=1; lock_count=l; }

        // 计算布局
        uint32_t off = 0;
        hdr_ = new (base_) HMHeader{};
        hdr_->magic = 0x484D5232; // 'HMR2'
        hdr_->version = 2;
        hdr_->total_size = total_bytes;
        hdr_->bucket_count_old = 0;
        hdr_->bucket_count_new = bucket_new;
        hdr_->lock_count = lock_count;
        hdr_->item_count = 0;
        hdr_->rehash_cursor.store(0, std::memory_order_relaxed);
        hdr_->rehash_active.store(0, std::memory_order_relaxed);
        hdr_->gets_ok.store(0, std::memory_order_relaxed);
        hdr_->gets_not_found.store(0, std::memory_order_relaxed);
        hdr_->puts_ok.store(0, std::memory_order_relaxed);
        hdr_->puts_overwrite.store(0, std::memory_order_relaxed);
        hdr_->puts_fail.store(0, std::memory_order_relaxed);
        hdr_->erases_ok.store(0, std::memory_order_relaxed);
        hdr_->erases_not_found.store(0, std::memory_order_relaxed);
        hdr_->retire_head.store(0, std::memory_order_relaxed);

        off = sizeof(HMHeader);
        hdr_->locks_off = off;
        auto* lks = reinterpret_cast<Spin*>(reinterpret_cast<uint8_t*>(base_)+off);
        for (uint32_t i=0;i<lock_count;++i){ new (&lks[i]) Spin(); lks[i].init(); }
        off += sizeof(Spin)*lock_count;

        hdr_->buckets_old_off = 0; // 目前无旧表
        hdr_->buckets_new_off = off;
        auto* bnew = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(base_)+off);
        std::memset(bnew, 0, sizeof(uint32_t)*bucket_new);
        off += sizeof(uint32_t)*bucket_new;

        // 预留 epoch header 紧跟在 slab header 前面更清晰；此处简单做：放在 bump 起点
        // 这里直接把 epoch 放在 locks+buckets_new 后，slab 的 region_off 在构造时已按此假设设置
        hdr_->epoch_off = off;
        new (reinterpret_cast<uint8_t*>(base_)+hdr_->epoch_off) shmrcu::EpochHeader{};
        // 重新构造 epoch（真正初始化）
        new (&epoch_) shmrcu::ShmEpoch(base_, hdr_->epoch_off, true);

        hdr_->slab_off = slab_.Header()->base_offset; // 对齐：slab 构造已在该处初始化
        hdr_->header_size = off; // 粗略保留

    } else {
        hdr_ = reinterpret_cast<HMHeader*>(base_);
        if (hdr_->magic != 0x484D5232 || hdr_->version != 2) {
            throw std::runtime_error("ShmHashMap: incompatible header");
        }
        // 重新附着 epoch/slab 的头
        new (&epoch_) shmrcu::ShmEpoch(base_, hdr_->epoch_off, false);
        // slab_ 已经在构造函数初始化为相同偏移；此处无需重做
    }
}

uint32_t ShmHashMap::AllocNode(uint64_t h, const void* k, uint32_t kl, const void* v, uint32_t vl){
    uint32_t need = NodeSizeBytes(kl, vl);
    uint32_t off = slab_.Alloc(need);
    if (!off) return 0;
    auto* n = reinterpret_cast<Node*>(off2ptr(off));
    n->next.store(0, std::memory_order_relaxed);
    n->hash = h;
    n->key_len = kl;
    n->val_len = vl;
    uint8_t* p = reinterpret_cast<uint8_t*>(n) + sizeof(Node);
    if (kl) std::memcpy(p, k, kl);
    if (vl) std::memcpy(p+kl, v, vl);
    return off;
}

void ShmHashMap::RetireNode(uint32_t node_off){
    if (!node_off) return;
    // 记录退休 epoch
    uint32_t bytes = NodeSizeBytes(reinterpret_cast<Node*>(off2ptr(node_off))->key_len,
                                   reinterpret_cast<Node*>(off2ptr(node_off))->val_len);
    uint32_t e = epoch_.BumpEpoch();

    // 在共享内存中构造 RetireNode
    // 为简化：用 slab 分配一块 RetireNode 记录（小对象），也可复用单独 freelist
    uint32_t rec_off = slab_.Alloc(Align8(sizeof(struct RetireNode)));
    if (!rec_off) return; // 极端下泄漏由进程退出释放
    auto* rec = reinterpret_cast<struct RetireNode*>(off2ptr(rec_off));
    rec->node_off = node_off;
    rec->bytes = bytes;
    rec->retire_epoch = e;
    // push 到 retire_head（lock-free）
    uint32_t head;
    do { head = hdr_->retire_head.load(std::memory_order_acquire); rec->next = head; }
    while (!hdr_->retire_head.compare_exchange_weak(head, rec_off, std::memory_order_acq_rel));
}

void ShmHashMap::Reclaim(){
    // 检查退休链中是否有可回收的节点
    uint32_t minE = epoch_.MinActiveEpoch();
    // 用一个临时栈把符合条件的摘下来，剩余的再挂回
    uint32_t prev = 0, cur = hdr_->retire_head.exchange(0, std::memory_order_acq_rel);
    uint32_t keep = 0, keep_tail = 0;
    while (cur){
        auto* r = reinterpret_cast<struct RetireNode*>(off2ptr(cur));
        uint32_t next = r->next;
        if (r->retire_epoch + 1 < minE){
            // 安全回收
            slab_.Free(r->node_off, r->bytes);
            slab_.Free(cur, Align8(sizeof(struct RetireNode)));
        } else {
            // 还不能回收，挂到 keep 链
            r->next = 0;
            if (!keep) { keep = cur; keep_tail = cur; }
            else {
                reinterpret_cast<struct RetireNode*>(off2ptr(keep_tail))->next = cur;
                keep_tail = cur;
            }
        }
        cur = next;
    }
    if (keep){
        // 再与 head 合并
        uint32_t head;
        do { head = hdr_->retire_head.load(std::memory_order_acquire);
             reinterpret_cast<struct RetireNode*>(off2ptr(keep_tail))->next = head;
        } while(!hdr_->retire_head.compare_exchange_weak(head, keep, std::memory_order_acq_rel));
    }
}

bool ShmHashMap::MoveToFront(uint32_t* head, uint64_t h, const void* key, uint32_t klen){
    // 在写锁内调用
    uint32_t cur = *head, prev = 0;
    while (cur){
        auto* n = reinterpret_cast<Node*>(off2ptr(cur));
        uint32_t nxt = n->next.load(std::memory_order_relaxed);
        if (n->hash==h && n->key_len==klen &&
            std::memcmp(reinterpret_cast<uint8_t*>(n)+sizeof(Node), key, klen)==0) {
            if (prev==0) return true; // 已在头部
            // 从链上摘下并插到头
            reinterpret_cast<Node*>(off2ptr(prev))->next.store(nxt, std::memory_order_relaxed);
            n->next.store(*head, std::memory_order_relaxed);
            *head = cur;
            return true;
        }
        prev = cur; cur = nxt;
    }
    return false;
}

bool ShmHashMap::EraseFromList(uint32_t* head, uint64_t h, const void* key, uint32_t klen, uint32_t& erased_off){
    uint32_t cur = *head, prev = 0;
    while (cur){
        auto* n = reinterpret_cast<Node*>(off2ptr(cur));
        uint32_t nxt = n->next.load(std::memory_order_relaxed);
        if (n->hash==h && n->key_len==klen &&
            std::memcmp(reinterpret_cast<uint8_t*>(n)+sizeof(Node), key, klen)==0) {
            if (prev==0) *head = nxt;
            else reinterpret_cast<Node*>(off2ptr(prev))->next.store(nxt, std::memory_order_relaxed);
            erased_off = cur;
            return true;
        }
        prev = cur; cur = nxt;
    }
    return false;
}

bool ShmHashMap::Put(const void* key, uint32_t klen, const void* val, uint32_t vlen){
    if (!key || klen==0) return false;
    uint64_t h = HashBytes(key,klen);
    bool use_new = true;
    uint32_t b = bucket_index(h, /*use_new*/true);
    uint32_t l = lock_index(h);

    auto& sp = locks()[l]; sp.lock();
    // 查找是否已存在于新表
    uint32_t* head = &buckets_new()[b];
    uint32_t cur = *head, prev = 0;
    while (cur){
        auto* n = reinterpret_cast<Node*>(off2ptr(cur));
        uint32_t nxt = n->next.load(std::memory_order_relaxed);
        if (n->hash==h && n->key_len==klen &&
            std::memcmp(reinterpret_cast<uint8_t*>(n)+sizeof(Node), key, klen)==0) {
            // 覆盖：同长原地写，否则新节点替换 + 退休旧
            uint8_t* p = reinterpret_cast<uint8_t*>(n)+sizeof(Node)+klen;
            if (n->val_len == vlen){
                if (vlen) std::memcpy(p, val, vlen);
                // 移头
                if (prev!=0){
                    reinterpret_cast<Node*>(off2ptr(prev))->next.store(nxt,std::memory_order_relaxed);
                    n->next.store(*head,std::memory_order_relaxed);
                    *head = cur;
                }
                hdr_->puts_overwrite.fetch_add(1,std::memory_order_relaxed);
                sp.unlock(); HelpRehashStep(); Reclaim(); return true;
            } else {
                uint32_t new_off = AllocNode(h, key, klen, val, vlen);
                if (!new_off){ hdr_->puts_fail.fetch_add(1,std::memory_order_relaxed); sp.unlock(); return false; }
                auto* nn = reinterpret_cast<Node*>(off2ptr(new_off));
                nn->next.store(nxt,std::memory_order_relaxed);
                if (prev==0) *head = new_off;
                else reinterpret_cast<Node*>(off2ptr(prev))->next.store(new_off,std::memory_order_relaxed);
                // 退休旧节点
                RetireNode(cur);
                hdr_->puts_overwrite.fetch_add(1,std::memory_order_relaxed);
                sp.unlock(); HelpRehashStep(); Reclaim(); return true;
            }
        }
        prev = cur; cur = nxt;
    }
    // 不存在：插入新表头
    uint32_t new_off = AllocNode(h, key, klen, val, vlen);
    if (!new_off){ hdr_->puts_fail.fetch_add(1,std::memory_order_relaxed); sp.unlock(); return false; }
    auto* nn = reinterpret_cast<Node*>(off2ptr(new_off));
    nn->next.store(*head, std::memory_order_relaxed);
    *head = new_off;
    hdr_->item_count++;
    hdr_->puts_ok.fetch_add(1,std::memory_order_relaxed);
    sp.unlock(); HelpRehashStep(); Reclaim(); return true;
}

bool ShmHashMap::Get(const void* key, uint32_t klen, std::string& out, int reader_idx){
    out.clear();
    if (!key || klen==0) return false;
    uint64_t h = HashBytes(key,klen);

    // 读者进入
    epoch_.ReaderEnter(reader_idx);

    // 先查新表（无锁）
    uint32_t b = bucket_index(h, /*use_new*/true);
    uint32_t cur = buckets_new()[b];
    while (cur){
        auto* n = reinterpret_cast<Node*>(off2ptr(cur));
        if (n->hash==h && n->key_len==klen &&
            std::memcmp(reinterpret_cast<uint8_t*>(n)+sizeof(Node), key, klen)==0) {
            const char* v = reinterpret_cast<const char*>(n)+sizeof(Node)+klen;
            out.assign(v, v + n->val_len);
            hdr_->gets_ok.fetch_add(1,std::memory_order_relaxed);
            epoch_.ReaderExit(reader_idx);
            return true;
        }
        cur = n->next.load(std::memory_order_relaxed);
    }

    // 若有旧表，再查旧表
    if (hdr_->bucket_count_old){
        uint32_t b2 = bucket_index(h, /*use_new*/false);
        uint32_t cur2 = buckets_old()[b2];
        while (cur2){
            auto* n = reinterpret_cast<Node*>(off2ptr(cur2));
            if (n->hash==h && n->key_len==klen &&
                std::memcmp(reinterpret_cast<uint8_t*>(n)+sizeof(Node), key, klen)==0) {
                const char* v = reinterpret_cast<const char*>(n)+sizeof(Node)+klen;
                out.assign(v, v + n->val_len);
                hdr_->gets_ok.fetch_add(1,std::memory_order_relaxed);
                epoch_.ReaderExit(reader_idx);
                return true;
            }
            cur2 = n->next.load(std::memory_order_relaxed);
        }
    }
    hdr_->gets_not_found.fetch_add(1,std::memory_order_relaxed);
    epoch_.ReaderExit(reader_idx);
    return false;
}

bool ShmHashMap::Exists(const void* key, uint32_t klen, int reader_idx){
    std::string tmp;
    return Get(key,klen,tmp,reader_idx);
}

bool ShmHashMap::Erase(const void* key, uint32_t klen){
    if (!key || klen==0) return false;
    uint64_t h = HashBytes(key,klen);

    // 先尝试新表
    {
        uint32_t b = bucket_index(h,true);
        uint32_t l = lock_index(h);
        auto& sp = locks()[l]; sp.lock();
        uint32_t* head = &buckets_new()[b];
        uint32_t erased=0;
        if (EraseFromList(head, h, key, klen, erased)){
            RetireNode(erased);
            hdr_->item_count--;
            hdr_->erases_ok.fetch_add(1,std::memory_order_relaxed);
            sp.unlock(); HelpRehashStep(); Reclaim(); return true;
        }
        sp.unlock();
    }
    // 再尝试旧表
    if (hdr_->bucket_count_old){
        uint32_t b = bucket_index(h,false);
        uint32_t l = lock_index(h);
        auto& sp = locks()[l]; sp.lock();
        uint32_t* head = &buckets_old()[b];
        uint32_t erased=0;
        if (EraseFromList(head, h, key, klen, erased)){
            RetireNode(erased);
            hdr_->item_count--;
            hdr_->erases_ok.fetch_add(1,std::memory_order_relaxed);
            sp.unlock(); HelpRehashStep(); Reclaim(); return true;
        }
        sp.unlock();
    }
    hdr_->erases_not_found.fetch_add(1,std::memory_order_relaxed);
    return false;
}

bool ShmHashMap::TouchMoveToFront(const void* key, uint32_t klen){
    if (!key || klen==0) return false;
    uint64_t h = HashBytes(key,klen);
    // 仅对新表尝试（热点迁移以新表为主）
    uint32_t b = bucket_index(h,true);
    uint32_t l = lock_index(h);
    auto& sp = locks()[l]; sp.lock();
    bool ok = MoveToFront(&buckets_new()[b], h, key, klen);
    sp.unlock();
    return ok;
}

// ========== 渐进 rehash ==========
void ShmHashMap::EnsureBuckets(uint32_t buckets, bool is_new){
    if (is_new){
        if (!is_pow2(buckets)){ uint32_t x=8; while(x<buckets) x<<=1; buckets=x; }
        // 已在构造时初始化，二次调用无需处理
        (void)buckets;
    } else {
        // old 表申请空间并清零
        hdr_->bucket_count_old = buckets;
        hdr_->buckets_old_off = hdr_->buckets_new_off + sizeof(uint32_t)*hdr_->bucket_count_new; // 紧随 new 后
        std::memset(off2ptr(hdr_->buckets_old_off), 0, sizeof(uint32_t)*hdr_->bucket_count_old);
    }
}

bool ShmHashMap::RehashStart(uint32_t new_bucket_count){
    if (!is_pow2(new_bucket_count)){ uint32_t x=8; while(x<new_bucket_count) x<<=1; new_bucket_count=x; }
    // 如果已经在 rehash，直接返回
    if (RehashInProgress()) return true;

    // 将当前 new 变为 old；分配新的 new
    // 简单做法：把 new 桶数组作为 old；在其后分配新的 new 桶数组
    uint32_t old_n = hdr_->bucket_count_new;
    uint32_t old_off = hdr_->buckets_new_off;

    // 新 new 桶数组紧跟在 old 数组后面
    uint32_t new_off = old_off + sizeof(uint32_t)*old_n;
    if (new_off + sizeof(uint32_t)*new_bucket_count + 1 > total_) return false;

    // 置旧表
    hdr_->bucket_count_old = old_n;
    hdr_->buckets_old_off  = old_off;

    // 设置新表
    hdr_->bucket_count_new = new_bucket_count;
    hdr_->buckets_new_off  = new_off;
    std::memset(off2ptr(hdr_->buckets_new_off), 0, sizeof(uint32_t)*hdr_->bucket_count_new);

    hdr_->rehash_cursor.store(0, std::memory_order_release);
    hdr_->rehash_active.store(1, std::memory_order_release);
    return true;
}

uint32_t ShmHashMap::MigrateOneBucket(uint32_t b){
    // 将旧表第 b 桶整体搬到新表
    if (b >= hdr_->bucket_count_old) return 0;
    uint32_t head = buckets_old()[b];
    if (!head) return 1; // 空桶，算作已处理

    // 需要加锁：为了与写路径对该旧桶的并发擦除/移动协调
    // 使用该桶第一个元素 hash 的锁；保守起见使用所有条带锁（固定顺序）避免死锁，简化实现
    for (uint32_t i=0;i<hdr_->lock_count;++i) locks()[i].lock();

    // 取出旧桶链头并清空旧桶
    head = buckets_old()[b];
    buckets_old()[b] = 0;

    // 将链上所有节点按新 hash 映射插入新表头（稳定无锁读）
    uint32_t cur = head;
    while (cur){
        auto* n = reinterpret_cast<Node*>(off2ptr(cur));
        uint32_t nxt = n->next.load(std::memory_order_relaxed);
        uint32_t bn = bucket_index(n->hash, /*use_new*/true);
        // 插到新桶头（在全局锁下）
        n->next.store(buckets_new()[bn], std::memory_order_relaxed);
        buckets_new()[bn] = cur;
        cur = nxt;
    }

    for (uint32_t i=0;i<hdr_->lock_count;++i) locks()[i].unlock();
    return 1;
}

uint32_t ShmHashMap::RehashStep(uint32_t steps){
    if (!RehashInProgress()) return 0;
    uint32_t done = 0;
    while (steps--){
        uint32_t b = hdr_->rehash_cursor.fetch_add(1, std::memory_order_acq_rel);
        if (b >= hdr_->bucket_count_old) { hdr_->rehash_active.store(0,std::memory_order_release); break; }
        done += MigrateOneBucket(b);
    }
    return done;
}
void ShmHashMap::HelpRehashStep(){
    if (RehashInProgress()) (void)RehashStep(1);
}

} // namespace shmrcu_map
