#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import queue
import shutil
import socket
import subprocess
import sys
import threading
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


class TtsSegmenter:
    def __init__(self, min_soft_chars=24, max_chars=90):
        self.buffer = ""
        self.min_soft_chars = min_soft_chars
        self.max_chars = max_chars
        self.strong_delimiters = set("。！？；：\n")
        self.soft_delimiters = set("，、")

    def feed(self, text):
        self.buffer += text
        return self._pop_ready(final=False)

    def flush(self):
        return self._pop_ready(final=True)

    def _pop_ready(self, final):
        segments = []
        while self.buffer:
            split_at = None
            for idx, ch in enumerate(self.buffer):
                pos = idx + 1
                if ch in self.strong_delimiters and pos >= 2:
                    split_at = pos
                    break
                if ch in self.soft_delimiters and pos >= self.min_soft_chars:
                    split_at = pos
                    break
            if split_at is None and len(self.buffer) >= self.max_chars:
                split_at = self._max_split_position()
            if split_at is None:
                if final:
                    split_at = len(self.buffer)
                else:
                    break
            segment = normalize_tts_segment(self.buffer[:split_at])
            self.buffer = self.buffer[split_at:]
            if segment:
                segments.append(segment)
        return segments

    def _max_split_position(self):
        window = self.buffer[: self.max_chars]
        for delimiter in ("，", "、", ",", " "):
            pos = window.rfind(delimiter)
            if pos >= self.min_soft_chars:
                return pos + 1
        return self.max_chars


def normalize_tts_segment(text):
    text = text.strip()
    text = text.replace("**", "").replace("__", "")
    text = text.lstrip("#>- \t")
    return text.strip()


def split_text_for_tts(text):
    segmenter = TtsSegmenter()
    segmenter.feed(text)
    return segmenter.flush()


def play_wav_artifact(segment, args, stream_start):
    artifact = segment.get("artifact", "")
    play_start_ms = None
    play_ms = 0.0
    delete_after_play = args.stream_tts_playback and not args.keep_tts_artifacts
    if args.stream_tts_playback and not args.no_tts_playback:
        if not shutil.which(args.audio_player):
            raise RuntimeError(f"audio player not found: {args.audio_player}")
        play_start = time.perf_counter()
        play_start_ms = (play_start - stream_start) * 1000.0
        cmd = [args.audio_player]
        if Path(args.audio_player).name == "aplay":
            cmd.append("-q")
        cmd.append(artifact)
        completed = subprocess.run(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            timeout=args.tts_play_timeout_sec,
            check=False,
        )
        play_ms = (time.perf_counter() - play_start) * 1000.0
        if completed.returncode != 0:
            stderr = completed.stderr.decode("utf-8", errors="replace").strip()
            raise RuntimeError(f"audio playback failed for {artifact}: {stderr}")
    if delete_after_play and artifact:
        try:
            Path(artifact).unlink(missing_ok=True)
            segment["deleted_after_play"] = True
        except OSError as exc:
            segment["delete_error"] = str(exc)
    segment["play_start_ms"] = metric_number(play_start_ms)
    segment["play_ms"] = metric_number(play_ms)


