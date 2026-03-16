"""
Prompt-construction benchmark.

Simulates the real-world pattern of:
  1. Receive 1000 decorated data items (proto-like dataclasses)
  2. Per item: parse fields → string formatting → tokenizer encode
  3. Concatenate all fragments into one final prompt

Runs three variants side by side:
  - serial        : plain Python for-loop, single thread
  - asyncio       : asyncio.gather — cooperative, GIL held during CPU work
  - cfuture       : ThreadPoolExecutor — sub-interpreters, true parallelism
                    during tokenizer (Rust, releases GIL) and independently
                    during string work in each sub-interpreter

The tokenizer is HuggingFace `tokenizers` (Rust-backed) which releases the
GIL, making this a hybrid case: string formatting holds the GIL, tokenizer
encode does not.

Usage:
    python -m stress_test.prompt_bench [--items N] [--runs N] [--workers N]
"""
from __future__ import annotations

import asyncio
import dataclasses
import random
import statistics
import time
import argparse

from tokenizers import Tokenizer

from cfuture import Future, ThreadPoolExecutor, all_of, xi_dataclass


# ── shared tokenizer ──────────────────────────────────────────────────────────

def load_tokenizer() -> Tokenizer:
    print("Loading GPT-2 tokenizer...", end=" ", flush=True)
    tok = Tokenizer.from_pretrained("gpt2")
    print("done")
    return tok


# ── data model ────────────────────────────────────────────────────────────────
# xi_dataclass so instances can cross sub-interpreter boundaries via deps=

@xi_dataclass
@dataclasses.dataclass
class ItemAttribute:
    key: str
    value: str


@xi_dataclass
@dataclasses.dataclass
class DecoratedItem:
    item_id: int
    title: str
    category: str
    subcategory: str
    score: float
    tags: list  # list[str]
    description: str
    author: str
    region: str
    metadata: dict   # dict[str, str]
    attributes: list  # list of plain dicts (key, value)
    related_ids: list  # list[int]


_WORDS = (
    "the quick brown fox jumps over lazy dog has many properties "
    "attributes features categories regions scores authors tags "
    "descriptions metadata version active primary secondary"
).split()


def _sentence(n: int) -> str:
    return " ".join(random.choices(_WORDS, k=n))


def make_items(n: int) -> list[DecoratedItem]:
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
        for i in range(n)
    ]


# ── per-item processing ───────────────────────────────────────────────────────

