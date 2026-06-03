/**
 * diff_parser.h — Git diff 解析接口
 *
 * 提供两个功能：
 *   1. run_git_diff(): 安全地执行 git diff 命令（fork+execvp，防注入）
 *   2. parse_diff(): 解析 git diff 输出，提取变更的文件和行范围
 *
 * 用途：
 *   - CLI 的 change-impact 命令
 *   - MCP 的 codegraph_change_impact 工具
 */

#pragma once

#include <string>
#include <vector>

namespace codegraph {

/**
 * Diff hunk：一个变更区域。
 *
 * 对应 git diff 输出中的一个 @@ ... @@ 块。
 * 只关心新文件的行号（用于定位变更影响的代码区域）。
 */
struct DiffHunk {
    std::string file_path;   // 相对路径（如 "./src/main.cpp"）
    int line_start;          // 变更起始行（新文件的行号）
    int line_end;            // 变更结束行
    bool is_added;           // true=新增, false=删除/修改
};

/**
 * 解析 git diff --unified=0 的输出。
 *
 * 从 diff 输出中提取变更的文件路径和行范围。
 * 只关心新文件的行号（+++ b/path），忽略删除的行。
 *
 * @param diff_output git diff 的原始输出
 * @return DiffHunk 列表
 */
std::vector<DiffHunk> parse_diff(const std::string& diff_output);

/**
 * 安全地执行 git diff 命令。
 *
 * 使用 fork+execvp（不经过 shell），参数用 argv 数组传递，
 * 防止命令注入。使用 -- 分隔符确保 ref 不被解释为文件路径。
 *
 * @param ref Git ref（如 "HEAD~1"），空字符串表示未提交的变更
 * @return git diff 的原始输出，失败返回空字符串
 */
std::string run_git_diff(const std::string& ref = "");

}  // namespace codegraph
