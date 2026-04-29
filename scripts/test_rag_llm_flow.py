#!/usr/bin/env python3
import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import time
import tempfile
from pathlib import Path


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


def send_json(sock_file, payload, label):
    print(f"[phase1-test] -> {label}", flush=True)
    raw = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    sock_file.write(raw.encode("utf-8") + b"\n")
    sock_file.flush()
    response = recv_json(sock_file)
    print(f"[phase1-test] <- {label}: object={response.get('object')} error={response_error(response)}", flush=True)
    return response


def request_id(prefix):
    return f"{prefix}-{int(time.time() * 1000)}"


def wait_for_tcp(host, port, timeout):
    deadline = time.time() + timeout
    last_error = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection((host, port), timeout=1.0)
            sock.close()
            return
        except OSError as exc:
            last_error = exc
            time.sleep(0.2)
    raise RuntimeError(f"gateway did not listen on {host}:{port}: {last_error}")


def wait_for_path(path, timeout, label):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if Path(path).exists():
            return
        time.sleep(0.1)
    raise RuntimeError(f"{label} did not create {path}")


def start_process(name, path, cwd, env, log_dir):
    if not path.exists():
        raise FileNotFoundError(f"missing executable: {path}")
    log_path = log_dir / f"{name}.log"
    log_file = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen(
        [str(path)],
        cwd=str(cwd),
        env=env,
        stdout=log_file,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    print(f"[phase1-test] started {name}: pid={proc.pid}, log={log_path}", flush=True)
    return {"name": name, "proc": proc, "log_path": log_path, "log_file": log_file}


def stop_processes(processes):
    for item in reversed(processes):
        proc = item["proc"]
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    deadline = time.time() + 5
    for item in reversed(processes):
        proc = item["proc"]
        while proc.poll() is None and time.time() < deadline:
            time.sleep(0.1)
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        item["log_file"].close()


def dump_process_logs(processes):
    for item in processes:
        name = item["name"]
        log_path = item["log_path"]
        try:
            output = log_path.read_text(encoding="utf-8", errors="replace")
        except Exception:
            output = ""
        if output:
            print(f"\n===== {name} output =====", file=sys.stderr)
            print(output[-4000:], file=sys.stderr)
        else:
            print(f"\n===== {name} output =====\n(no output, log={log_path})", file=sys.stderr)


def main():
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Run RAG -> LLM IPC flow through gateway")
    parser.add_argument("--build-dir", default=str(repo / "build-phase1"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10001)
    parser.add_argument("--query", default="发动机故障怎么办")
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--no-start", action="store_true", help="Use already running services")
    parser.add_argument("--require-real-llm", action="store_true")
    parser.add_argument("--log-dir", default="")
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    log_dir = Path(args.log_dir) if args.log_dir else Path(tempfile.mkdtemp(prefix="edge_voice_phase1_"))
    log_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.setdefault("EDGE_VOICE_CONFIG", str(repo / "config" / "master_config.json"))
    env.setdefault("PYTHONPATH", str(repo / "services" / "rag-service" / "rag"))

    named_processes = []
    try:
        if not args.no_start:
            executables = [
                ("gateway", build_dir / "common" / "gateway" / "unit_manager"),
                ("rag", build_dir / "services" / "rag-service" / "rag_ipc_service"),
                ("llm", build_dir / "services" / "llm-service" / "llm_ipc_service"),
            ]
            for name, path in executables:
                named_processes.append(start_process(name, path, repo, env, log_dir))
                time.sleep(0.2)
                if named_processes[-1]["proc"].poll() is not None:
                    dump_process_logs(named_processes)
                    raise RuntimeError(f"{name} exited early with code {named_processes[-1]['proc'].returncode}")
            print("[phase1-test] waiting for gateway TCP listener", flush=True)
            wait_for_tcp(args.host, args.port, args.timeout)
            print("[phase1-test] waiting for RAG/LLM RPC sockets", flush=True)
            wait_for_path("/tmp/rpc.rag", args.timeout, "rag_ipc_service")
            wait_for_path("/tmp/rpc.llm", args.timeout, "llm_ipc_service")

        with socket.create_connection((args.host, args.port), timeout=args.timeout) as sock:
            sock.settimeout(args.timeout)
            sock_file = sock.makefile("rwb", buffering=0)

            rag_setup = send_json(sock_file, {
                "request_id": request_id("setup-rag"),
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
                    "context_chars": 1600,
                },
            }, "rag setup")
            if response_error(rag_setup).get("code") != 0:
                raise AssertionError(f"RAG setup failed: {rag_setup}")

            llm_setup = send_json(sock_file, {
                "request_id": request_id("setup-llm"),
                "work_id": "llm",
                "action": "setup",
                "object": "llm.setup",
                "data": {
                    "model": str(repo / "models" / "llm" / "Qwen3-1.7B.rkllm"),
                    "max_new_tokens": 256,
                    "max_context_len": 4096,
                    "require_real_backend": args.require_real_llm,
                },
            }, "llm setup")
            if response_error(llm_setup).get("code") != 0:
                raise AssertionError(f"LLM setup failed: {llm_setup}")
            if args.require_real_llm and llm_setup.get("data", {}).get("backend") != "rkllm":
                raise AssertionError(f"LLM is not using rkllm backend: {llm_setup}")

            rag_work_id = rag_setup["work_id"]
            llm_work_id = llm_setup["work_id"]

            rag_response = send_json(sock_file, {
                "request_id": request_id("rag"),
                "work_id": rag_work_id,
                "action": "inference",
                "object": "rag.request",
                "data": {"query": args.query},
            }, "rag inference")
            if response_error(rag_response).get("code") != 0:
                raise AssertionError(f"RAG inference failed: {rag_response}")
            rag_data = rag_response.get("data", {})
            if not rag_data.get("context"):
                raise AssertionError(f"RAG returned empty context: {rag_response}")

            rag_cached_response = send_json(sock_file, {
                "request_id": request_id("rag-cache"),
                "work_id": rag_work_id,
                "action": "inference",
                "object": "rag.request",
                "data": {"query": args.query},
            }, "rag inference cache")
            if response_error(rag_cached_response).get("code") != 0:
                raise AssertionError(f"RAG cached inference failed: {rag_cached_response}")
            cache_hit = rag_cached_response.get("data", {}).get("metrics", {}).get("cache_hit")
            if cache_hit is not True:
                raise AssertionError(f"RAG cache did not hit on repeated query: {rag_cached_response}")
            rag_response = rag_cached_response
            rag_data = rag_response.get("data", {})

            llm_response = send_json(sock_file, {
                "request_id": request_id("llm"),
                "work_id": llm_work_id,
                "action": "inference",
                "object": "llm.request",
                "data": {
                    "query": args.query,
                    "context": rag_data["context"],
                    "prompt": rag_data.get("prompt", args.query),
                },
            }, "llm inference")
            if response_error(llm_response).get("code") != 0:
                raise AssertionError(f"LLM inference failed: {llm_response}")
            llm_data = llm_response.get("data", {})
            if not llm_data.get("text"):
                raise AssertionError(f"LLM returned empty text: {llm_response}")

        print(json.dumps({
            "ok": True,
            "rag_work_id": rag_work_id,
            "llm_work_id": llm_work_id,
            "rag": rag_response,
            "llm": llm_response,
        }, ensure_ascii=False, indent=2))
        return 0
    except Exception:
        dump_process_logs(named_processes)
        raise
    finally:
        if not args.no_start:
            stop_processes(named_processes)


if __name__ == "__main__":
    raise SystemExit(main())
