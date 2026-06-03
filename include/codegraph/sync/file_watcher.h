/**
 * file_watcher.h — Linux inotify 文件监听接口
 *
 * 封装 Linux 的 inotify API，用于监听文件系统变更并触发增量索引。
 *
 * 使用模式：
 *   FileWatcher watcher("/path/to/project");
 *   watcher.set_callback([](const std::string& path, uint32_t mask) {
 *       // 处理文件变更
 *   });
 *   while (running) {
 *       watcher.poll(1000);  // 阻塞等待事件，超时 1 秒
 *   }
 *
 * 监听的事件：修改、创建、删除、移动
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace codegraph {

class FileWatcher {
public:
    /**
     * 构造函数：初始化 inotify 并递归监听目录。
     *
     * @param path 要监听的根目录
     * @throws std::runtime_error 如果 inotify 初始化失败
     */
    FileWatcher(const std::string& path);
    ~FileWatcher();

    /** 事件回调类型：(文件路径, 事件掩码)。 */
    using Callback = std::function<void(const std::string& path, uint32_t mask)>;

    /** 设置事件回调。 */
    void set_callback(Callback cb);

    /**
     * 对单个目录添加 inotify 监听。
     * 监听事件：IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO
     */
    void add_watch(const std::string& path);

    /** 递归监听目录树中的所有子目录。 */
    void add_watch_recursive(const std::string& path);

    /**
     * 轮询 inotify 事件。
     *
     * 阻塞等待直到有事件发生或超时。
     * select() + read() 的组合，处理 EINTR 信号中断。
     *
     * @param timeout_ms 超时时间（毫秒）
     */
    void poll(int timeout_ms = 1000);

    /** 停止监听。 */
    void stop();

private:
    int inotify_fd_ = -1;                               // inotify 文件描述符
    std::unordered_map<int, std::string> watch_map_;    // wd → 目录路径映射
    Callback callback_;
    bool running_ = false;
};

}  // namespace codegraph
