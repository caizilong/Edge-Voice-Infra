#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import json
import sys

from edge_rag.json_util import dumps_compact


def main():
    parser = argparse.ArgumentParser(description="Run SQLite hybrid RAG queries and emit JSON.")
    parser.add_argument("--input", help="JSON file containing the query payload")
    parser.add_argument("--db", required=True, help="SQLite knowledge database")
    parser.add_argument("--model", required=True, help="SentenceTransformer model directory")
    parser.add_argument("--top-k", type=int, default=3)
    parser.add_argument("--candidate-k", type=int, default=30)
    parser.add_argument("--threshold", type=float, default=0.35)
    parser.add_argument("--context-chars", type=int, default=1600)
    parser.add_argument("--worker", action="store_true", help="Read JSON requests from stdin and write JSON lines to stdout")
    args = parser.parse_args()

    from edge_rag.search import HybridRagIndex

    index = HybridRagIndex(args.db, args.model)
    try:
        if args.worker:
            index.warmup()
            return run_worker(index, args)
        if not args.input:
            raise ValueError("--input is required outside --worker mode")
        with open(args.input, "r", encoding="utf-8") as f:
            payload = json.load(f)
        result = run_query(index, payload, args)
    finally:
        index.close()

    print(dumps_compact(result))
    return 0


def run_query(index, payload, args):
    query = payload.get("query", "")
    if not isinstance(query, str) or not query.strip():
        raise ValueError("query is empty")
    return index.search(
        query.strip(),
        top_k=args.top_k,
        candidate_k=args.candidate_k,
        threshold=args.threshold,
        context_chars=args.context_chars,
    )


def run_worker(index, args):
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            payload = json.loads(line)
            result = run_query(index, payload, args)
        except Exception as exc:
            result = {
                "error": {
                    "code": -1,
                    "message": str(exc),
                }
            }
        print(dumps_compact(result), flush=True)
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
