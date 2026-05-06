#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import socket
import sys
import time


def recv_json(sock_file):
    line = sock_file.readline()
    if not line:
        raise RuntimeError("gateway closed the connection")
    return json.loads(line.decode("utf-8"))


def response_error(response):
    error = response.get("error") if isinstance(response, dict) else None
    if isinstance(error, dict):
        return error
    if isinstance(error, str):
        if not error or error == "None":
            return {"code": 0, "message": ""}
        try:
            parsed = json.loads(error)
        except json.JSONDecodeError:
            return {"code": -1, "message": error}
        if isinstance(parsed, dict):
            return parsed
        return {"code": -1, "message": str(parsed)}
    if error is None:
        return {"code": 0, "message": ""}
    return {"code": -1, "message": str(error)}


def ensure_ok(label, response):
    error = response_error(response)
    if error.get("code") != 0:
        raise RuntimeError(f"{label} failed: object={response.get('object')} error={error}")


def send_payload(sock_file, payload):
    raw = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    sock_file.write(raw.encode("utf-8") + b"\n")
    sock_file.flush()


def send_json(sock_file, payload):
    send_payload(sock_file, payload)
    return recv_json(sock_file)


def send_json_timed(sock_file, payload):
    start = time.perf_counter()
    response = send_json(sock_file, payload)
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    return response, elapsed_ms


def send_json_stream(sock_file, payload, final_object):
    start = time.perf_counter()
    send_payload(sock_file, payload)

    deltas = []
    first_delta_ms = None
    while True:
        response = recv_json(sock_file)
        obj = response.get("object")
        error = response_error(response)
        if obj == "llm.delta":
            if first_delta_ms is None:
                first_delta_ms = (time.perf_counter() - start) * 1000.0
            deltas.append(response.get("data", {}).get("delta", ""))
            continue

        response.setdefault("data", {})
        if isinstance(response["data"], dict):
            response["data"].setdefault("stream_text", "".join(deltas))
            response["data"].setdefault("stream_delta_count", len(deltas))
            response["data"].setdefault("client_first_delta_ms", first_delta_ms)
            response["data"].setdefault("client_total_ms", (time.perf_counter() - start) * 1000.0)
        if obj == final_object or error.get("code") != 0:
            return response


def request_id(prefix, index):
    return f"{prefix}-{int(time.time() * 1000)}-{index}"


def metric_number(value):
    return round(float(value), 3) if isinstance(value, (int, float)) else value


