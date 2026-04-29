#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import json
import os
import sys


def to_jsonable(value):
    if isinstance(value, dict):
        return {str(key): to_jsonable(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [to_jsonable(item) for item in value]
    if hasattr(value, "tolist"):
        try:
            return to_jsonable(value.tolist())
        except Exception:
            pass
    if hasattr(value, "item"):
        try:
            return value.item()
        except Exception:
            pass
    return value


def main():
    parser = argparse.ArgumentParser(description="Run one vehicle RAG query and emit JSON.")
    parser.add_argument("--input", required=True, help="JSON file containing the query payload")
    parser.add_argument("--model", required=True, help="SentenceTransformer model directory")
    parser.add_argument("--vector-db", required=True, help="Vector DB directory")
    parser.add_argument("--top-k", type=int, default=1)
    parser.add_argument("--threshold", type=float, default=0.5)
    args = parser.parse_args()

    with open(args.input, "r", encoding="utf-8") as f:
        payload = json.load(f)
    query = payload.get("query", "")
    if not query:
        raise ValueError("query is empty")

    from vehicle_vector_search import VehicleVectorSearch

    searcher = VehicleVectorSearch(
        os.path.normpath(args.model),
        os.path.normpath(args.vector_db),
    )
    results = searcher.search(query, top_k=args.top_k, threshold=args.threshold)

    items = []
    for item in results:
        items.append({
            "id": to_jsonable(item.get("id")),
            "text": item.get("text", ""),
            "section": item.get("section", ""),
            "subsection": item.get("subsection", ""),
            "similarity": to_jsonable(item.get("similarity", 0.0)),
            "metadata": to_jsonable(item.get("metadata", {})),
        })

    context = "\n".join(item["text"] for item in items)
    out = {
        "query": query,
        "context": context,
        "results": items,
        "model": os.path.normpath(args.model),
        "vector_db": os.path.normpath(args.vector_db),
    }
    print(json.dumps(out, ensure_ascii=False, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(json.dumps({
            "error": {
                "code": -1,
                "message": str(exc),
            }
        }, ensure_ascii=False), file=sys.stdout)
        raise SystemExit(1)
