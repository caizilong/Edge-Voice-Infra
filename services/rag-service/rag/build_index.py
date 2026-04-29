#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description="Build a SQLite RAG knowledge base from txt/md files.")
    parser.add_argument("--input", required=True, help="Input txt/md file or directory")
    parser.add_argument("--db", required=True, help="Output SQLite knowledge database")
    parser.add_argument("--model", required=True, help="SentenceTransformer model directory")
    parser.add_argument("--chunk-size", type=int, default=450)
    parser.add_argument("--overlap", type=int, default=80)
    args = parser.parse_args()

    from edge_rag.documents import load_chunks
    from edge_rag.embeddings import encode_texts
    from edge_rag.store import connect, insert_index, reset_schema

    input_path = Path(args.input)
    chunks = load_chunks(input_path, args.chunk_size, args.overlap)
    if not chunks:
        raise RuntimeError(f"no txt/md chunks found in {input_path}")

    embeddings = encode_texts(args.model, [chunk.text for chunk in chunks])
    con = connect(Path(args.db))
    try:
        reset_schema(con)
        insert_index(con, chunks, embeddings, args.model, args.chunk_size, args.overlap)
    finally:
        con.close()

    print(f"built {args.db}: chunks={len(chunks)} model={args.model}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"build_index failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
