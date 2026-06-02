#!/usr/bin/env python3
"""
Codegraph semantic embedding tool.

Usage:
  python3 embed.py index <db_path>   -- Generate embeddings for all nodes
  python3 embed.py query <db_path> <query> [--top N]  -- Semantic search
"""

import sys
import sqlite3
import struct
import json

def get_model():
    """Load sentence-transformers model (lazy import)."""
    from sentence_transformers import SentenceTransformer
    return SentenceTransformer('all-MiniLM-L6-v2')

def ensure_embeddings_table(db_path):
    """Create node_embeddings table if it doesn't exist."""
    conn = sqlite3.connect(db_path)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS node_embeddings (
            node_id INTEGER PRIMARY KEY,
            embedding BLOB NOT NULL,
            FOREIGN KEY (node_id) REFERENCES nodes(id)
        )
    """)
    conn.commit()
    return conn

def embedding_to_blob(embedding):
    """Convert float list to bytes for SQLite BLOB storage."""
    return struct.pack(f'{len(embedding)}f', *embedding)

def blob_to_embedding(blob):
    """Convert SQLite BLOB back to float list."""
    count = len(blob) // 4
    return list(struct.unpack(f'{count}f', blob))

def cosine_similarity(a, b):
    """Compute cosine similarity between two vectors."""
    dot = sum(x * y for x, y in zip(a, b))
    norm_a = sum(x * x for x in a) ** 0.5
    norm_b = sum(x * x for x in b) ** 0.5
    if norm_a == 0 or norm_b == 0:
        return 0.0
    return dot / (norm_a * norm_b)

def build_text_for_node(node):
    """Build text representation of a node for embedding."""
    # node = (id, kind, name, qualified_name, signature, docstring, file_path, ...)
    node_id = node[0]
    kind = node[1]
    name = node[2]
    qualified_name = node[3]
    signature = node[4]
    docstring = node[5]
    file_path = node[6]
    parts = []
    if kind:
        kind_names = {
            0: "file", 1: "function", 2: "method", 3: "class",
            4: "struct", 5: "enum", 6: "enum_member", 7: "variable",
            8: "type_alias", 9: "namespace", 10: "import",
            11: "parameter", 12: "field"
        }
        parts.append(kind_names.get(kind, "symbol"))
    if name:
        parts.append(name)
    if qualified_name and qualified_name != name:
        parts.append(qualified_name)
    if signature:
        parts.append(signature)
    if docstring:
        parts.append(docstring[:200])
    return " ".join(parts)

def cmd_index(db_path):
    """Generate embeddings for all nodes in the database."""
    conn = ensure_embeddings_table(db_path)
    model = get_model()

    # Get all nodes
    cursor = conn.execute("""
        SELECT id, kind, name, qualified_name, signature, docstring, file_path
        FROM nodes
    """)
    nodes = cursor.fetchall()

    if not nodes:
        print("No nodes found in database.")
        return

    print(f"Generating embeddings for {len(nodes)} nodes...")

    # Build texts
    texts = []
    node_ids = []
    for node in nodes:
        node_id = node[0]
        text = build_text_for_node(node)
        if text.strip():
            texts.append(text)
            node_ids.append(node_id)

    # Batch encode
    embeddings = model.encode(texts, show_progress_bar=True, batch_size=64)

    # Store
    conn.execute("DELETE FROM node_embeddings")
    for node_id, emb in zip(node_ids, embeddings):
        conn.execute(
            "INSERT INTO node_embeddings (node_id, embedding) VALUES (?, ?)",
            (node_id, embedding_to_blob(emb.tolist()))
        )
    conn.commit()
    print(f"Stored {len(node_ids)} embeddings.")

def cmd_query(db_path, query, top_n=10):
    """Semantic search for nodes."""
    conn = ensure_embeddings_table(db_path)
    model = get_model()

    # Check if embeddings exist
    count = conn.execute("SELECT COUNT(*) FROM node_embeddings").fetchone()[0]
    if count == 0:
        print("No embeddings found. Run 'embed.py index <db_path>' first.")
        return

    # Generate query embedding
    query_emb = model.encode([query])[0].tolist()

    # Load all embeddings and compute similarity
    cursor = conn.execute("""
        SELECT ne.node_id, ne.embedding, n.kind, n.name, n.file_path, n.line, n.signature
        FROM node_embeddings ne
        JOIN nodes n ON ne.node_id = n.id
    """)

    results = []
    for row in cursor:
        node_id, blob, kind, name, file_path, line, signature = row
        emb = blob_to_embedding(blob)
        sim = cosine_similarity(query_emb, emb)
        results.append({
            "similarity": round(sim, 4),
            "kind": kind,
            "name": name,
            "file": file_path,
            "line": line,
            "signature": signature or ""
        })

    # Sort by similarity
    results.sort(key=lambda x: x["similarity"], reverse=True)
    results = results[:top_n]

    # Map kind int to string
    kind_names = {
        0: "file", 1: "function", 2: "method", 3: "class",
        4: "struct", 5: "enum", 6: "enum_member", 7: "variable",
        8: "type_alias", 9: "namespace", 10: "import",
        11: "parameter", 12: "field"
    }
    for r in results:
        r["kind"] = kind_names.get(r["kind"], "unknown")

    print(json.dumps(results, indent=2, ensure_ascii=False))

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    cmd = sys.argv[1]
    db_path = sys.argv[2]

    if cmd == "index":
        cmd_index(db_path)
    elif cmd == "query":
        query = sys.argv[3] if len(sys.argv) > 3 else ""
        top_n = 10
        if "--top" in sys.argv:
            idx = sys.argv.index("--top")
            if idx + 1 < len(sys.argv):
                top_n = int(sys.argv[idx + 1])
        if not query:
            print("Usage: embed.py query <db_path> <query> [--top N]")
            sys.exit(1)
        cmd_query(db_path, query, top_n)
    else:
        print(f"Unknown command: {cmd}")
        print(__doc__)
        sys.exit(1)
