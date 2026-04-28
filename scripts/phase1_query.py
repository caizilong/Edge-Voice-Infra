#!/usr/bin/env python3
import argparse
import json
import socket
import sys
import time


def recv_json(sock_file):
    line = sock_file.readline()
    if not line:
        raise RuntimeError("gateway closed the connection")
    return json.loads(line.decode("utf-8"))


def send_json(sock_file, payload):
    raw = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    sock_file.write(raw.encode("utf-8") + b"\n")
    sock_file.flush()
    return recv_json(sock_file)


def request_id(prefix, index):
    return f"{prefix}-{int(time.time() * 1000)}-{index}"


def main():
    parser = argparse.ArgumentParser(description="Phase1 gateway -> RAG -> LLM -> TTS smoke client")
    parser.add_argument("query", help="Query text to run through the phase1 services")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10001)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    with socket.create_connection((args.host, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        sock_file = sock.makefile("rwb", buffering=0)

        rag_setup = send_json(sock_file, {
            "request_id": request_id("setup-rag", 1),
            "work_id": "rag",
            "action": "setup",
            "object": "rag.setup",
            "data": {},
        })
        llm_setup = send_json(sock_file, {
            "request_id": request_id("setup-llm", 2),
            "work_id": "llm",
            "action": "setup",
            "object": "llm.setup",
            "data": {"model": "phase1-mock"},
        })
        tts_setup = send_json(sock_file, {
            "request_id": request_id("setup-tts", 3),
            "work_id": "tts",
            "action": "setup",
            "object": "tts.setup",
            "data": {"output_dir": "/tmp/edge_voice_tts"},
        })

        rag_work_id = rag_setup["work_id"]
        llm_work_id = llm_setup["work_id"]
        tts_work_id = tts_setup["work_id"]

        rag_response = send_json(sock_file, {
            "request_id": request_id("rag", 4),
            "work_id": rag_work_id,
            "action": "inference",
            "object": "rag.request",
            "data": {"query": args.query},
        })
        rag_data = rag_response.get("data", {})

        llm_response = send_json(sock_file, {
            "request_id": request_id("llm", 5),
            "work_id": llm_work_id,
            "action": "inference",
            "object": "llm.request",
            "data": {
                "query": args.query,
                "context": rag_data.get("context", ""),
                "prompt": rag_data.get("prompt", args.query),
            },
        })
        llm_data = llm_response.get("data", {})
        llm_text = llm_data.get("text", "")

        tts_response = send_json(sock_file, {
            "request_id": request_id("tts", 6),
            "work_id": tts_work_id,
            "action": "inference",
            "object": "tts.request",
            "data": {"text": llm_text},
        })

    result = {
        "work_ids": {
            "rag": rag_work_id,
            "llm": llm_work_id,
            "tts": tts_work_id,
        },
        "rag": rag_response,
        "llm": llm_response,
        "tts": tts_response,
    }
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"phase1_query failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
