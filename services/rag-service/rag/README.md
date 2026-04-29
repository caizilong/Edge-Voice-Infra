# Edge RAG

SQLite-backed hybrid RAG for the RK3588 phase1 services.

## Build the knowledge database

Run this inside the `rag38` environment:

```bash
python3 services/rag-service/rag/build_index.py \
  --input services/rag-service/data/sources \
  --db services/rag-service/data/knowledge/vehicle_knowledge.sqlite \
  --model models/rag/text2vec-base-chinese
```

The generated database stores document metadata, chunks, FTS5 rows, float32
embeddings, and int8 quantized embeddings in one SQLite file.

## Query

```bash
tmp="$(mktemp)"
printf '{"query":"发动机故障怎么办"}' > "$tmp"
python3 services/rag-service/rag/query.py \
  --input "$tmp" \
  --db services/rag-service/data/knowledge/vehicle_knowledge.sqlite \
  --model models/rag/text2vec-base-chinese \
  --top-k 3 \
  --candidate-k 30
rm -f "$tmp"
```

