#pragma once

#include <string>
#include <vector>

namespace codegraph {

struct DiffHunk {
    std::string file_path;   // 相对路径
    int line_start;          // 变更起始行
    int line_end;            // 变更结束行
    bool is_added;           // true=新增, false=删除/修改
};

// 解析 git diff --unified=0 的输出
std::vector<DiffHunk> parse_diff(const std::string& diff_output);

// 执行 git diff 命令并返回输出
std::string run_git_diff(const std::string& ref = "");

}  // namespace codegraph
