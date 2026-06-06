#!/usr/bin/env python3
"""
MatchCore — benchmark chart generator
======================================
Reads the Phase 3 results from the DATA section below and writes
docs/benchmark.png, which is embedded in the README.

Usage
-----
    python3 docs/gen_charts.py

Regeneration after a new benchmark run
---------------------------------------
1. Run:  ./build/phase3_bench [ops_per_thread]
2. Copy the numbers from its output into the DATA section below.
3. Re-run this script.

Requirements: pip install matplotlib  (tested on 3.10+)
"""

from pathlib import Path
import matplotlib
matplotlib.use("Agg")   # non-interactive; works without a display / CI
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

# ── DATA ──────────────────────────────────────────────────────────────────────
# Source: ./build/phase3_bench 300000
# Workload: 300k ops/thread · 80% limit / 10% market / 10% cancel
# Prices:   $99.00–$101.00 (tight band) · qty 1–50 · Apple Silicon ARM64 · -O2

THREADS = [1, 2, 4, 8]

# MatchingEngine — MPSC queue + dedicated single matching thread
ENGINE_OPS_S = [10_311_965, 10_385_931, 7_211_507, 6_556_677]

# ConcurrentOrderBook — shared_mutex, writers serialise on unique_lock
BOOK_OPS_S  = [7_906_874, 4_329_282, 1_639_248,   829_067]
BOOK_P50_NS = [84,         125,        125,          167]
BOOK_P99_NS = [333,       8_900,      51_000,      155_300]


# ── HELPERS ───────────────────────────────────────────────────────────────────

def fmt_ns(ns: float) -> str:
    """Format nanoseconds into the most readable unit (ns / µs / ms)."""
    if ns >= 1_000_000:
        return f"{ns / 1_000_000:.1f} ms"
    if ns >= 1_000:
        return f"{ns / 1_000:.0f} µs"
    return f"{int(ns)} ns"


def annotate_line(ax, threads, values, fmt, color, *, above=True, edge_flip=True):
    """
    Place a data-value label next to each marker on a line.
    `edge_flip=True` mirrors the last label to the left so it stays in frame.
    """
    for i, (x, y) in enumerate(zip(threads, values)):
        last = edge_flip and i == len(threads) - 1
        ha   = "right" if last else "left"
        xoff = -8     if last else  8
        yoff = 6 if above else -12
        ax.annotate(
            fmt(y), (x, y),
            textcoords="offset points", xytext=(xoff, yoff),
            ha=ha, va="bottom" if above else "top",
            fontsize=8.5, color=color, fontweight="bold",
        )


# ── STYLE ─────────────────────────────────────────────────────────────────────

ENG_COLOR  = "#2563EB"  # blue   – MatchingEngine
BOOK_COLOR = "#DC2626"  # red    – ConcurrentOrderBook throughput
P50_COLOR  = "#16A34A"  # green  – p50 latency
P99_COLOR  = "#7C3AED"  # purple – p99 latency

plt.rcParams.update({
    "font.family":        "DejaVu Sans",
    "font.size":          11,
    "axes.titlesize":     12,
    "axes.titleweight":   "bold",
    "axes.labelsize":     10.5,
    "axes.labelcolor":    "#374151",
    "axes.spines.top":    False,
    "axes.spines.right":  False,
    "axes.edgecolor":     "#D1D5DB",
    "xtick.labelsize":    10,
    "ytick.labelsize":    10,
    "legend.fontsize":    9.5,
    "legend.framealpha":  0.92,
    "legend.edgecolor":   "#E5E7EB",
    "grid.color":         "#E5E7EB",
    "grid.linewidth":     0.9,
    "figure.facecolor":   "white",
    "axes.facecolor":     "#FAFAFA",
})


# ── FIGURE ────────────────────────────────────────────────────────────────────

fig, (ax1, ax2) = plt.subplots(
    1, 2, figsize=(13, 5.5),
    gridspec_kw={"width_ratios": [1.1, 1]},
)
fig.suptitle(
    "MatchCore Benchmark  ·  300k ops/thread  ·  "
    "80% limit / 10% market / 10% cancel  ·  "
    "$99–$101  ·  Apple Silicon  ·  Release (-O2)",
    fontsize=9.5, color="#6B7280", y=1.01,
)


# ── CHART 1: Throughput ───────────────────────────────────────────────────────

eng_m  = [v / 1_000_000 for v in ENGINE_OPS_S]
book_m = [v / 1_000_000 for v in BOOK_OPS_S]

