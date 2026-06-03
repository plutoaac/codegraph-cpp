/**
 * diff_parser.cpp — Git diff 解析和安全执行
 *
 * 本文件提供两个功能：
 *   1. run_git_diff(): 安全地执行 git diff 命令（防注入）
 *   2. parse_diff(): 解析 git diff 输出，提取变更的文件和行范围
 *
 * 安全设计：
 *   - 不使用 popen()（会被 shell 注入攻击）
 *   - 使用 fork() + execvp() 直接执行 git，不经过 shell
 *   - 参数用 argv 数组传递，不受特殊字符影响
 *   - -- 分隔符确保 ref 参数不会被解释为文件路径
 *   - EINTR 处理确保信号中断不会导致数据丢失
 */

#include "codegraph/diff/diff_parser.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <regex>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace codegraph {

/**
 * 解析 git diff --unified=0 的输出，提取变更的文件和行范围。
 *
 * git diff 输出格式：
 *   diff --git a/file.cpp b/file.cpp
 *   --- a/file.cpp
 *   +++ b/file.cpp              ← 文件名
 *   @@ -10,3 +10,5 @@          ← hunk 头：旧行号,旧行数 新行号,新行数
 *   @@ -20 +22 @@              ← 简化形式：行数为 1 时省略
 *
 * 解析策略：
 *   - 用正则匹配 +++ b/path 提取文件名
 *   - 用正则匹配 @@ -a,b +c,d @@ 提取 hunk 信息
 *   - 只关心新文件的行号（+c,d），用于定位变更影响的代码区域
 *   - new_count == 0 表示纯删除，跳过（没有新代码需要分析）
 *
 * 返回值：DiffHunk 列表，每个 hunk 包含 file_path、line_start、line_end
 */
std::vector<DiffHunk> parse_diff(const std::string& diff_output) {
    std::vector<DiffHunk> hunks;
    std::istringstream stream(diff_output);
    std::string line;

    std::string current_file;
    // 匹配 +++ b/path/to/file（新文件路径）
    static const std::regex file_regex(R"(^\+\+\+ b/(.+)$)");
    // 匹配 @@ -a,b +c,d @@ 或 @@ -a +c @@
    // capture group: 1=旧行号, 2=旧行数(可选), 3=新行号, 4=新行数(可选)
    static const std::regex hunk_regex(R"(^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@)");

    while (std::getline(stream, line)) {
        std::smatch match;

        // 检测文件名：+++ b/path → 当前文件
        if (std::regex_match(line, match, file_regex)) {
            current_file = "./" + match[1].str();
            continue;
        }

        // 检测 hunk 头
        if (std::regex_match(line, match, hunk_regex)) {
            if (current_file.empty()) continue;

            int new_start = std::stoi(match[3].str());
            int new_count = match[4].matched ? std::stoi(match[4].str()) : 1;

            // 纯删除（new_count == 0）：没有新代码，跳过
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

/**
 * 安全地执行 git diff 命令。
 *
 * 安全措施（替代 popen）：
 *   1. fork() + execvp()：直接执行 git，不经过 shell
 *   2. 参数用 argv 数组传递：特殊字符（如 `; rm -rf /`）会被当作普通字符串
 *   3. -- 分隔符：确保 ref 参数不会被解释为文件路径
 *   4. stderr 重定向到 /dev/null：避免错误信息泄露到 stdout
 *
 * 流程：
 *   父进程                      子进程
 *   ──────                      ──────
 *   创建 pipe()
 *   fork()
 *                                  关闭读端 pipe[0]
 *                                  dup2(pipe[1], STDOUT)
 *                                  execvp("git", ["git", "diff", ...])
 *   关闭写端 pipe[1]
 *   循环 read(pipe[0]) 读取输出
 *   waitpid() 等待子进程结束
 *
 * EINTR 处理：
 *   read() 和 waitpid() 都可能被信号中断（如 SIGINT），
 *   需要检查 errno == EINTR 并重试。
 *
 * @param ref Git ref（如 "HEAD~1"、"abc1234"），空字符串表示未提交的变更
 * @return git diff 的原始输出，失败返回空字符串
 */
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
        // ── 子进程 ──
        close(pipefd[0]);  // 关闭读端

        // 将 stdout 重定向到 pipe 写端
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[1]);

        // 将 stderr 重定向到 /dev/null（避免错误信息混入输出）
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        // 构建 argv 数组（不经过 shell，直接 execvp）
        std::string unified_arg = "--unified=0";
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("git"));
        argv.push_back(const_cast<char*>("diff"));
        argv.push_back(unified_arg.data());
        if (!ref.empty()) {
            argv.push_back(const_cast<char*>(ref.c_str()));
        }
        argv.push_back(const_cast<char*>("--"));  // 分隔符：确保 ref 不被解释为文件
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);  // execvp 失败
    }

    // ── 父进程 ──
    close(pipefd[1]);  // 关闭写端

    // 循环读取子进程的输出
    std::string result;
    std::array<char, 4096> buffer;
    while (true) {
        ssize_t n = read(pipefd[0], buffer.data(), buffer.size());
        if (n > 0) {
            result.append(buffer.data(), static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            break;  // EOF：子进程关闭了写端
        }
        if (errno == EINTR) {
            continue;  // 信号中断，重试
        }
        result.clear();
        break;
    }
    close(pipefd[0]);

    // 等待子进程结束
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;  // 信号中断，重试
        }
        return "";
    }
    // 子进程非正常退出或退出码非 0，返回空
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return "";
    return result;
}

}  // namespace codegraph
