#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace codegraph {

class FileWatcher {
public:
    FileWatcher(const std::string& path);
    ~FileWatcher();

    using Callback = std::function<void(const std::string& path, uint32_t mask)>;
    void set_callback(Callback cb);
    void add_watch(const std::string& path);
    void add_watch_recursive(const std::string& path);
    void poll(int timeout_ms = 1000);
    void stop();

private:
    int inotify_fd_ = -1;
    std::unordered_map<int, std::string> watch_map_;  // wd -> directory path
    Callback callback_;
    bool running_ = false;
};

}  // namespace codegraph
