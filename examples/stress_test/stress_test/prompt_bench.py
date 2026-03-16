"""
Prompt-construction benchmark.

Simulates the real-world pattern of:
  - Each REQUEST carries a list of 1000 decorated data items
  - Processing: format all items into text strings, tokenize the batch
    in one encode_batch call, assemble into one final prompt
  - Multiple concurrent requests compete for the pool

Three variants are compared:
  serial   — plain Python for-loop, one request at a time
  asyncio  — asyncio.gather, one coroutine per request
  cfuture  — one Future per request; tokenizer shared via pool shared=
             so it is deserialized once per worker, not once per request

The tokenizer is HuggingFace `tokenizers` (Rust-backed).  It releases
the GIL during encode_batch, so sub-interpreters can run their Rust
tokenization concurrently.  String formatting is pure Python (GIL held)
but is a small fraction of total per-request time.

Usage:
    python -m stress_test.prompt_bench [--items N] [--requests N]
                                        [--runs N] [--workers N]
"""
from __future__ import annotations

import asyncio
import dataclasses
import os
import random
import statistics
import time
import argparse

from tokenizers import Tokenizer

from cfuture import Future, ThreadPoolExecutor, all_of, xi_dataclass


# ── data model ────────────────────────────────────────────────────────────────
# @xi_dataclass so instances cross sub-interpreter boundaries via deps=

@xi_dataclass
@dataclasses.dataclass
class DecoratedItem:
    item_id: int
    title: str
    category: str
    subcategory: str
    score: float
    tags: list       # list[str]
    description: str
    author: str
    region: str
    metadata: dict   # dict[str, str]
    attributes: list  # list of {"key": str, "value": str}
    related_ids: list  # list[int]


_WORDS = (
    "the quick brown fox jumps over lazy dog has many properties "
    "attributes features categories regions scores authors tags "
    "descriptions metadata version active primary secondary"
).split()


def _sentence(n: int) -> str:
    return " ".join(random.choices(_WORDS, k=n))


def make_request(n_items: int) -> list[DecoratedItem]:
    """Generate one request worth of decorated items."""
    return [
        DecoratedItem(
            item_id=i,
            title=f"Item {i}: {_sentence(8)}",
            category=f"category_{i % 10}",
            subcategory=f"sub_{i % 50}",
            score=round(random.uniform(0.1, 1.0), 3),
            tags=[f"tag_{j}_{_sentence(2)}" for j in range(i % 8 + 3)],
            description=_sentence(60),
            author=f"author_{i % 20}: {_sentence(4)}",
            region=f"region_{i % 8}",
            metadata={f"key_{k}": _sentence(3) for k in range(5)},
            attributes=[{"key": f"attr_{k}", "value": _sentence(4)} for k in range(10)],
            related_ids=list(range(i, i + 5)),
        )
        for i in range(n_items)
    ]


# ── formatting helpers (shared by all variants) ───────────────────────────────

def _item_to_text(item: DecoratedItem) -> str:
    """Format one item into its text fragment. Pure Python — GIL held."""
    attrs = "\n".join(f"  {a['key']}: {a['value']}" for a in item.attributes)
    tags_str = ", ".join(item.tags)
    meta_str = " | ".join(f"{k}={v}" for k, v in item.metadata.items())
    related_str = ", ".join(str(r) for r in item.related_ids)
    return (
        f"### Item {item.item_id}\n"
        f"Title: {item.title}\n"
        f"Category: {item.category} > {item.subcategory}\n"
        f"Score: {item.score:.3f} | Author: {item.author} | Region: {item.region}\n"
        f"Tags: {tags_str}\n"
        f"Description: {item.description}\n"
        f"Attributes:\n{attrs}\n"
        f"Metadata: {meta_str}\n"
        f"Related: {related_str}\n"
    )


