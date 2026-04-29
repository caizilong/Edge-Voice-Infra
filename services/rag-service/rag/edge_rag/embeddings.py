from functools import lru_cache
from pathlib import Path
from typing import Iterable

import numpy as np


@lru_cache(maxsize=2)
def load_model(model_path: str):
    from sentence_transformers import SentenceTransformer

    return SentenceTransformer(str(Path(model_path)))


def encode_texts(model_path: str, texts: Iterable[str]) -> np.ndarray:
    model = load_model(model_path)
    embeddings = model.encode(list(texts), show_progress_bar=False)
    return np.asarray(embeddings, dtype="<f4")


def normalize(vec: np.ndarray) -> np.ndarray:
    arr = np.asarray(vec, dtype="<f4")
    norm = float(np.linalg.norm(arr))
    if norm <= 0.0:
        return arr
    return arr / norm


def quantize_i8(vec: np.ndarray) -> np.ndarray:
    normalized = np.clip(normalize(vec), -1.0, 1.0)
    return np.rint(normalized * 127.0).astype(np.int8)


def vector_norm(vec: np.ndarray) -> float:
    return float(np.linalg.norm(np.asarray(vec, dtype="<f4")))


def pack_f32(vec: np.ndarray) -> bytes:
    return np.asarray(vec, dtype="<f4").tobytes()


def pack_i8(vec: np.ndarray) -> bytes:
    return np.asarray(vec, dtype=np.int8).tobytes()


def unpack_f32(raw: bytes) -> np.ndarray:
    return np.frombuffer(raw, dtype="<f4")

