#include "codegraph/sync/file_watcher.h"
#include <sys/inotify.h>
#include <unistd.h>
#include <filesystem>
#include <cstring>
#include <stdexcept>

namespace fs = std::filesystem;

namespace codegraph {

FileWatcher::FileWatcher(const std::string& path) {
    inotify_fd_ = inotify_init1(IN_NONBLOCK);
    if (inotify_fd_ < 0) {
        throw std::runtime_error("Failed to initialize inotify");
    }
    running_ = true;
    add_watch_recursive(path);
}

FileWatcher::~FileWatcher() {
    stop();
    for (auto& [wd, path] : watch_map_) {
        inotify_rm_watch(inotify_fd_, wd);
    }
    if (inotify_fd_ >= 0) close(inotify_fd_);
}

void FileWatcher::set_callback(Callback cb) { callback_ = cb; }

void FileWatcher::add_watch(const std::string& path) {
    uint32_t mask = IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
    int wd = inotify_add_watch(inotify_fd_, path.c_str(), mask);
    if (wd >= 0) watch_map_[wd] = path;
}

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
        // EINTR is expected when signals are delivered (e.g., SIGINT)
        if (errno == EINTR) return;
        return;
    }
    if (ret == 0) return;

    ssize_t len = read(inotify_fd_, buf, sizeof(buf));
    if (len <= 0) return;

    ssize_t i = 0;
    while (i < len) {
        struct inotify_event* event = (struct inotify_event*)&buf[i];
        if (callback_ && event->len > 0) {
            auto it = watch_map_.find(event->wd);
            std::string dir = (it != watch_map_.end()) ? it->second : "";
            std::string filepath = dir.empty() ? std::string(event->name)
                                               : dir + "/" + event->name;
            callback_(filepath, event->mask);
        }
        i += sizeof(struct inotify_event) + event->len;
    }
}

void FileWatcher::stop() { running_ = false; }

}  // namespace codegraph
