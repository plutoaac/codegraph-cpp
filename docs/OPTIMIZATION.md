# 性能优化

## 测试项目

- **项目**: mini-rpc 框架 (93 个 C++ 文件, 28,405 行)
- **文件**: 93 个 C++ 文件, 28,405 行代码

## 优化前后对比

| 指标 | 优化前 | 优化后 | 提升 |
|------|--------|--------|------|
| **索引耗时** | 24.0s | 11.0s | **2.2x** |
| **未决引用** | 7,664 | 5,935 | **23% 减少** |
| **数据库操作** | ~18,717 次 INSERT | 批量插入 | **大幅减少** |
| **MCP 响应大小** | ~8KB (16字段/节点) | ~700B (5字段/节点) | **10x 压缩** |
| **节点数** | 3,943 | 3,676 | 过滤变量声明 |
| **边数** | 6,482 | 5,158 | 去重 + 过滤 |

## 已完成的优化

### 1. 过滤 C++ 内置符号

`cpp_extractor.cpp` 中添加 `is_builtin_symbol()` 过滤函数，排除：
- C++ 运算符：`static_cast`, `dynamic_cast`, `reinterpret_cast`, `const_cast`
- 内置类型：`size_t`, `int8_t`, `uint8_t` 等
- STL 函数：`to_string`, `sort`, `min`, `max`, `move`, `forward`
- Protobuf 内置：`GetEmptyStringAlreadyInited`, `InitSCC`, `GetArena` 等

**效果**: 未决引用从 7,664 降到 5,935（减少 23%）

### 2. 预编译语句缓存

`database.cpp` 中缓存常用的 prepared statement，避免每次 INSERT 都 prepare/finalize。

**效果**: 索引时间从 24s 降到 11s（2.2x 加速）

### 3. RAII 管理 SQLite 语句

用 `StmtGuard`（`std::unique_ptr<sqlite3_stmt, StmtDeleter>`）替代 19 处手动 `sqlite3_finalize` 调用。

### 4. MCP 响应精简

| 字段 | 优化前 | 优化后 |
|------|--------|--------|
| node_to_json | 16 字段 | 5 字段（kind/name/file/line + 可选 signature） |
| edge_to_json | 5 字段 | 3 字段（src/dst/kind） |
| JSON 格式 | `.dump(2)` 缩进 | `.dump()` 紧凑 |

**效果**: search 5 条结果从 ~6KB 降到 ~700B

### 5. Watch / Index 增量重建

文件变更时只重建变更文件，并基于旧调用图把调用变更文件符号的 caller 文件加入本轮重建。这样可以保留未改文件的节点 ID，同时避免目标文件重建后跨文件入边丢失。

### 6. 搜索结果排序

`SearchNodes()` 按 kind 优先级排序：functions/classes > variables > imports

### 7. Token 节省统计

CLI 的 `search` 命令在 stderr 输出 token 节省统计：

```
$ codegraph search RpcServer
...
[codegraph] 591 tokens (vs 8048 tokens to read 4 source files)
```

### 8. 边去重

索引时对边进行去重，避免同一个调用关系被多次记录。

**效果**: 边数从 6,482 降到 5,158（减少 20%）

### 9. 变量声明过滤

修复 `classify_cpp_node()` 逻辑，区分变量声明和函数定义：
- `RpcServer server(...)` → 变量（不是函数）
- `void foo(int x) { ... }` → 函数（有函数体）

**效果**: 节点数从 3,943 降到 3,676（减少 7%）

### 10. Dead Code 检测优化

智能过滤假阳性：
- 排除头文件中的公共 API
- 排除 test/benchmark/demo 文件
- 排除 main() 函数、析构函数
- 排除有限定名的方法（类成员）
- 排除常见变量名（lock, guard, buffer 等）

**效果**: 从 534 个假阳性降到 1 个真正的死代码

### 11. WAL 模式

启用 SQLite WAL（Write-Ahead Logging）模式，支持并发读写。

### 12. 解析错误检查

tree-sitter 解析后检查 `ts_node_has_error()`，跳过有语法错误的文件，避免产生垃圾节点。

### 13. Git Flag 注入防护

`run_git_diff()` 中在 ref 参数前添加 `--` 分隔符，防止恶意 ref 被解释为 git 选项。

### 14. DOT 导出转义

导出 Graphviz DOT 格式时转义特殊字符（`"`, `<`, `>`, `{`, `}`, `|`）。

### 15. 类上下文聚合

对类/结构体使用 `context` 命令时，自动聚合该类所有方法的调用关系，支持 `.h`/`.hpp`/`.hxx`/`.hh` 到 `.cpp`/`.cc`/`.cxx` 的文件映射。

## 未完成的优化方向

| 方向 | 预期提升 | 难度 |
|------|---------|------|
| 并行提取（多线程解析） | 4x | 中 |
| 更多噪音过滤（this->*, *.size()） | 进一步减少未决引用 | 低 |
| 存储压缩（qualified_name 公共前缀） | 数据库 -30% | 中 |
| TSParser 缓存（避免每次 extract 创建） | 索引 -10% | 低 |

## 测量命令

```bash
# 重新索引并计时
cd /path/to/your/project
rm -rf .codegraph
time codegraph init -i .

# 检查数据库
sqlite3 .codegraph/index "
  SELECT 'nodes: ' || COUNT(*) FROM nodes;
  SELECT 'edges: ' || COUNT(*) FROM edges;
  SELECT 'unresolved_refs: ' || COUNT(*) FROM unresolved_refs;
"

# 测试查询
codegraph search RpcServer
codegraph context RpcServer
codegraph dead-code
codegraph export --dot --symbol RpcServer::Start --depth 1
```