def _process_request(items: list[DecoratedItem], tok: Tokenizer) -> str:
    """
    Process one full request: format all items, tokenize in one batch,
    assemble into a prompt.  This is the unit of work per Future/coroutine.
    """
    texts = [_item_to_text(item) for item in items]
    # encode_batch releases the GIL — Rust processes all strings concurrently
    encodings = tok.encode_batch(texts)
    fragments = [
        f"<item id={item.item_id} tokens={len(enc.ids)}>{text}</item>"
        for item, text, enc in zip(items, texts, encodings)
    ]
    return "\n".join(fragments)


# ── cfuture worker callback ───────────────────────────────────────────────────
# Runs inside a sub-interpreter.  Tokenizer is passed via shared= as a JSON
# string (serialized once at pool creation) and cached at module level so
# deserialization happens once per worker, not once per request.

def worker_process_request(x: int, d: list, s: dict) -> str:
    """
    d[0] = list[DecoratedItem] for this request
    s['tok_json'] = tokenizer serialized to JSON string
    """
    import stress_test.prompt_bench as _self  # worker-local module copy

    if not hasattr(_self, "_worker_tok"):
        from tokenizers import Tokenizer as _T
        _self._worker_tok = _T.from_str(s["tok_json"])
    tok = _self._worker_tok

    items = d[0]
    texts = [_self._item_to_text(item) for item in items]
    encodings = tok.encode_batch(texts)
    fragments = [
        f"<item id={item.item_id} tokens={len(enc.ids)}>{text}</item>"
        for item, text, enc in zip(items, texts, encodings)
    ]
    return "\n".join(fragments)


# ── three variants ────────────────────────────────────────────────────────────

def run_serial(
    requests: list[list[DecoratedItem]], tok: Tokenizer
) -> tuple[list[str], float]:
    t0 = time.perf_counter()
    results = [_process_request(req, tok) for req in requests]
    return results, (time.perf_counter() - t0) * 1000


async def _asyncio_inner(
    requests: list[list[DecoratedItem]], tok: Tokenizer
) -> tuple[list[str], float]:
    t0 = time.perf_counter()

    async def handle(req: list[DecoratedItem]) -> str:
        return _process_request(req, tok)

    results = await asyncio.gather(*[handle(req) for req in requests])
    return list(results), (time.perf_counter() - t0) * 1000


def run_asyncio(
    requests: list[list[DecoratedItem]], tok: Tokenizer
) -> tuple[list[str], float]:
    return asyncio.run(_asyncio_inner(requests, tok))


def run_cfuture(
    requests: list[list[DecoratedItem]], pool: ThreadPoolExecutor
) -> tuple[list[str], float]:
    t0 = time.perf_counter()
    futs: list[Future[str]] = [
        pool.submit(lambda: 0).then(worker_process_request, deps=[req])
        for req in requests
    ]
    results: list[str] = all_of(*futs).result(timeout=300.0)
    return results, (time.perf_counter() - t0) * 1000


# ── reporting ─────────────────────────────────────────────────────────────────

def _report(label: str, times_ms: list[float], n_requests: int, n_items: int) -> None:
    mean = statistics.mean(times_ms)
    p50 = statistics.median(times_ms)
    p95 = sorted(times_ms)[max(0, int(len(times_ms) * 0.95) - 1)]
    req_per_s = n_requests / (mean / 1000)
    items_per_s = (n_requests * n_items) / (mean / 1000)
    print(f"\n  {label}")
    print(f"    mean        : {mean:.0f} ms")
    print(f"    p50/p95     : {p50:.0f} / {p95:.0f} ms")
    print(f"    min/max     : {min(times_ms):.0f} / {max(times_ms):.0f} ms")
    print(f"    req/s       : {req_per_s:.1f}")
    print(f"    items/s     : {items_per_s:,.0f}")


