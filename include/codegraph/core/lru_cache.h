/**
 * lru_cache.h — LRU 缓存实现
 *
 * 带 TTL 和主动失效机制的 LRU 缓存。
 * 用于缓存 MCP 查询结果，减少重复数据库查询。
 *
 * 特性：
 *   - LRU 淘汰策略（Least Recently Used）
 *   - TTL 过期（默认 30 秒）
 *   - 主动失效（索引更新时清空缓存）
 *   - Generation 机制（每次索引更新 generation++，旧数据自动失效）
 *
 * 使用示例：
 *   LruCache<string, vector<Node>> cache(100, 30);  // 100 条，30s TTL
 *   cache.put("search:foo", results);
 *   auto results = cache.get("search:foo");  // 命中缓存
 *   cache.invalidate();  // 索引更新，清空缓存
 */

#pragma once

#include <chrono>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace codegraph {

template<typename Key, typename Value>
class LruCache {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::seconds;

    /**
     * 构造函数。
     *
     * @param max_size 最大缓存条目数
     * @param ttl_seconds 缓存过期时间（秒）
     */
    explicit LruCache(size_t max_size = 100, int ttl_seconds = 30)
        : max_size_(max_size), ttl_(ttl_seconds) {}

    /**
     * 获取缓存值。
     *
     * @param key 缓存键
     * @return 命中返回值，未命中或过期返回 nullopt
     *
     * 命中条件：
     *   1. key 存在
     *   2. 未过期（now - created < ttl）
     *   3. generation 匹配（未被主动失效）
     *
     * 命中后会将该条目移到 LRU 头部。
     */
    std::optional<Value> get(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = map_.find(key);
        if (it == map_.end()) {
            return std::nullopt;
        }

        auto& entry = it->second;

        // 检查是否过期
        if (is_expired(entry)) {
            // 删除过期条目
            order_.erase(entry.it);
            map_.erase(it);
            return std::nullopt;
        }

        // 检查 generation（主动失效）
        if (entry.generation != generation_) {
            order_.erase(entry.it);
            map_.erase(it);
            return std::nullopt;
        }

        // 移到 LRU 头部（最近使用）
        order_.splice(order_.begin(), order_, entry.it);

        return entry.value;
    }

    /**
     * 存入缓存。
     *
     * @param key 缓存键
     * @param value 缓存值
     *
     * 如果缓存已满，会淘汰最久未使用的条目。
     */
    void put(const Key& key, Value value) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = map_.find(key);
        if (it != map_.end()) {
            // 更新已存在的条目
            it->second.value = std::move(value);
            it->second.created = Clock::now();
            it->second.generation = generation_;
            order_.splice(order_.begin(), order_, it->second.it);
            return;
        }

        // 缓存已满，淘汰最久未使用的
        if (map_.size() >= max_size_) {
            auto last = order_.back();
            map_.erase(last);
            order_.pop_back();
        }

        // 插入新条目
        order_.push_front(key);
        Entry entry;
        entry.value = std::move(value);
        entry.it = order_.begin();
        entry.created = Clock::now();
        entry.generation = generation_;
        map_[key] = std::move(entry);
    }

    /**
     * 主动失效：清空所有缓存。
     *
     * 当索引更新时调用，确保下次查询返回最新数据。
     * 使用 generation 机制，不需要遍历删除。
     */
    void invalidate() {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_++;
        // 不立即删除，等下次 get 时惰性清理
        // 但如果 generation 变化太大，还是清理一下
        if (generation_ % 100 == 0) {
            clear_unsafe();
        }
    }

    /**
     * 清空缓存。
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_unsafe();
    }

    /**
     * 获取当前缓存大小。
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_.size();
    }

    /**
     * 检查缓存是否包含某个键（不更新 LRU 顺序）。
     */
    bool contains(const Key& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        if (is_expired(it->second)) return false;
        if (it->second.generation != generation_) return false;
        return true;
    }

private:
    struct Entry {
        Value value;
        typename std::list<Key>::iterator it;  // 指向 order_ 中的位置
        Clock::time_point created;             // 创建时间
        uint64_t generation;                   // 创建时的 generation
    };

    bool is_expired(const Entry& entry) const {
        return Clock::now() - entry.created > ttl_;
    }

    void clear_unsafe() {
        map_.clear();
        order_.clear();
    }

    size_t max_size_;
    Duration ttl_;
    uint64_t generation_ = 0;

    mutable std::mutex mutex_;
    std::unordered_map<Key, Entry> map_;
    std::list<Key> order_;  // LRU 顺序，头部是最近使用
};

}  // namespace codegraph
