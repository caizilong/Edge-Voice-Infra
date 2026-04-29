#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import json
import sys

from edge_rag.json_util import dumps_compact


def main():
    parser = argparse.ArgumentParser(description="Run one SQLite hybrid RAG query and emit JSON.")
    parser.add_argument("--input", required=True, help="JSON file containing the query payload")
    parser.add_argument("--db", required=True, help="SQLite knowledge database")
    parser.add_argument("--model", required=True, help="SentenceTransformer model directory")
    parser.add_argument("--top-k", type=int, default=3)
    parser.add_argument("--candidate-k", type=int, default=30)
    parser.add_argument("--threshold", type=float, default=0.35)
    parser.add_argument("--context-chars", type=int, default=1600)
    args = parser.parse_args()

    with open(args.input, "r", encoding="utf-8") as f:
        payload = json.load(f)
    query = payload.get("query", "")
    if not isinstance(query, str) or not query.strip():
        raise ValueError("query is empty")

    from edge_rag.search import HybridRagIndex

    index = HybridRagIndex(args.db, args.model)
    try:
        result = index.search(
            query.strip(),
            top_k=args.top_k,
            candidate_k=args.candidate_k,
            threshold=args.threshold,
            context_chars=args.context_chars,
        )
    finally:
        index.close()

    print(dumps_compact(result))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(dumps_compact({
            "error": {
                "code": -1,
                "message": str(exc),
            }
        }), file=sys.stdout)
        raise SystemExit(1)
