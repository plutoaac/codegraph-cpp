#include "codegraph/diff/diff_parser.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <regex>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace codegraph {

std::vector<DiffHunk> parse_diff(const std::string& diff_output) {
    std::vector<DiffHunk> hunks;
    std::istringstream stream(diff_output);
    std::string line;

    std::string current_file;
    // 匹配 +++ b/path/to/file
    static const std::regex file_regex(R"(^\+\+\+ b/(.+)$)");
    // 匹配 @@ -a,b +c,d @@ 或 @@ -a +c @@
    static const std::regex hunk_regex(R"(^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@)");

    while (std::getline(stream, line)) {
        std::smatch match;

        // 检测文件名
        if (std::regex_match(line, match, file_regex)) {
            current_file = "./" + match[1].str();
            continue;
        }

        // 检测 hunk 头
        if (std::regex_match(line, match, hunk_regex)) {
            if (current_file.empty()) continue;

            int new_start = std::stoi(match[3].str());
            int new_count = match[4].matched ? std::stoi(match[4].str()) : 1;

            // 如果 new_count 为 0，说明是纯删除，跳过
            if (new_count == 0) continue;

            DiffHunk hunk;
            hunk.file_path = current_file;
            hunk.line_start = new_start;
            hunk.line_end = new_start + new_count - 1;
            hunk.is_added = true;
            hunks.push_back(hunk);
        }
    }

    return hunks;
}

std::string run_git_diff(const std::string& ref) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return "";
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return "";
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[1]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        std::string unified_arg = "--unified=0";
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("git"));
        argv.push_back(const_cast<char*>("diff"));
        argv.push_back(unified_arg.data());
        if (!ref.empty()) {
            argv.push_back(const_cast<char*>(ref.c_str()));
        }
        argv.push_back(const_cast<char*>("--"));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipefd[1]);

    std::string result;
    std::array<char, 4096> buffer;
    while (true) {
        ssize_t n = read(pipefd[0], buffer.data(), buffer.size());
        if (n > 0) {
            result.append(buffer.data(), static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        result.clear();
        break;
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return "";
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return "";
    return result;
}

}  // namespace codegraph