def _format_item(item: DecoratedItem, tok: Tokenizer) -> str:
    """
    Pure-Python string formatting (GIL held) then tokenizer encode (GIL released).
    Returns a tagged text fragment for this item.
    """
    attrs = "\n".join(f"  {a['key']}: {a['value']}" for a in item.attributes)
    tags_str = ", ".join(item.tags)
    meta_str = " | ".join(f"{k}={v}" for k, v in item.metadata.items())
    related_str = ", ".join(str(r) for r in item.related_ids)

    text = (
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
    encoded = tok.encode(text)
    return f"<item id={item.item_id} tokens={len(encoded.ids)}>{text}</item>"


# ── cfuture worker callback ───────────────────────────────────────────────────
# Must be at module level.
#
# Sub-interpreters have their own module state — a module-level variable set
# in one task persists for subsequent tasks on the same worker.  We exploit
# this to load the tokenizer once per worker rather than once per item.

def worker_process_item(x: int, d: list) -> str:
    """Runs inside a cfuture sub-interpreter worker."""
    import stress_test.prompt_bench as _self  # own module, worker-local copy
    from tokenizers import Tokenizer as _Tok  # noqa: PLC0415

    # Load tokenizer once per sub-interpreter; reuse on every subsequent task.
    if not hasattr(_self, "_worker_tok"):
        _self._worker_tok = _Tok.from_pretrained("gpt2")
    tok = _self._worker_tok

    item = d[0]
    attrs = "\n".join(f"  {a['key']}: {a['value']}" for a in item.attributes)
    tags_str = ", ".join(item.tags)
    meta_str = " | ".join(f"{k}={v}" for k, v in item.metadata.items())
    related_str = ", ".join(str(r) for r in item.related_ids)

    text = (
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
    encoded = tok.encode(text)
    return f"<item id={item.item_id} tokens={len(encoded.ids)}>{text}</item>"


# ── three variants ────────────────────────────────────────────────────────────

def run_serial(items: list[DecoratedItem], tok: Tokenizer) -> tuple[str, float]:
    t0 = time.perf_counter()
    fragments = [_format_item(item, tok) for item in items]
    prompt = "\n".join(fragments)
    return prompt, (time.perf_counter() - t0) * 1000


async def _run_asyncio_inner(items: list[DecoratedItem], tok: Tokenizer) -> tuple[str, float]:
    t0 = time.perf_counter()

    async def handle(item: DecoratedItem) -> str:
        return _format_item(item, tok)

    fragments = await asyncio.gather(*[handle(item) for item in items])
    prompt = "\n".join(fragments)
    return prompt, (time.perf_counter() - t0) * 1000


def run_asyncio(items: list[DecoratedItem], tok: Tokenizer) -> tuple[str, float]:
    return asyncio.run(_run_asyncio_inner(items, tok))


def run_cfuture(items: list[DecoratedItem], pool: ThreadPoolExecutor) -> tuple[str, float]:
    t0 = time.perf_counter()
    futs: list[Future[str]] = [
        pool.submit(lambda: 0).then(worker_process_item, deps=[item])
        for item in items
    ]
    fragments: list[str] = all_of(*futs).result(timeout=120.0)
    prompt = "\n".join(fragments)
    return prompt, (time.perf_counter() - t0) * 1000


# ── runner ────────────────────────────────────────────────────────────────────

def _report_run(label: str, times_ms: list[float], n_items: int) -> None:
    mean = statistics.mean(times_ms)
    p50 = statistics.median(times_ms)
    p95 = sorted(times_ms)[int(len(times_ms) * 0.95)]
    throughput = n_items / (mean / 1000)
    print(f"\n  {label}")
    print(f"    runs        : {len(times_ms)}")
    print(f"    mean        : {mean:.0f} ms")
    print(f"    p50         : {p50:.0f} ms")
    print(f"    p95         : {p95:.0f} ms")
    print(f"    min/max     : {min(times_ms):.0f} / {max(times_ms):.0f} ms")
    print(f"    throughput  : {throughput:,.0f} items/s")


def main() -> None:
    parser = argparse.ArgumentParser(description="Prompt-construction benchmark")
    parser.add_argument("--items", type=int, default=1000,
                        help="number of items per batch (default: 1000)")
    parser.add_argument("--runs", type=int, default=5,
                        help="number of timed runs per variant (default: 5)")
    parser.add_argument("--workers", type=int, default=None,
                        help="cfuture worker count (default: cpu_count)")
    parser.add_argument("--skip-serial", action="store_true",
                        help="skip serial baseline (saves time on large --items)")
    args = parser.parse_args()

    import os
    n_workers = args.workers or os.cpu_count() or 4
    random.seed(42)

    print("\nPrompt-construction benchmark")
    print(f"  items={args.items}  runs={args.runs}  cfuture_workers={n_workers}")
    print("  tokenizer: HuggingFace tokenizers (Rust, releases GIL)")

    tok = load_tokenizer()
    items = make_items(args.items)

    # ── warm up tokenizer (avoid first-call overhead in timed runs) ────────────
    _ = tok.encode("warmup")

    # ── serial ────────────────────────────────────────────────────────────────
    serial_times: list[float] = []
    if not args.skip_serial:
        print(f"\nRunning serial ({args.runs} runs)...")
        for i in range(args.runs):
            _, ms = run_serial(items, tok)
            serial_times.append(ms)
            print(f"  run {i+1}: {ms:.0f}ms", flush=True)

    # ── asyncio ───────────────────────────────────────────────────────────────
    print(f"\nRunning asyncio ({args.runs} runs)...")
    asyncio_times: list[float] = []
    for i in range(args.runs):
        _, ms = run_asyncio(items, tok)
        asyncio_times.append(ms)
        print(f"  run {i+1}: {ms:.0f}ms", flush=True)

    # ── cfuture ───────────────────────────────────────────────────────────────
    print(f"\nBooting cfuture pool ({n_workers} workers)...", flush=True)
    pool = ThreadPoolExecutor(workers=n_workers)
    # Warm the pool: one task per worker to load the tokenizer in each
    # sub-interpreter before the timed runs.
    print("Warming workers (loading tokenizer in each sub-interpreter)...", flush=True)
    warm_items = items[:n_workers]
    warm_futs = [pool.submit(lambda: 0).then(worker_process_item, deps=[item])
                 for item in warm_items]
    all_of(*warm_futs).result(timeout=60.0)
    print("Workers warm.")

    print(f"\nRunning cfuture ({args.runs} runs)...")
    cfuture_times: list[float] = []
    for i in range(args.runs):
        _, ms = run_cfuture(items, pool)
        cfuture_times.append(ms)
        print(f"  run {i+1}: {ms:.0f}ms", flush=True)

    pool.shutdown()

    # ── report ────────────────────────────────────────────────────────────────
    print(f"\n{'━' * 56}")
    print(f"  Results — {args.items} items/batch, {args.runs} runs")
    print(f"{'━' * 56}")

    if serial_times:
        _report_run("serial (baseline)", serial_times, args.items)

    _report_run("asyncio (gather, GIL-bound string work)", asyncio_times, args.items)
    _report_run(f"cfuture ({n_workers} sub-interpreter workers)", cfuture_times, args.items)

    if serial_times:
        serial_mean = statistics.mean(serial_times)
        cfuture_mean = statistics.mean(cfuture_times)
        asyncio_mean = statistics.mean(asyncio_times)
        print("\n  Speedup vs serial:")
        print(f"    asyncio : {serial_mean / asyncio_mean:.2f}x")
        print(f"    cfuture : {serial_mean / cfuture_mean:.2f}x")

    cfuture_mean = statistics.mean(cfuture_times)
    asyncio_mean = statistics.mean(asyncio_times)
    print(f"\n  cfuture vs asyncio : {asyncio_mean / cfuture_mean:.2f}x faster")
    print(f"{'━' * 56}\n")


if __name__ == "__main__":
    main()
