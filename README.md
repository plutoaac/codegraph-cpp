# CodeGraph C++

C++ 代码索引工具。用 tree-sitter 解析源码，存入 SQLite，提供符号搜索和调用图分析。支持 CLI 和 MCP 协议（可接入 Claude Code / OpenCode）。

## 项目亮点

- **轻量代码图**：tree-sitter 解析 C++/Python，SQLite + FTS5 持久化索引。
- **调用图分析**：支持 callers/callees/context/impact、循环依赖、路径查找、指标统计和 DOT 导出。
- **上下文感知建边**：解析同名函数时按同文件、同目录、同命名空间评分，减少 `WaitServerReady` 这类同名函数乱连。
- **增量索引**：只重建变更文件，并把调用变更文件的 caller 文件纳入重建，避免跨文件调用边丢失。
- **MCP 接入**：既可直接 `serve --mcp`，也可通过 `mini_rpc` 远程化为共享服务。
- **工程化防护**：SQLite 错误显式处理、FTS 查询转义、`git diff` 使用 `execvp` 避免 shell 注入。

## 构建

```bash
git clone https://github.com/plutoaac/codegraph-cpp.git
cd codegraph-cpp
npm install          # 安装 tree-sitter grammar
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## 快速开始

```bash
# 索引项目
cd /your/cpp/project
/path/to/codegraph init -i .

# 搜索符号
codegraph search RpcServer

# 查看上下文（调用者/被调用者）
codegraph context RpcServer

# 分析代码变更影响
codegraph change-impact HEAD~1

# 语义搜索（需要 pip install sentence-transformers）
codegraph embed
codegraph search-semantic "处理网络连接"

# 查看索引状态
codegraph status

# 图分析
codegraph cycles
codegraph path RpcServer::Start RpcServer::Stop
codegraph metrics
codegraph impact-chain Connection::Serve
```

## CLI 命令

| 命令 | 用途 |
|------|------|
| `init [-i <path>]` | 初始化 `.codegraph/` 索引目录 |
| `index [<path>]` | 索引源文件 |
| `resolve` | 解析未决的跨文件引用 |
| `search <query>` | 按名称搜索符号 |
| `search-semantic <query>` | 语义搜索（需要先运行 `embed`） |
| `embed` | 生成语义嵌入向量 |
| `context <symbol>` | 获取符号上下文（定义、调用者、被调用者） |
| `change-impact [ref]` | 分析代码变更影响（默认：未提交的更改） |
| `dead-code` | 查找未被引用的符号（死代码） |
| `cycles` | 检测循环依赖（Tarjan 强连通分量） |
| `path <from> <to>` | 查找两个符号间的调用链路径 |
| `metrics` | 显示调用图指标（入度/出度排名、深度、循环数） |
| `impact-chain <symbol>` | 影响链分析（附调用路径） |
| `export --dot` | 导出调用图为 Graphviz DOT 格式 |
| `status` | 显示索引统计 |
| `serve --mcp` | 启动 MCP 服务器（stdio JSON-RPC） |
| `watch [<path>]` | 监听文件变更，自动增量重索引 |

搜索命令会在 stderr 输出 token 节省统计：

```
$ codegraph search RpcServer
class RpcServer @ ./src/server/rpc_server.h:121
function RpcServer::Start @ ./src/server/rpc_server.cpp:188
...
[codegraph] 591 tokens (vs 8048 tokens to read 4 source files)
```

## 索引与建边策略

索引流程分两轮：

1. `index_extracted_files()` 批量插入文件节点、符号节点和 `contains` 边。
2. 调用引用先按当前数据库尝试解析，无法解析的写入 `unresolved_refs`，随后由 `resolve` 或 `index` 后的 resolve pass 统一补边。

同名函数匹配不是简单取第一个结果，而是使用上下文评分：

| 信号 | 分数 | 目的 |
|------|------|------|
| 同文件 | +10 | 优先连接当前文件内的 helper/local function |
| 同目录 | +5 | 优先连接同模块内函数 |
| 同命名空间 | +3 | 降低跨 namespace 误连 |

增量索引不会粗暴全量重建：它先找变更文件，再根据旧调用图把调用这些变更文件符号的 caller 文件加入本轮重建。这样既保留未改文件的节点 ID，也能避免重建目标文件后丢失跨文件入边。

## 类上下文聚合

对类/结构体使用 `context` 命令时，会自动聚合该类所有方法的调用关系：

```bash
codegraph context RpcServer
```

输出包含：
- `methods`: 类的所有方法列表
- `callers`: 调用该类方法的所有函数
- `callees`: 该类方法调用的所有函数
- `edges`: 调用关系边

自动搜索 `.h` 和对应的 `.cpp` 文件（支持 `.h`/`.hpp`/`.hxx`/`.hh`）。

## 代码变更影响分析

分析代码变更的影响范围：

```bash
# 分析未提交的更改
codegraph change-impact

