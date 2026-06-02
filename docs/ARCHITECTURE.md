# 架构

## 整体结构

```
┌─────────────────────────────────────────────────────────────────────┐
│                          使用方式                                    │
├─────────────────────────────────────────────────────────────────────┤
│  ┌──────────────┐    ┌──────────────────┐    ┌──────────────────┐  │
│  │   CLI 命令    │    │  MCP Server      │    │  RPC Server      │  │
│  │  (直接使用)   │    │  (Claude Code)   │    │  (远程访问)       │  │
│  └──────┬───────┘    └────────┬─────────┘    └────────┬─────────┘  │
│         └─────────────────────┼────────────────────────┘            │
│                    ┌──────────▼──────────┐                          │
│                    │    codegraph_core    │                          │
│                    └──────────┬──────────┘                          │
└───────────────────────────────┼─────────────────────────────────────┘
                 ┌──────────────┼──────────────┐
          ┌──────▼──────┐ ┌────▼────┐ ┌───────▼───────┐ ┌───────────┐
          │  Extractor   │ │Database │ │  Traverser    │ │   Diff    │
          │  (tree-sitter│ │(SQLite) │ │  (BFS 图遍历)  │ │  Parser   │
          │   代码解析)   │ │ +FTS5   │ │               │ │ (git diff)│
          └─────────────┘ └─────────┘ └───────────────┘ └───────────┘
```

## 源码目录

```
src/
├── main.cpp                    # CLI 入口（所有命令实现）
├── core/types.cpp              # 数据类型定义（Node, Edge, NodeKind, EdgeKind）
├── db/database.cpp             # SQLite 数据库层（CRUD + 批量操作 + FTS）
├── extraction/
│   ├── extractor.cpp           # 通用提取接口
│   └── cpp_extractor.cpp       # C++/Python 提取逻辑（tree-sitter AST 解析）
├── graph/traverser.cpp         # BFS 图遍历（callers/callees/impact/find_path）
├── search/fts_search.cpp       # FTS5 全文搜索
├── context/context_builder.cpp # 上下文组装（支持类聚合）
├── sync/file_watcher.cpp       # inotify 文件监听
├── diff/diff_parser.cpp        # Git diff 解析
└── mcp/mcp_server.cpp          # MCP JSON-RPC 服务器

include/codegraph/
├── core/types.h
├── db/database.h
├── extraction/extractor.h
├── graph/traverser.h
├── search/fts_search.h
├── context/context_builder.h
├── sync/file_watcher.h
├── diff/diff_parser.h
└── mcp/mcp_server.h
```

## 索引流程

```
源文件 (.cpp/.h/.py)
        │
        ▼
┌───────────────┐     tree-sitter      ┌──────────────┐
│  detect_lang() │ ───────────────────► │  Extractor   │
│  识别语言      │                      │  解析 AST     │
└───────────────┘                      └──────┬───────┘
                                              ▼
                                    ┌──────────────────┐
                                    │  ExtractionResult │
                                    │  - nodes[]        │
                                    │  - unresolved[]   │
                                    └────────┬─────────┘
                                             ▼
                                    ┌──────────────────┐
                                    │  index_extracted  │
                                    │  _files()         │
                                    │  - 创建文件节点    │
                                    │  - 批量插入节点    │
                                    │  - 创建 contains 边│
                                    │  - 解析调用边      │
                                    │  - 去重边          │
                                    └────────┬─────────┘
                                             ▼
                                    ┌──────────────────┐
                                    │  SQLite + FTS5    │
                                    │  nodes / edges    │
                                    │  nodes_fts        │
                                    │  unresolved_refs  │
                                    └──────────────────┘
```

## Watch / Incremental Index 模式

监听文件变更后触发增量索引。实现会扫描目录找出真正变更的源文件，只重建这些文件；如果旧图中存在“未改文件调用变更文件”的入边，还会把这些 caller 文件加入本轮重建，避免目标节点被删除重插后跨文件调用边丢失。

```
┌─────────────┐   inotify    ┌──────────────┐
│  文件系统    │ ──────────►  │ FileWatcher  │
└─────────────┘              └──────┬───────┘
                                    ▼
                           ┌──────────────────┐
                           │  index_directory  │
                           │  incremental=true │
                           │  - 找变更文件      │
                           │  - 调度 caller 文件│
                           │  - 批量重建节点/边 │
                           │  - resolve refs   │
                           └──────────────────┘
```

## Diff 影响分析

```
git diff --unified=0 <ref>
        │
        ▼
┌───────────────┐
│  parse_diff() │  解析变更文件和行号
└───────┬───────┘
        ▼
┌───────────────┐
│  find_nodes   │  查找变更行范围内的符号
│  _by_file()   │
└───────┬───────┘
        ▼
┌───────────────┐
│  get_impact() │  BFS 遍历调用图
│  (BFS)        │  找到所有受影响的符号
└───────┬───────┘
        ▼
   JSON 输出
   - changed_files
   - affected_symbols
   - impact
```

## 数据库 Schema

