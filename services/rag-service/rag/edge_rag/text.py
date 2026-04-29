import re


def cjk_bigrams(text: str):
    for span in re.findall(r"[\u4e00-\u9fff]{2,}", text):
        for index in range(len(span) - 1):
            yield span[index:index + 2]


def query_terms(text: str):
    terms = re.findall(r"[A-Za-z0-9_]+", text)
    terms.extend(cjk_bigrams(text))
    seen = set()
    out = []
    for term in terms:
        if term not in seen:
            seen.add(term)
            out.append(term)
    return out


def searchable_text(text: str) -> str:
    bigrams = list(cjk_bigrams(text))
    if not bigrams:
        return text
    return text + "\n" + " ".join(bigrams)