# 分析最近一次提交
codegraph change-impact HEAD~1

# 分析两个提交之间的差异
codegraph change-impact abc123..def456
```

输出 JSON 包含：
- `changed_files`: 变更的文件列表
- `affected_symbols`: 直接受影响的符号（函数、类等）
- `impact`: 间接受影响的符号（调用者/引用者）

## Dead Code 检测

查找未被任何其他符号引用的函数：

```bash
codegraph dead-code
```

智能过滤：
- 排除头文件中的公共 API
- 排除 test/benchmark/demo 文件
- 排除 main() 函数
- 排除析构函数和协程内部方法

## 循环依赖检测（Tarjan SCC）

使用 Tarjan 强连通分量算法检测调用图中的循环依赖：

```bash
codegraph cycles
```

输出示例：
```json
[
  {
    "size": 3,
    "members": [
      {"name": "RpcServer::Start", "file": "./src/server/rpc_server.cpp", "line": 188},
      {"name": "RpcServer::StartWorkers", "file": "./src/server/rpc_server.cpp", "line": 134},
      {"name": "RpcServer::StartBusinessThreadPool", "file": "./src/server/rpc_server.cpp", "line": 119}
    ]
  }
]
```

- SCC 大小 > 1 表示存在循环依赖
- 算法复杂度：O(V + E)，对整个调用图做一次 DFS
- 用途：代码审查、架构分析、重构前的影响评估

## 调用链路径查找

查找两个符号之间的调用路径：

```bash
codegraph path RpcServer::Start RpcServer::Stop
```

输出示例：
```json
{
  "from": {"name": "RpcServer::Start", "file": "./src/server/rpc_server.cpp", "line": 188},
  "to": {"name": "RpcServer::Stop", "file": "./src/server/rpc_server.cpp", "line": 331},
  "path": [
    {"name": "StopBusinessThreadPool", "file": "./src/server/rpc_server.cpp", "line": 154},
    {"name": "RpcServer::Start", "file": "./src/server/rpc_server.cpp", "line": 188},
    {"name": "RpcServer::Stop", "file": "./src/server/rpc_server.cpp", "line": 331}
  ],
  "depth": 3
}
```

- 使用 BFS 找最短调用路径
- 用途：理解函数间的调用关系、调试调用链、代码审查

## 调用图指标

统计调用图的关键指标：

```bash
codegraph metrics
```

输出示例：
```json
{
  "total_function_nodes": 579,
  "total_call_edges": 1595,
  "circular_dependencies": 2,
  "max_call_depth": 10,
  "avg_call_depth": 3.93,
  "most_called": [
    {"name": "AppendMilliseconds3", "file": "./src/common/async_logger.cpp", "callers": 87},
    {"name": "Connection::AssertOwnerThread", "file": "./src/server/connection.cpp", "callers": 63}
  ],
  "most_calling": [
    {"name": "RpcServer::Start", "file": "./src/server/rpc_server.cpp", "callees": 11}
  ]
}
```

指标说明：
- **most_called**（入度排名）：被调用最多的函数，通常是核心基础设施代码
- **most_calling**（出度排名）：调用最多的函数，通常是编排/协调逻辑
- **max_call_depth**：从入口点到叶子节点的最长调用链
- **circular_dependencies**：循环依赖数量（Tarjan SCC）

## 影响链分析

分析修改一个符号会影响哪些代码，以及通过什么调用路径影响：

```bash
codegraph impact-chain Connection::Serve
```

输出示例：
```json
{
  "source": {"name": "Connection::Serve", "file": "./src/server/connection.cpp", "line": 830},
  "impact": [
    {
      "name": "Connection::HandleOneRequest",
      "file": "./src/server/connection.cpp",
      "line": 1012,
      "path": ["Connection::Serve", "Connection::HandleOneRequest"],
      "depth": 2
    },
    {
      "name": "RpcServer::Stop",
      "file": "./src/server/rpc_server.cpp",
      "line": 331,
      "path": ["Connection::Serve", "Connection::HandleOneRequest", "ServiceRegistry::Find", "RpcServer::Stop"],
      "depth": 4
    }
  ]
}
```

与 `codegraph impact` 的区别：
- `impact` 只返回受影响的节点集合
- `impact-chain` 额外返回从源到每个受影响节点的调用路径，回答"为什么改 X 会影响 Y"

## 可视化导出

将调用图导出为 Graphviz DOT 格式：

```bash
# 导出特定符号的调用图
codegraph export --dot --symbol RpcServer::Start --depth 2 > graph.dot