def send_json_stream_with_tts(sock_file, payload, final_object, tts_work_id, args):
    stream_start = time.perf_counter()
    send_payload(sock_file, payload)

    segmenter = TtsSegmenter()
    deltas = []
    first_delta_ms = None
    final_response = None
    pending = {}
    ready_segments = {}
    tts_segments = []
    tts_queue = queue.Queue()
    playback_errors = []
    first_segment_sent_ms = None
    first_audio_ready_ms = None
    next_play_index = 0

    def playback_worker():
        while True:
            item = tts_queue.get()
            if item is None:
                return
            try:
                play_wav_artifact(item, args, stream_start)
            except Exception as exc:  # noqa: BLE001 - propagate after reader loop finishes.
                item["play_error"] = str(exc)
                playback_errors.append(str(exc))

    player_thread = threading.Thread(target=playback_worker, daemon=True)
    player_thread.start()

    def send_segment(text):
        nonlocal first_segment_sent_ms
        index = len(tts_segments)
        req_id = request_id("tts-seg", 100 + index)
        segment = {
            "index": index,
            "request_id": req_id,
            "text": text,
            "sent_at": time.perf_counter(),
        }
        if first_segment_sent_ms is None:
            first_segment_sent_ms = (segment["sent_at"] - stream_start) * 1000.0
        tts_segments.append(segment)
        pending[req_id] = segment
        send_payload(sock_file, {
            "request_id": req_id,
            "work_id": tts_work_id,
            "action": "inference",
            "object": "tts.request",
            "data": {"text": text, "segment_index": index},
        })

    def handle_tts_response(response):
        nonlocal first_audio_ready_ms, next_play_index
        req_id = response.get("request_id")
        segment = pending.pop(req_id, None)
        if segment is None:
            return
        segment["response"] = response
        segment["tts_client_ms"] = metric_number((time.perf_counter() - segment["sent_at"]) * 1000.0)
        error = response_error(response)
        if error.get("code") != 0:
            segment["error"] = error
            raise RuntimeError(f"tts segment {segment['index']} failed: {error}")
        data = response.get("data", {})
        segment["artifact"] = data.get("artifact", "")
        segment["mime_type"] = data.get("mime_type")
        segment["backend"] = data.get("backend")
        segment["service_total_ms"] = metric_number(data.get("metrics", {}).get("total_ms"))
        segment["worker_ms"] = metric_number(data.get("metrics", {}).get("worker_ms"))
        if args.require_real_tts:
            artifact_path = Path(segment["artifact"])
            if data.get("mime_type") != "audio/wav":
                raise RuntimeError(f"tts segment did not return wav output: {response}")
            if not artifact_path.exists() or artifact_path.stat().st_size <= 44:
                raise RuntimeError(f"tts segment wav is missing or invalid: {segment['artifact']}")
        if first_audio_ready_ms is None:
            first_audio_ready_ms = (time.perf_counter() - stream_start) * 1000.0
        ready_segments[segment["index"]] = segment
        while next_play_index in ready_segments:
            tts_queue.put(ready_segments.pop(next_play_index))
            next_play_index += 1

    try:
        while True:
            response = recv_json(sock_file)
            obj = response.get("object")
            error = response_error(response)
            if obj == "llm.delta":
                if first_delta_ms is None:
                    first_delta_ms = (time.perf_counter() - stream_start) * 1000.0
                delta = response.get("data", {}).get("delta", "")
                deltas.append(delta)
                for segment_text in segmenter.feed(delta):
                    send_segment(segment_text)
                continue
            if obj in {"tts.response", "tts.error"}:
                handle_tts_response(response)
                continue

            response.setdefault("data", {})
            if isinstance(response["data"], dict):
                response["data"].setdefault("stream_text", "".join(deltas))
                response["data"].setdefault("stream_delta_count", len(deltas))
                response["data"].setdefault("client_first_delta_ms", first_delta_ms)
                response["data"].setdefault(
                    "client_total_ms", (time.perf_counter() - stream_start) * 1000.0
                )
            if obj == final_object or error.get("code") != 0:
                final_response = response
                break

        if final_response is None:
            raise RuntimeError("missing llm response")
        if response_error(final_response).get("code") == 0:
            if deltas:
                for segment_text in segmenter.flush():
                    send_segment(segment_text)
            else:
                llm_data = final_response.get("data", {})
                for segment_text in split_text_for_tts(llm_data.get("text", "")):
                    send_segment(segment_text)

        while pending:
            response = recv_json(sock_file)
            if response.get("object") in {"tts.response", "tts.error"}:
                handle_tts_response(response)
    finally:
        tts_queue.put(None)
        player_thread.join()

    if playback_errors:
        raise RuntimeError(playback_errors[0])

    stream_total_ms = (time.perf_counter() - stream_start) * 1000.0
    tts_public_segments = []
    for segment in tts_segments:
        public = {
            "index": segment["index"],
            "text": segment["text"],
            "artifact": segment.get("artifact", ""),
            "mime_type": segment.get("mime_type"),
            "backend": segment.get("backend"),
            "tts_client_ms": segment.get("tts_client_ms"),
            "play_start_ms": segment.get("play_start_ms"),
            "play_ms": segment.get("play_ms"),
            "service_total_ms": segment.get("service_total_ms"),
            "worker_ms": segment.get("worker_ms"),
        }
        if segment.get("deleted_after_play"):
            public["deleted_after_play"] = True
        if segment.get("play_error"):
            public["play_error"] = segment["play_error"]
        tts_public_segments.append(public)

    metrics = {
        "first_segment_sent_ms": metric_number(first_segment_sent_ms),
        "first_audio_ready_ms": metric_number(first_audio_ready_ms),
        "segment_count": len(tts_segments),
        "stream_total_ms": metric_number(stream_total_ms),
        "playback_enabled": args.stream_tts_playback and not args.no_tts_playback,
    }
    return final_response, tts_public_segments, metrics


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
    parser.add_argument("--tts-play-timeout-sec", type=int, default=120)
    parser.add_argument("--tts-model", default="")
    parser.add_argument("--edge-tts-executable", default="")
    parser.add_argument("--edge-tts-worker-executable", default="")
    parser.add_argument("--stream-tts-playback", action="store_true")
    parser.add_argument("--no-tts-playback", action="store_true")
    parser.add_argument("--keep-tts-artifacts", action="store_true")
    parser.add_argument("--audio-player", default="aplay")
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
            "output_dir": "/tmp/edge_voice_tts/segments" if args.stream_tts_playback else "/tmp/edge_voice_tts",
            "speaker_id": args.tts_speaker_id,
            "length_scale": args.tts_length_scale,
            "timeout_sec": args.tts_timeout_sec,
            "require_real_backend": args.require_real_tts,
        }
        if args.stream_tts_playback:
            tts_config["backend"] = "summertts-worker"
            tts_config["stream_segments"] = True
            tts_config["delete_artifact_after_play"] = not args.keep_tts_artifacts
        if args.tts_model:
            tts_config["model"] = args.tts_model
        if args.edge_tts_executable:
            tts_config["edge_tts_executable"] = args.edge_tts_executable
        if args.edge_tts_worker_executable:
            tts_config["edge_tts_worker_executable"] = args.edge_tts_worker_executable

        tts_setup, tts_setup_ms = send_json_timed(sock_file, {
            "request_id": request_id("setup-tts", 3),
            "work_id": "tts",
            "action": "setup",
            "object": "tts.setup",
            "data": tts_config,
        })
        ensure_ok("tts setup", tts_setup)
        real_tts_backends = {"summertts-subprocess", "summertts-worker"}
        if args.require_real_tts and tts_setup.get("data", {}).get("backend") not in real_tts_backends:
            raise RuntimeError(f"tts setup did not use SummerTTS backend: {tts_setup}")
        if args.stream_tts_playback and tts_setup.get("data", {}).get("backend") != "summertts-worker":
            raise RuntimeError(f"streaming tts setup did not use worker backend: {tts_setup}")

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

        llm_payload = {
            "request_id": request_id("llm", 6),
            "work_id": llm_work_id,
            "action": "inference",
            "object": "llm.request",
            "data": {
                "query": args.query,
                "context": rag_cached_data.get("context") or rag_data.get("context", ""),
                "prompt": rag_cached_data.get("prompt") or rag_data.get("prompt", args.query),
            },
        }
        tts_stream_segments = []
        tts_stream_metrics = {}
        if args.stream_tts_playback:
            llm_response, tts_stream_segments, tts_stream_metrics = send_json_stream_with_tts(
                sock_file, llm_payload, "llm.response", tts_work_id, args
            )
        else:
            llm_response = send_json_stream(sock_file, llm_payload, "llm.response")
        ensure_ok("llm inference", llm_response)
        llm_data = llm_response.get("data", {})
        llm_text = llm_data.get("text") or llm_data.get("stream_text", "")

        tts_response = None
        tts_client_ms = None
        if not args.stream_tts_playback:
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
    if args.stream_tts_playback:
        performance["tts_stream"] = {
            "first_segment_sent_ms": tts_stream_metrics.get("first_segment_sent_ms"),
            "first_audio_ready_ms": tts_stream_metrics.get("first_audio_ready_ms"),
            "first_play_start_ms": next(
                (segment.get("play_start_ms") for segment in tts_stream_segments
                 if segment.get("play_start_ms") is not None),
                None,
            ),
            "segment_count": tts_stream_metrics.get("segment_count"),
            "stream_total_ms": tts_stream_metrics.get("stream_total_ms"),
            "playback_enabled": tts_stream_metrics.get("playback_enabled"),
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
    if args.stream_tts_playback:
        result["tts_stream"] = tts_stream_segments
    tts_perf = (
        f"tts_first_audio={performance['tts_stream']['first_audio_ready_ms']}ms "
        f"tts_segments={performance['tts_stream']['segment_count']} "
        if args.stream_tts_playback else
        f"tts_total={performance['tts_client_ms']}ms "
    )
    print(
        "[phase1-demo] performance "
        f"rag_first={performance['rag_first']['client_ms']}ms "
        f"rag_cache={performance['rag_cache']['client_ms']}ms "
        f"llm_first_token={performance['llm']['service_first_token_ms']}ms "
        f"llm_total={performance['llm']['service_total_ms']}ms "
        f"{tts_perf}"
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