ax1.plot(THREADS, eng_m,  "o-",  color=ENG_COLOR,  linewidth=2.5, markersize=8,
         label=f"MatchingEngine (MPSC queue)",   zorder=3)
ax1.plot(THREADS, book_m, "s--", color=BOOK_COLOR, linewidth=2.5, markersize=8,
         label="ConcurrentOrderBook (shared_mutex)", zorder=3)

annotate_line(ax1, THREADS, eng_m,  lambda v: f"{v:.1f}M", ENG_COLOR,  above=True)
annotate_line(ax1, THREADS, book_m, lambda v: f"{v:.1f}M", BOOK_COLOR, above=False)

# Dotted ceiling line at single-thread throughput
ceiling = eng_m[0]
ax1.axhline(ceiling, color=ENG_COLOR, linewidth=0.9, linestyle=":", alpha=0.55)
ax1.text(
    0.98, ceiling + 0.18,
    f"matching-thread ceiling  ({ceiling:.1f}M)",
    ha="right", va="bottom",
    transform=ax1.get_yaxis_transform(),
    fontsize=8, color=ENG_COLOR, alpha=0.75,
)

# Callout: why 2T is marginally faster than 1T
# With 2 producers, the matching thread is kept busier between batches —
# less idle time waiting for the next item. The gain is within run noise.
ax1.annotate(
    "1→2T: ≈ flat\n(matching thread is ceiling)",
    xy=(2, eng_m[1]), xytext=(2.7, 11.5),
    fontsize=7.5, color=ENG_COLOR,
    arrowprops=dict(arrowstyle="->", color=ENG_COLOR, lw=1.1),
    ha="left",
)
ax1.annotate(
    "4→8T: lock-convoy\n(p99 grows 450×)",
    xy=(8, book_m[3]), xytext=(5.5, 1.5),
    fontsize=7.5, color=BOOK_COLOR,
    arrowprops=dict(arrowstyle="->", color=BOOK_COLOR, lw=1.1),
    ha="left",
)

ax1.set_title("Throughput vs Thread Count", pad=10)
ax1.set_xlabel("Producer threads")
ax1.set_ylabel("Throughput  (million ops / s)")
ax1.set_xticks(THREADS)
ax1.set_xlim(0.5, 9)
ax1.set_ylim(0, max(eng_m) * 1.40)
ax1.yaxis.set_major_formatter(mticker.FormatStrFormatter("%.0f"))
ax1.legend(loc="upper right")
ax1.grid(axis="y")


# ── CHART 2: Latency ──────────────────────────────────────────────────────────

ax2.plot(THREADS, BOOK_P50_NS, "o-",  color=P50_COLOR, linewidth=2.5,
         markersize=8, label="p50  (median, uncontended fast-path)")
ax2.plot(THREADS, BOOK_P99_NS, "s--", color=P99_COLOR, linewidth=2.5,
         markersize=8, label="p99  (tail, lock-convoy victim)")

annotate_line(ax2, THREADS, BOOK_P50_NS, fmt_ns, P50_COLOR, above=False)
annotate_line(ax2, THREADS, BOOK_P99_NS, fmt_ns, P99_COLOR, above=True)

# p50 / p99 ratio at 8 threads — shows the convoy effect numerically
ratio = BOOK_P99_NS[-1] / BOOK_P50_NS[-1]
ax2.annotate(
    f"p99 / p50 = {ratio:.0f}×\nat 8 threads",
    xy=(8, BOOK_P99_NS[-1]), xytext=(5.0, 30_000),
    fontsize=8, color=P99_COLOR,
    arrowprops=dict(arrowstyle="->", color=P99_COLOR, lw=1.1),
)

ax2.set_title(
    "ConcurrentOrderBook — per-call Latency\n(log scale · p99 grows 450× from 1→8 threads)",
    pad=10,
)
ax2.set_xlabel("Producer threads")
ax2.set_ylabel("Latency  (log scale)")
ax2.set_yscale("log")
ax2.set_xticks(THREADS)
ax2.set_xlim(0.5, 9.5)
ax2.legend(loc="upper left")
ax2.grid(axis="y", which="both")
ax2.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: fmt_ns(v)))


# ── SAVE ──────────────────────────────────────────────────────────────────────

plt.tight_layout(pad=1.8)

out = Path(__file__).parent / "benchmark.png"
fig.savefig(out, dpi=150, bbox_inches="tight", facecolor="white")
sz_kb = out.stat().st_size // 1024
print(f"Saved  →  {out}  ({sz_kb} KB)")