```sql
-- 符号节点
CREATE TABLE nodes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind INTEGER NOT NULL,          -- NodeKind 枚举值
    name TEXT NOT NULL,              -- 符号短名
    qualified_name TEXT,             -- 全限定名（如 RpcServer::Start）
    file_path TEXT NOT NULL,         -- 源文件路径
    language TEXT,                   -- cpp / python
    line INTEGER, col INTEGER,       -- 起始位置
    end_line INTEGER, end_col INTEGER, -- 结束位置
    signature TEXT,                  -- 函数签名
    docstring TEXT,                  -- 文档注释
    visibility TEXT,                 -- public/private/protected
    is_static INTEGER DEFAULT 0,
    is_const INTEGER DEFAULT 0,
    is_exported INTEGER DEFAULT 0
);

-- NodeKind 枚举
-- 0=File, 1=Function, 2=Method, 3=Class, 4=Struct
-- 5=Enum, 6=EnumMember, 7=Variable, 8=TypeAlias
-- 9=Namespace, 10=Import, 11=Parameter, 12=Field

-- 关系边
CREATE TABLE edges (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_id INTEGER NOT NULL,      -- 源节点 ID
    target_id INTEGER NOT NULL,      -- 目标节点 ID
    kind INTEGER NOT NULL,           -- EdgeKind 枚举值
    line INTEGER, col INTEGER,       -- 边的位置
    metadata TEXT,
    FOREIGN KEY (source_id) REFERENCES nodes(id),
    FOREIGN KEY (target_id) REFERENCES nodes(id)
);

-- EdgeKind 枚举
-- 0=Contains (文件包含符号)
-- 1=Calls (函数调用)
-- 2=Imports (导入)
-- 3=Exports (导出)
-- 4=Extends (继承)
-- 5=Implements (实现接口)
-- 6=References (引用)
-- 7=TypeOf (类型关系)
-- 8=Returns (返回类型)
-- 9=Overrides (重写)

-- 已索引文件
CREATE TABLE files (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    path TEXT UNIQUE NOT NULL,
    language TEXT,
    mtime INTEGER,                   -- 上次修改时间（增量索引用）
    size INTEGER
);

-- 未决引用（跨文件调用暂未解析的目标）
CREATE TABLE unresolved_refs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_node_id INTEGER NOT NULL,
    ref_name TEXT NOT NULL,
    ref_kind TEXT,
    line INTEGER, col INTEGER
);

-- FTS5 全文索引（加速搜索）
CREATE VIRTUAL TABLE nodes_fts USING fts5(
    name, qualified_name, signature, docstring, file_path,
    content='nodes', content_rowid='id'
);

-- FTS5 同步触发器
CREATE TRIGGER nodes_ai AFTER INSERT ON nodes BEGIN
    INSERT INTO nodes_fts(rowid, name, qualified_name, signature, docstring, file_path)
    VALUES (new.id, new.name, new.qualified_name, new.signature, new.docstring, new.file_path);
END;

CREATE TRIGGER nodes_ad AFTER DELETE ON nodes BEGIN
    INSERT INTO nodes_fts(nodes_fts, rowid, name, qualified_name, signature, docstring, file_path)
    VALUES ('delete', old.id, old.name, old.qualified_name, old.signature, old.docstring, old.file_path);
END;

CREATE TRIGGER nodes_au AFTER UPDATE ON nodes BEGIN
    INSERT INTO nodes_fts(nodes_fts, rowid, name, qualified_name, signature, docstring, file_path)
    VALUES ('delete', old.id, old.name, old.qualified_name, old.signature, old.docstring, old.file_path);
    INSERT INTO nodes_fts(rowid, name, qualified_name, signature, docstring, file_path)
    VALUES (new.id, new.name, new.qualified_name, new.signature, new.docstring, new.file_path);
END;

-- 性能索引
CREATE INDEX idx_nodes_name ON nodes(name);
CREATE INDEX idx_nodes_file ON nodes(file_path);
CREATE INDEX idx_edges_source ON edges(source_id);
CREATE INDEX idx_edges_target ON edges(target_id);
CREATE INDEX idx_edges_kind ON edges(kind);
```

## MCP 接入方式

### 直接 stdio（推荐）

```
┌─────────────┐   stdio JSON-RPC    ┌──────────────────┐
│ Claude Code  │ ◄──────────────────► │ codegraph serve  │
│  (MCP Client)│                     │    --mcp         │
└─────────────┘                      └────────┬─────────┘
                                              │
                                       ┌──────▼──────┐
                                       │   SQLite     │
                                       └─────────────┘
```

### RPC 桥接（远程/多客户端）

```
┌─────────────┐   stdio MCP    ┌──────────────┐   TCP RPC    ┌──────────────┐
│ Claude Code  │ ◄────────────► │ MCP Adapter  │ ◄──────────► │  RPC Server  │
└─────────────┘                └──────────────┘              └──────┬───────┘
                                                             ┌──────▼──────┐
                                                             │   SQLite    │
                                                             └─────────────┘
```