# 导出完整调用图
codegraph export --dot > full_graph.dot

# 渲染为 SVG
dot -Tsvg graph.dot -o graph.svg

# 渲染为 PNG
dot -Tpng graph.dot -o graph.png
```

节点颜色说明：
- 浅蓝色：函数/方法
- 浅绿色：类/结构体
- 浅橙色：其他符号（文件、变量等）

边类型说明：
- `calls`：函数调用关系
- `contains`：文件包含符号

## 语义搜索

使用 embedding 模型进行语义搜索，可以用自然语言描述找到相关代码：

```bash
# 1. 安装依赖
pip install sentence-transformers

# 2. 生成嵌入（首次使用或索引更新后）
codegraph embed

# 3. 语义搜索
codegraph search-semantic "处理网络连接的函数"
codegraph search-semantic "handle authentication"
codegraph search-semantic "发送RPC请求"
```

## MCP 集成

### Claude Code

在项目根目录创建 `.mcp.json`：

```json
{
  "mcpServers": {
    "codegraph": {
      "command": "/path/to/codegraph",
      "args": ["serve", "--mcp"],
      "cwd": "/your/cpp/project"
    }
  }
}
```

### OpenCode

编辑 `~/.config/opencode/opencode.json`：

```json
{
  "mcp": {
    "codegraph": {
      "type": "local",
      "command": ["/path/to/codegraph", "serve", "--mcp"]
    }
  }
}
```

### MCP 工具列表

| 工具 | 用途 | 输入 |
|------|------|------|
| `codegraph_search` | 搜索符号 | query, limit |
| `codegraph_context` | 获取符号上下文（支持类聚合） | symbol, limit, max_depth |
| `codegraph_callers` | 查找谁调用了这个函数 | symbol, max_depth |
| `codegraph_callees` | 查找这个函数调用了什么 | symbol, max_depth |
| `codegraph_impact` | 影响分析 | symbol, max_depth |
| `codegraph_node` | 获取单个符号详情 | symbol |
| `codegraph_status` | 索引统计 | - |
| `codegraph_files` | 列出已索引文件 | - |
| `codegraph_search_semantic` | 语义搜索 | query, limit |
| `codegraph_change_impact` | 代码变更影响分析 | ref |

## 支持的语言

| 语言 | 扩展名 | 状态 |
|------|--------|------|
| C++ | .cpp .cc .cxx .h .hpp .hxx .hh | 完整支持 |
| Python | .py | 完整支持（含调用边） |

扩展其他语言：实现 `LanguageExtractor` 接口，注册到 `create_extractor()` 工厂函数。

## 依赖

- SQLite3
- nlohmann_json
- tree-sitter（通过 npm 安装，C 源码编译进项目）
- pthread
- sentence-transformers（可选，用于语义搜索）

## 测试

```bash
cd build
ctest --output-on-failure
```

当前 `test_basic` 覆盖数据库 CRUD、FTS qualified-name 查询、C++/Python 抽取、成员调用抽取、上下文建边、CLI 增量索引、同名函数解析、Tarjan SCC、路径查找、图指标和 impact-chain。
