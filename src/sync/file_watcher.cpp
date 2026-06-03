/**
 * file_watcher.cpp — Linux inotify 文件监听实现
 *
 * 本文件封装了 Linux 的 inotify API，用于监听文件系统变更并触发增量索引。
 *
 * inotify 工作原理：
 *   1. inotify_init1() 创建一个 inotify 实例（返回文件描述符）
 *   2. inotify_add_watch() 注册要监听的目录和事件类型
 *   3. read() / select() 阻塞等待事件发生
 *   4. 事件结构体包含变更的文件名和事件类型
 *
 * 监听的事件类型：
 *   - IN_MODIFY: 文件被修改
 *   - IN_CREATE: 新文件/目录被创建
 *   - IN_DELETE: 文件/目录被删除
 *   - IN_MOVED_FROM/IN_MOVED_TO: 文件被移动
 *
 * 设计要点：
 *   - IN_NONBLOCK: 非阻塞模式，poll() 中用 select() 控制超时
 *   - 递归监听：初始时递归监听所有子目录
 *   - 新目录处理：IN_CREATE 事件触发时，自动添加对新目录的监听
 *   - EINTR 处理：信号中断不会导致崩溃
 */

#include "codegraph/sync/file_watcher.h"
#include <sys/inotify.h>
#include <unistd.h>
#include <filesystem>
#include <cstring>
#include <stdexcept>

namespace fs = std::filesystem;

namespace codegraph {

/**
 * 构造函数：初始化 inotify 并递归监听目录。
 *
 * 流程：
 *   1. inotify_init1(IN_NONBLOCK) 创建非阻塞的 inotify 实例
 *   2. add_watch_recursive() 递归监听目录树中的所有子目录
 */
FileWatcher::FileWatcher(const std::string& path) {
    inotify_fd_ = inotify_init1(IN_NONBLOCK);
    if (inotify_fd_ < 0) {
        throw std::runtime_error("Failed to initialize inotify");
    }
    running_ = true;
    add_watch_recursive(path);
}

/**
 * 析构函数：停止监听，移除所有 watch，关闭文件描述符。
 */
FileWatcher::~FileWatcher() {
    stop();
    for (auto& [wd, path] : watch_map_) {
        inotify_rm_watch(inotify_fd_, wd);
    }
    if (inotify_fd_ >= 0) close(inotify_fd_);
}

/** 设置事件回调函数。 */
void FileWatcher::set_callback(Callback cb) { callback_ = cb; }

/**
 * 对单个目录添加 inotify 监听。
 *
 * 监听的事件掩码：
 *   IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO
 *
 * wd（watch descriptor）是 inotify_add_watch() 返回的整数标识，
 * 用于在事件中识别是哪个目录发生了变更。
 * watch_map_ 维护 wd → 目录路径的映射。
 */
void FileWatcher::add_watch(const std::string& path) {
    uint32_t mask = IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
    int wd = inotify_add_watch(inotify_fd_, path.c_str(), mask);
    if (wd >= 0) watch_map_[wd] = path;
}

/**
 * 递归监听目录树中的所有子目录。
 *
 * 为什么需要递归：
 *   inotify 只监听单个目录，不会自动监听子目录。
 *   如果不递归，新建子目录中的文件变更不会被检测到。
 *
 * 异常处理：
 *   权限错误等非致命异常被静默忽略（catch (...)）。
 *   缺少某些子目录的监听不影响整体功能。
 */
void FileWatcher::add_watch_recursive(const std::string& path) {
    add_watch(path);
    try {
        for (auto& entry : fs::recursive_directory_iterator(path)) {
            if (entry.is_directory()) {
                add_watch(entry.path().string());
            }
        }
    } catch (...) {
        // Permission errors etc. are non-fatal
    }
}

/**
 * 轮询 inotify 事件。
 *
 * 流程：
 *   1. select() 等待 inotify_fd_ 可读，超时 timeout_ms 毫秒
 *   2. read() 读取事件缓冲区（可能包含多个事件）
 *   3. 遍历缓冲区，解析每个 inotify_event
 *   4. 调用回调函数，传入完整文件路径和事件掩码
 *
 * 事件缓冲区格式：
 *   [inotify_event | name padding] [inotify_event | name padding] ...
 *   每个事件的大小 = sizeof(inotify_event) + event->len
 *
 * EINTR 处理：
 *   select() 可能被信号中断（如 SIGINT），检查 errno == EINTR 并静默返回。
 */
void FileWatcher::poll(int timeout_ms) {
    char buf[4096];
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(inotify_fd_, &fds);

    int ret = select(inotify_fd_ + 1, &fds, nullptr, nullptr, &tv);
    if (ret < 0) {
        // EINTR 是信号中断（如 SIGINT），不是错误
        if (errno == EINTR) return;
        return;
    }
    if (ret == 0) return;  // 超时，无事件

    ssize_t len = read(inotify_fd_, buf, sizeof(buf));
    if (len <= 0) return;

    // 遍历事件缓冲区中的所有事件
    ssize_t i = 0;
    while (i < len) {
        struct inotify_event* event = (struct inotify_event*)&buf[i];
        if (callback_ && event->len > 0) {
            // 通过 wd 查找目录路径，拼接完整文件路径
            auto it = watch_map_.find(event->wd);
            std::string dir = (it != watch_map_.end()) ? it->second : "";
            std::string filepath = dir.empty() ? std::string(event->name)
                                               : dir + "/" + event->name;
            callback_(filepath, event->mask);
        }
        // 移动到下一个事件
        i += sizeof(struct inotify_event) + event->len;
    }
}

/** 停止监听（设置标志位）。 */
void FileWatcher::stop() { running_ = false; }

}  // namespace codegraph