# ── main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="Prompt-construction benchmark")
    parser.add_argument("--items", type=int, default=1000,
                        help="items per request (default: 1000)")
    parser.add_argument("--requests", type=int, default=8,
                        help="concurrent requests per run (default: 8)")
    parser.add_argument("--runs", type=int, default=5,
                        help="timed runs per variant (default: 5)")
    parser.add_argument("--workers", type=int, default=None,
                        help="cfuture worker count (default: cpu_count)")
    parser.add_argument("--skip-serial", action="store_true")
    args = parser.parse_args()

    n_workers = args.workers or os.cpu_count() or 4
    random.seed(42)

    print("\nPrompt-construction benchmark")
    print(f"  items/request={args.items}  concurrent_requests={args.requests}"
          f"  runs={args.runs}  workers={n_workers}")
    print("  tokenizer: HuggingFace tokenizers (Rust, GIL released during encode_batch)")

    print("\nLoading GPT-2 tokenizer...", end=" ", flush=True)
    tok = Tokenizer.from_pretrained("gpt2")
    tok_json = tok.to_str()
    print(f"done  ({len(tok_json):,} chars serialized)")

    print("Generating request data...", end=" ", flush=True)
    # Same requests reused across all runs — fair comparison
    requests = [make_request(args.items) for _ in range(args.requests)]
    print("done")

    tok.encode("warmup")  # ensure tokenizer is fully initialized

    # ── serial ────────────────────────────────────────────────────────────────
    serial_times: list[float] = []
    if not args.skip_serial:
        print(f"\nRunning serial ({args.runs} runs × {args.requests} requests)...")
        for i in range(args.runs):
            _, ms = run_serial(requests, tok)
            serial_times.append(ms)
            print(f"  run {i+1}: {ms:.0f}ms", flush=True)

    # ── asyncio ───────────────────────────────────────────────────────────────
    print(f"\nRunning asyncio ({args.runs} runs × {args.requests} requests)...")
    asyncio_times: list[float] = []
    for i in range(args.runs):
        _, ms = run_asyncio(requests, tok)
        asyncio_times.append(ms)
        print(f"  run {i+1}: {ms:.0f}ms", flush=True)

    # ── cfuture ───────────────────────────────────────────────────────────────
    # Tokenizer JSON goes into shared= — encoded once, available in every
    # worker as s['tok_json'].  Workers deserialize and cache on first use.
    print(f"\nBooting cfuture pool ({n_workers} workers) with shared tokenizer...",
          flush=True)
    pool = ThreadPoolExecutor(workers=n_workers, shared={"tok_json": tok_json})

    print("Warming workers (first task triggers tokenizer deserialization)...",
          flush=True)
    warm = [pool.submit(lambda: 0).then(worker_process_request, deps=[requests[i % len(requests)]])
            for i in range(n_workers)]
    all_of(*warm).result(timeout=60.0)
    print("Workers warm.")

    print(f"\nRunning cfuture ({args.runs} runs × {args.requests} requests)...")
    cfuture_times: list[float] = []
    for i in range(args.runs):
        _, ms = run_cfuture(requests, pool)
        cfuture_times.append(ms)
        print(f"  run {i+1}: {ms:.0f}ms", flush=True)

    pool.shutdown()

    # ── report ────────────────────────────────────────────────────────────────
    print(f"\n{'━' * 60}")
    print(f"  Results — {args.items} items/req, {args.requests} concurrent requests")
    print(f"{'━' * 60}")

    if serial_times:
        _report("serial", serial_times, args.requests, args.items)

    _report("asyncio (gather, GIL held for string work)", asyncio_times, args.requests, args.items)
    _report(f"cfuture ({n_workers} sub-interpreters, shared tokenizer)",
            cfuture_times, args.requests, args.items)

    if serial_times:
        sm = statistics.mean(serial_times)
        print("\n  Speedup vs serial:")
        print(f"    asyncio : {sm / statistics.mean(asyncio_times):.2f}x")
        print(f"    cfuture : {sm / statistics.mean(cfuture_times):.2f}x")

    am = statistics.mean(asyncio_times)
    cm = statistics.mean(cfuture_times)
    winner = "cfuture" if cm < am else "asyncio"
    ratio = max(am, cm) / min(am, cm)
    print(f"\n  {winner} wins by {ratio:.2f}x")
    print(f"{'━' * 60}\n")


if __name__ == "__main__":
    main()