def main():
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Phase1 gateway -> RAG -> LLM -> TTS smoke client")
    parser.add_argument("query", help="Query text to run through the phase1 services")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10001)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--require-real-llm", action="store_true")
    parser.add_argument("--require-real-tts", action="store_true")
    parser.add_argument("--max-new-tokens", type=int, default=256)
    parser.add_argument("--context-chars", type=int, default=1600)
    parser.add_argument("--tts-speaker-id", type=int, default=0)
    parser.add_argument("--tts-length-scale", type=float, default=1.0)
    parser.add_argument("--tts-timeout-sec", type=int, default=120)
    parser.add_argument("--tts-model", default="")
    parser.add_argument("--edge-tts-executable", default="")
    args = parser.parse_args()

    setup_start = time.perf_counter()
    with socket.create_connection((args.host, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        sock_file = sock.makefile("rwb", buffering=0)

        rag_setup, rag_setup_ms = send_json_timed(sock_file, {
            "request_id": request_id("setup-rag", 1),
            "work_id": "rag",
            "action": "setup",
            "object": "rag.setup",
            "data": {
                "model": str(repo / "models" / "rag" / "text2vec-base-chinese"),
                "knowledge_db": str(repo / "services" / "rag-service" / "data" / "knowledge" / "vehicle_knowledge.sqlite"),
                "script": str(repo / "services" / "rag-service" / "rag" / "query.py"),
                "top_k": 3,
                "candidate_k": 30,
                "threshold": 0.35,
                "context_chars": args.context_chars,
            },
        })
        ensure_ok("rag setup", rag_setup)

        llm_setup, llm_setup_ms = send_json_timed(sock_file, {
            "request_id": request_id("setup-llm", 2),
            "work_id": "llm",
            "action": "setup",
            "object": "llm.setup",
            "data": {
                "model": str(repo / "models" / "llm" / "Qwen3-1.7B.rkllm"),
                "max_new_tokens": args.max_new_tokens,
                "max_context_len": 4096,
                "require_real_backend": args.require_real_llm,
            },
        })
        ensure_ok("llm setup", llm_setup)
        if args.require_real_llm and llm_setup.get("data", {}).get("backend") != "rkllm":
            raise RuntimeError(f"llm setup did not use rkllm backend: {llm_setup}")

        tts_config = {
            "output_dir": "/tmp/edge_voice_tts",
            "speaker_id": args.tts_speaker_id,
            "length_scale": args.tts_length_scale,
            "timeout_sec": args.tts_timeout_sec,
            "require_real_backend": args.require_real_tts,
        }
        if args.tts_model:
            tts_config["model"] = args.tts_model
        if args.edge_tts_executable:
            tts_config["edge_tts_executable"] = args.edge_tts_executable

        tts_setup, tts_setup_ms = send_json_timed(sock_file, {
            "request_id": request_id("setup-tts", 3),
            "work_id": "tts",
            "action": "setup",
            "object": "tts.setup",
            "data": tts_config,
        })
        ensure_ok("tts setup", tts_setup)
        if args.require_real_tts and tts_setup.get("data", {}).get("backend") != "summertts-subprocess":
            raise RuntimeError(f"tts setup did not use SummerTTS backend: {tts_setup}")

        rag_work_id = rag_setup["work_id"]
        llm_work_id = llm_setup["work_id"]
        tts_work_id = tts_setup["work_id"]

        setup_ms = (time.perf_counter() - setup_start) * 1000.0
        demo_start = time.perf_counter()

        rag_response, rag_first_client_ms = send_json_timed(sock_file, {
            "request_id": request_id("rag", 4),
            "work_id": rag_work_id,
            "action": "inference",
            "object": "rag.request",
            "data": {"query": args.query},
        })
        ensure_ok("rag inference", rag_response)
        rag_data = rag_response.get("data", {})

        pipeline_start = time.perf_counter()
        rag_cached_response, rag_cache_client_ms = send_json_timed(sock_file, {
            "request_id": request_id("rag-cache", 5),
            "work_id": rag_work_id,
            "action": "inference",
            "object": "rag.request",
            "data": {"query": args.query},
        })
        ensure_ok("rag cache inference", rag_cached_response)
        rag_cached_data = rag_cached_response.get("data", {})

        llm_response = send_json_stream(sock_file, {
            "request_id": request_id("llm", 6),
            "work_id": llm_work_id,
            "action": "inference",
            "object": "llm.request",
            "data": {
                "query": args.query,
                "context": rag_cached_data.get("context") or rag_data.get("context", ""),
                "prompt": rag_cached_data.get("prompt") or rag_data.get("prompt", args.query),
            },
        }, "llm.response")
        ensure_ok("llm inference", llm_response)
        llm_data = llm_response.get("data", {})
        llm_text = llm_data.get("text") or llm_data.get("stream_text", "")

        tts_response, tts_client_ms = send_json_timed(sock_file, {
            "request_id": request_id("tts", 7),
            "work_id": tts_work_id,
            "action": "inference",
            "object": "tts.request",
            "data": {"text": llm_text},
        })
        ensure_ok("tts inference", tts_response)
        tts_data = tts_response.get("data", {})
        tts_artifact = tts_data.get("artifact", "")
        if args.require_real_tts:
            if tts_data.get("mime_type") != "audio/wav":
                raise RuntimeError(f"tts did not return wav output: {tts_response}")
            artifact_path = Path(tts_artifact)
            if not artifact_path.exists() or artifact_path.stat().st_size <= 44:
                raise RuntimeError(f"tts wav artifact is missing or invalid: {tts_artifact}")
        end_to_end_ms = (time.perf_counter() - pipeline_start) * 1000.0
        demo_total_ms = (time.perf_counter() - demo_start) * 1000.0

    rag_metrics = rag_response.get("data", {}).get("metrics", {})
    rag_cache_metrics = rag_cached_response.get("data", {}).get("metrics", {})
    llm_metrics = llm_response.get("data", {}).get("metrics", {})
    performance = {
        "setup_client_ms": metric_number(setup_ms),
        "setup_breakdown_client_ms": {
            "rag": metric_number(rag_setup_ms),
            "llm": metric_number(llm_setup_ms),
            "tts": metric_number(tts_setup_ms),
        },
        "rag_first": {
            "client_ms": metric_number(rag_first_client_ms),
            "service_total_ms": metric_number(rag_metrics.get("total_ms")),
            "embed_ms": metric_number(rag_metrics.get("embed_ms")),
            "fts_ms": metric_number(rag_metrics.get("fts_ms")),
            "vector_ms": metric_number(rag_metrics.get("vector_ms")),
            "rerank_ms": metric_number(rag_metrics.get("rerank_ms")),
            "cache_hit": rag_metrics.get("cache_hit"),
        },
        "rag_cache": {
            "client_ms": metric_number(rag_cache_client_ms),
            "service_total_ms": metric_number(rag_cache_metrics.get("total_ms")),
            "cache_hit": rag_cache_metrics.get("cache_hit"),
        },
        "llm": {
            "client_first_delta_ms": metric_number(llm_data.get("client_first_delta_ms")),
            "client_total_ms": metric_number(llm_data.get("client_total_ms")),
            "service_first_token_ms": metric_number(llm_metrics.get("first_token_ms")),
            "service_total_ms": metric_number(llm_metrics.get("total_ms")),
            "delta_count": llm_metrics.get("delta_count", llm_data.get("stream_delta_count")),
            "stream_delta_count": llm_data.get("stream_delta_count"),
            "backend": llm_data.get("backend"),
        },
        "tts_client_ms": metric_number(tts_client_ms),
        "end_to_end_client_ms": metric_number(end_to_end_ms),
        "demo_total_client_ms": metric_number(demo_total_ms),
    }

    result = {
        "work_ids": {
            "rag": rag_work_id,
            "llm": llm_work_id,
            "tts": tts_work_id,
        },
        "performance": performance,
        "rag": rag_cached_response,
        "rag_first": rag_response,
        "llm": llm_response,
        "tts": tts_response,
    }
    print(
        "[phase1-demo] performance "
        f"rag_first={performance['rag_first']['client_ms']}ms "
        f"rag_cache={performance['rag_cache']['client_ms']}ms "
        f"llm_first_token={performance['llm']['service_first_token_ms']}ms "
        f"llm_total={performance['llm']['service_total_ms']}ms "
        f"end_to_end={performance['end_to_end_client_ms']}ms",
        file=sys.stderr,
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"phase1_query failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
