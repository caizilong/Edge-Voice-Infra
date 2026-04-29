import json
import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable

from .documents import Chunk
from .embeddings import pack_f32, pack_i8, quantize_i8, vector_norm
from .json_util import dumps_compact
from .text import searchable_text


SCHEMA = """
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;

DROP TABLE IF EXISTS chunks_fts;
DROP TABLE IF EXISTS chunks;
DROP TABLE IF EXISTS documents;
DROP TABLE IF EXISTS index_meta;

CREATE TABLE documents (
  doc_id INTEGER PRIMARY KEY,
  source TEXT NOT NULL,
  title TEXT NOT NULL,
  metadata_json TEXT NOT NULL DEFAULT '{}'
);

CREATE TABLE chunks (
  chunk_id INTEGER PRIMARY KEY,
  doc_id INTEGER NOT NULL REFERENCES documents(doc_id),
  chunk_text TEXT NOT NULL,
  section TEXT NOT NULL DEFAULT '',
  subsection TEXT NOT NULL DEFAULT '',
  page INTEGER,
  chunk_index INTEGER NOT NULL,
  metadata_json TEXT NOT NULL DEFAULT '{}',
  embedding_f32 BLOB NOT NULL,
  embedding_i8 BLOB NOT NULL,
  embedding_norm REAL NOT NULL
);

CREATE VIRTUAL TABLE chunks_fts USING fts5(
  chunk_text,
  section,
  subsection,
  source,
  content='',
  tokenize='unicode61'
);

CREATE TABLE index_meta (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
"""


def connect(db_path: Path) -> sqlite3.Connection:
    db_path.parent.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(str(db_path))
    con.row_factory = sqlite3.Row
    return con


def reset_schema(con: sqlite3.Connection):
    con.executescript(SCHEMA)


def insert_index(con: sqlite3.Connection, chunks: Iterable[Chunk], embeddings, model_path: str,
                 chunk_size: int, overlap: int):
    source_to_doc_id = {}
    chunk_rows = []
    fts_rows = []

    for chunk, embedding in zip(chunks, embeddings):
        doc_id = source_to_doc_id.get(chunk.source)
        if doc_id is None:
            cur = con.execute(
                "INSERT INTO documents(source, title, metadata_json) VALUES (?, ?, ?)",
                (chunk.source, chunk.title, dumps_compact({"source": chunk.source})),
            )
            doc_id = int(cur.lastrowid)
            source_to_doc_id[chunk.source] = doc_id

        cur = con.execute(
            """INSERT INTO chunks(
                 doc_id, chunk_text, section, subsection, page, chunk_index,
                 metadata_json, embedding_f32, embedding_i8, embedding_norm
               ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
            (
                doc_id,
                chunk.text,
                chunk.section,
                chunk.subsection,
                chunk.page,
                chunk.chunk_index,
                dumps_compact(chunk.metadata),
                pack_f32(embedding),
                pack_i8(quantize_i8(embedding)),
                vector_norm(embedding),
            ),
        )
        chunk_id = int(cur.lastrowid)
        chunk_rows.append(chunk_id)
        fts_rows.append((
            chunk_id,
            searchable_text(chunk.text),
            searchable_text(chunk.section),
            searchable_text(chunk.subsection),
            chunk.source,
        ))

    con.executemany(
        "INSERT INTO chunks_fts(rowid, chunk_text, section, subsection, source) VALUES (?, ?, ?, ?, ?)",
        fts_rows,
    )

    embedding_dim = 0
    if len(chunk_rows) > 0:
        first = con.execute("SELECT length(embedding_f32) AS n FROM chunks LIMIT 1").fetchone()
        embedding_dim = int(first["n"] // 4)

    meta = {
        "created_at": datetime.now(timezone.utc).isoformat(),
        "model_path": model_path,
        "embedding_dimension": str(embedding_dim),
        "chunk_count": str(len(chunk_rows)),
        "document_count": str(len(source_to_doc_id)),
        "chunk_size": str(chunk_size),
        "overlap": str(overlap),
        "backend": "sqlite-fts5-python-vector",
    }
    con.executemany(
        "INSERT INTO index_meta(key, value) VALUES (?, ?)",
        sorted(meta.items()),
    )
    con.commit()


def load_meta(con: sqlite3.Connection) -> dict:
    rows = con.execute("SELECT key, value FROM index_meta").fetchall()
    return {row["key"]: row["value"] for row in rows}


def parse_metadata(raw: str) -> dict:
    try:
        value = json.loads(raw or "{}")
    except json.JSONDecodeError:
        return {}
    return value if isinstance(value, dict) else {}
