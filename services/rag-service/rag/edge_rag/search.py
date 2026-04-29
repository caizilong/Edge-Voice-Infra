import sqlite3
import time
from pathlib import Path
from typing import Dict, Iterable, List

import numpy as np

from .embeddings import encode_texts, normalize, unpack_f32
from .store import connect, parse_metadata
from .text import query_terms


def now_ms() -> float:
    return time.perf_counter() * 1000.0


def fts_query(text: str) -> str:
    tokens = query_terms(text)
    if not tokens:
        return '"' + text.replace('"', '""') + '"'
    return " OR ".join('"' + token.replace('"', '""') + '"' for token in tokens[:8])


def rank_map(ids: Iterable[int]) -> Dict[int, int]:
    return {chunk_id: index + 1 for index, chunk_id in enumerate(ids)}


def rrf_score(rank: int, k: int = 60) -> float:
    return 1.0 / (k + rank)


class HybridRagIndex:
    def __init__(self, db_path: str, model_path: str):
        self.db_path = Path(db_path)
        self.model_path = model_path
        self.con = connect(self.db_path)
        self._embeddings = None
        self._rows = None

    def close(self):
        self.con.close()

    def _load_vectors(self):
        if self._embeddings is not None:
            return
        rows = self.con.execute(
            """SELECT chunk_id, doc_id, chunk_text, section, subsection, page,
                      chunk_index, metadata_json, embedding_f32, embedding_norm
               FROM chunks"""
        ).fetchall()
        vectors = []
        normalized_rows = []
        for row in rows:
            vec = normalize(unpack_f32(row["embedding_f32"]))
            vectors.append(vec)
            normalized_rows.append(row)
        self._rows = normalized_rows
        self._embeddings = np.vstack(vectors) if vectors else np.zeros((0, 0), dtype="<f4")

    def _fts_search(self, query: str, limit: int):
        try:
            rows = self.con.execute(
                """SELECT rowid AS chunk_id, bm25(chunks_fts) AS bm25_score
                   FROM chunks_fts
                   WHERE chunks_fts MATCH ?
                   ORDER BY bm25_score
                   LIMIT ?""",
                (fts_query(query), limit),
            ).fetchall()
        except sqlite3.Error:
            rows = []
        if rows:
            return [{"chunk_id": int(row["chunk_id"]), "bm25_score": float(row["bm25_score"])}
                    for row in rows]

        like = f"%{query}%"
        rows = self.con.execute(
            """SELECT chunk_id, -1.0 AS bm25_score
               FROM chunks
               WHERE chunk_text LIKE ? OR section LIKE ? OR subsection LIKE ?
               LIMIT ?""",
            (like, like, like, limit),
        ).fetchall()
        return [{"chunk_id": int(row["chunk_id"]), "bm25_score": float(row["bm25_score"])}
                for row in rows]

    def _vector_search(self, query: str, limit: int):
        self._load_vectors()
        if self._embeddings.size == 0:
            return [], 0.0, 0.0
        embed_start = now_ms()
        query_vec = normalize(encode_texts(self.model_path, [query])[0])
        embed_ms = now_ms() - embed_start
        score_start = now_ms()
        scores = self._embeddings @ query_vec
        top_indices = np.argsort(scores)[::-1][:limit]
        score_ms = now_ms() - score_start
        return [
            {
                "chunk_id": int(self._rows[index]["chunk_id"]),
                "vector_score": float(scores[index]),
            }
            for index in top_indices
        ], embed_ms, score_ms

    def _chunk_by_ids(self, chunk_ids: List[int]):
        if not chunk_ids:
            return {}
        placeholders = ",".join("?" for _ in chunk_ids)
        rows = self.con.execute(
            f"""SELECT chunk_id, doc_id, chunk_text, section, subsection, page,
                       chunk_index, metadata_json
                FROM chunks
                WHERE chunk_id IN ({placeholders})""",
            chunk_ids,
        ).fetchall()
        return {int(row["chunk_id"]): row for row in rows}

    def search(self, query: str, top_k: int, candidate_k: int, threshold: float, context_chars: int):
        total_start = now_ms()

        fts_start = now_ms()
        fts_hits = self._fts_search(query, candidate_k)
        fts_ms = now_ms() - fts_start

        vector_hits, embed_ms, score_ms = self._vector_search(query, candidate_k)

        rerank_start = now_ms()
        fts_ranks = rank_map(hit["chunk_id"] for hit in fts_hits)
        vector_ranks = rank_map(hit["chunk_id"] for hit in vector_hits)
        bm25_scores = {hit["chunk_id"]: hit["bm25_score"] for hit in fts_hits}
        vector_scores = {hit["chunk_id"]: hit["vector_score"] for hit in vector_hits}
        all_ids = sorted(set(fts_ranks) | set(vector_ranks))
        rows = self._chunk_by_ids(all_ids)

        fused = []
        for chunk_id in all_ids:
            score = 0.0
            if chunk_id in fts_ranks:
                score += rrf_score(fts_ranks[chunk_id])
            if chunk_id in vector_ranks:
                score += rrf_score(vector_ranks[chunk_id])
            vector_score = vector_scores.get(chunk_id, 0.0)
            if vector_score < threshold and chunk_id not in fts_ranks:
                continue
            row = rows.get(chunk_id)
            if row is None:
                continue
            fused.append({
                "chunk_id": chunk_id,
                "doc_id": int(row["doc_id"]),
                "text": row["chunk_text"],
                "source": self._source_for_doc(int(row["doc_id"])),
                "section": row["section"],
                "subsection": row["subsection"],
                "page": row["page"],
                "chunk_index": int(row["chunk_index"]),
                "bm25_score": bm25_scores.get(chunk_id, 0.0),
                "vector_score": vector_score,
                "rrf_score": score,
                "metadata": parse_metadata(row["metadata_json"]),
            })

        fused.sort(key=lambda item: (item["rrf_score"], item["vector_score"]), reverse=True)
        results = fused[:top_k]
        context_parts = []
        used_chars = 0
        for item in results:
            text = item["text"]
            if used_chars + len(text) > context_chars:
                text = text[:max(0, context_chars - used_chars)]
            if text:
                context_parts.append(text)
                used_chars += len(text)
            if used_chars >= context_chars:
                break
        rerank_ms = now_ms() - rerank_start

        context = "\n".join(context_parts)
        return {
            "query": query,
            "context": context,
            "prompt": "Question: " + query + "\nContext: " + context,
            "results": results,
            "backend": "sqlite-fts5-python-vector",
            "metrics": {
                "embed_ms": round(embed_ms, 3),
                "fts_ms": round(fts_ms, 3),
                "vector_ms": round(score_ms, 3),
                "rerank_ms": round(rerank_ms, 3),
                "total_ms": round(now_ms() - total_start, 3),
                "cache_hit": False,
                "candidate_count": len(all_ids),
            },
            "model": self.model_path,
            "knowledge_db": str(self.db_path),
        }

    def _source_for_doc(self, doc_id: int) -> str:
        row = self.con.execute("SELECT source FROM documents WHERE doc_id = ?", (doc_id,)).fetchone()
        return row["source"] if row else ""
