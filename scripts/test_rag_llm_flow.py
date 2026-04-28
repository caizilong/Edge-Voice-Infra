#!/usr/bin/env python3
import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path


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


def start_process(path, cwd, env):
    if not path.exists():
        raise FileNotFoundError(f"missing executable: {path}")
    return subprocess.Popen(
        [str(path)],
        cwd=str(cwd),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )


def stop_processes(processes):
    for proc in reversed(processes):
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    deadline = time.time() + 5
    for proc in reversed(processes):
        while proc.poll() is None and time.time() < deadline:
            time.sleep(0.1)
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def dump_process_logs(processes):
    for name, proc in processes:
        if proc.stdout is None:
            continue
        try:
            output = proc.stdout.read()
        except Exception:
            output = ""
        if output:
            print(f"\n===== {name} output =====", file=sys.stderr)
            print(output[-4000:], file=sys.stderr)


def main():
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Run RAG -> LLM IPC flow through gateway")
    parser.add_argument("--build-dir", default=str(repo / "build-phase1"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10001)
    parser.add_argument("--query", default="发动机故障怎么办")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--no-start", action="store_true", help="Use already running services")
    parser.add_argument("--require-real-llm", action="store_true")
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    env = os.environ.copy()
    env.setdefault("EDGE_VOICE_CONFIG", str(repo / "config" / "master_config.json"))
    env.setdefault("PYTHONPATH", str(repo / "services" / "rag-service" / "python"))

    named_processes = []
    try:
        if not args.no_start:
            executables = [
                ("gateway", build_dir / "common" / "gateway" / "unit_manager"),
                ("rag", build_dir / "services" / "rag-service" / "rag_ipc_service"),
                ("llm", build_dir / "services" / "llm-service" / "llm_ipc_service"),
            ]
            for name, path in executables:
                named_processes.append((name, start_process(path, repo, env)))
            wait_for_tcp(args.host, args.port, args.timeout)

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
                    "vector_db": str(repo / "services" / "rag-service" / "python" / "vector_db"),
                    "script": str(repo / "services" / "rag-service" / "python" / "rag_query.py"),
                    "top_k": 1,
                    "threshold": 0.5,
                },
            })
            if rag_setup.get("error", {}).get("code") != 0:
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
            })
            if llm_setup.get("error", {}).get("code") != 0:
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
            })
            if rag_response.get("error", {}).get("code") != 0:
                raise AssertionError(f"RAG inference failed: {rag_response}")
            rag_data = rag_response.get("data", {})
            if not rag_data.get("context"):
                raise AssertionError(f"RAG returned empty context: {rag_response}")

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
            })
            if llm_response.get("error", {}).get("code") != 0:
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
            stop_processes([proc for _, proc in named_processes])


if __name__ == "__main__":
    raise SystemExit(main())
