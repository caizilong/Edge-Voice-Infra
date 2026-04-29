from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional


@dataclass
class Chunk:
    source: str
    title: str
    text: str
    section: str
    subsection: str
    page: Optional[int]
    chunk_index: int
    metadata: dict


def iter_source_files(input_path: Path) -> Iterable[Path]:
    if input_path.is_file():
        if input_path.suffix.lower() in {".txt", ".md"}:
            yield input_path
        return
    for path in sorted(input_path.rglob("*")):
        if path.is_file() and path.suffix.lower() in {".txt", ".md"}:
            yield path


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def split_sections(text: str):
    title = ""
    section = ""
    subsection = ""
    buffer: List[str] = []

    def flush():
        nonlocal buffer
        body = "\n".join(line for line in buffer if line.strip()).strip()
        buffer = []
        if body:
            return {
                "title": title,
                "section": section,
                "subsection": subsection,
                "content": body,
            }
        return None

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line:
            buffer.append("")
            continue
        if line.startswith("# "):
            item = flush()
            if item:
                yield item
            title = line[2:].strip()
            section = ""
            subsection = ""
        elif line.startswith("## "):
            item = flush()
            if item:
                yield item
            section = line[3:].strip()
            subsection = ""
        elif line.startswith("### "):
            item = flush()
            if item:
                yield item
            subsection = line[4:].strip()
        else:
            buffer.append(line)
    item = flush()
    if item:
        yield item


def chunk_text(text: str, chunk_size: int, overlap: int) -> Iterable[str]:
    cleaned = " ".join(part.strip() for part in text.splitlines() if part.strip())
    if not cleaned:
        return
    if len(cleaned) <= chunk_size:
        yield cleaned
        return
    start = 0
    while start < len(cleaned):
        end = min(start + chunk_size, len(cleaned))
        yield cleaned[start:end]
        if end == len(cleaned):
            break
        start = max(end - overlap, start + 1)


def load_chunks(input_path: Path, chunk_size: int, overlap: int) -> List[Chunk]:
    chunks: List[Chunk] = []
    for source_file in iter_source_files(input_path):
        source = str(source_file)
        text = read_text(source_file)
        for section_item in split_sections(text):
            title = section_item["title"] or source_file.stem
            section = section_item["section"]
            subsection = section_item["subsection"]
            prefix_parts = []
            if section:
                prefix_parts.append(f"章节: {section}")
            if subsection:
                prefix_parts.append(f"子章节: {subsection}")
            prefix = " | ".join(prefix_parts)
            for part in chunk_text(section_item["content"], chunk_size, overlap):
                chunk_body = f"{prefix} | {part}" if prefix else part
                chunks.append(Chunk(
                    source=source,
                    title=title,
                    text=chunk_body,
                    section=section,
                    subsection=subsection,
                    page=None,
                    chunk_index=len(chunks),
                    metadata={
                        "content_length": len(part),
                        "source_name": source_file.name,
                    },
                ))
    return chunks

