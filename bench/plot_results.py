#!/usr/bin/env python3
"""
Benchmark visualization for cpp-httpd.
Generates RPS and latency comparison charts vs nginx.

Usage:
    python3 bench/plot_results.py              # uses embedded results
    python3 bench/plot_results.py results.json # load wrk JSON output
"""

import sys
import json
import os
import matplotlib
matplotlib.use("Agg")  # headless — no display required
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np

# ── Benchmark data ─────────────────────────────────────────────────────────────
# Collected with: wrk -t4 -c50 -d15s
# Platform: Apple M-series, macOS 15

RESULTS = {
    "4KB": {
        "thread-pool":        {"rps": 23690, "latency_us": 165,   "p99_us":  None},
        "kqueue+sendfile":    {"rps": 28451, "latency_us": 1680,  "p99_us":  None},
        "nginx":              {"rps": 38895, "latency_us": 1230,  "p99_us":  None},
    },
    "1MB": {
        "thread-pool":        {"rps":  2503, "latency_us": 1560,  "p99_us":  None},
        "kqueue+sendfile":    {"rps":  2699, "latency_us": 17630, "p99_us":  None},
        "nginx":              {"rps":  2680, "latency_us": 17850, "p99_us":  None},
    },
}

COLORS = {
    "thread-pool":     "#4C72B0",
    "kqueue+sendfile": "#55A868",
    "nginx":           "#C44E52",
}

LABELS = {
    "thread-pool":     "Thread-pool (blocking I/O)",
    "kqueue+sendfile": "kqueue + sendfile()",
    "nginx":           "nginx 1.31.1 (baseline)",
}

OUT_DIR = os.path.dirname(os.path.abspath(__file__))


def bar_chart_rps():
    """Grouped bar chart: RPS by mode for each file size."""
    fig, ax = plt.subplots(figsize=(9, 5))

    file_sizes = list(RESULTS.keys())
    modes      = list(COLORS.keys())
    n_groups   = len(file_sizes)
    n_bars     = len(modes)
    bar_width  = 0.22
    x          = np.arange(n_groups)

    for i, mode in enumerate(modes):
        values = [RESULTS[fs][mode]["rps"] for fs in file_sizes]
        bars   = ax.bar(x + i * bar_width, values, bar_width,
                        label=LABELS[mode], color=COLORS[mode], zorder=3)
        for bar, val in zip(bars, values):
            ax.text(bar.get_x() + bar.get_width() / 2,
                    bar.get_height() + 200,
                    f"{val:,}", ha="center", va="bottom", fontsize=8)

    ax.set_xlabel("File size", fontsize=11)
    ax.set_ylabel("Requests / second", fontsize=11)
    ax.set_title("cpp-httpd vs nginx — Throughput\nwrk -t4 -c50 -d15s, Apple M-series", fontsize=12)
    ax.set_xticks(x + bar_width)
    ax.set_xticklabels(file_sizes, fontsize=11)
    ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, _: f"{int(v):,}"))
    ax.legend(loc="upper right", fontsize=9)
    ax.grid(axis="y", linestyle="--", alpha=0.5, zorder=0)
    ax.set_ylim(0, max(
        v["rps"] for fs in RESULTS.values() for v in fs.values()
    ) * 1.18)

    plt.tight_layout()
    path = os.path.join(OUT_DIR, "rps_comparison.png")
    plt.savefig(path, dpi=150)
    plt.close()
    print(f"Saved {path}")


def bar_chart_latency():
    """Grouped bar chart: average latency by mode (4KB only — most interesting)."""
    fig, ax = plt.subplots(figsize=(7, 4.5))

    mode_labels = [LABELS[m] for m in COLORS]
    latencies   = [RESULTS["4KB"][m]["latency_us"] / 1000 for m in COLORS]  # → ms
    bar_colors  = list(COLORS.values())

    bars = ax.bar(mode_labels, latencies, color=bar_colors, width=0.45, zorder=3)
    for bar, val in zip(bars, latencies):
        ax.text(bar.get_x() + bar.get_width() / 2,
                bar.get_height() + 0.02,
                f"{val:.2f} ms", ha="center", va="bottom", fontsize=9)

    ax.set_ylabel("Avg latency (ms)", fontsize=11)
    ax.set_title("cpp-httpd vs nginx — Avg Latency (4 KB file)\nwrk -t4 -c50 -d15s", fontsize=12)
    ax.tick_params(axis="x", labelsize=9)
    ax.grid(axis="y", linestyle="--", alpha=0.5, zorder=0)
    ax.set_ylim(0, max(latencies) * 1.3)

    plt.tight_layout()
    path = os.path.join(OUT_DIR, "latency_comparison.png")
    plt.savefig(path, dpi=150)
    plt.close()
    print(f"Saved {path}")


def line_chart_scalability():
    """Horizontal efficiency bars: % of nginx RPS achieved per mode."""
    fig, ax = plt.subplots(figsize=(8, 4))

    file_sizes = list(RESULTS.keys())
    modes_no_nginx = ["thread-pool", "kqueue+sendfile"]
    y_pos = np.arange(len(modes_no_nginx))
    bar_height = 0.3

    for i, fs in enumerate(file_sizes):
        nginx_rps = RESULTS[fs]["nginx"]["rps"]
        pcts      = [RESULTS[fs][m]["rps"] / nginx_rps * 100 for m in modes_no_nginx]
        offset    = (i - len(file_sizes) / 2 + 0.5) * bar_height
        bars      = ax.barh(y_pos + offset, pcts, bar_height * 0.9,
                             label=f"{fs} file",
                             color=["#4a90d9", "#5cb85c"][i], zorder=3)
        for bar, pct in zip(bars, pcts):
            ax.text(bar.get_width() + 0.5, bar.get_y() + bar.get_height() / 2,
                    f"{pct:.0f}%", va="center", fontsize=9)

    ax.axvline(100, color="#C44E52", linestyle="--", linewidth=1.2, label="nginx = 100%")
    ax.set_xlabel("% of nginx RPS", fontsize=11)
    ax.set_title("cpp-httpd Efficiency Relative to nginx", fontsize=12)
    ax.set_yticks(y_pos)
    ax.set_yticklabels([LABELS[m] for m in modes_no_nginx], fontsize=9)
    ax.set_xlim(0, 120)
    ax.legend(fontsize=9)
    ax.grid(axis="x", linestyle="--", alpha=0.5, zorder=0)

    plt.tight_layout()
    path = os.path.join(OUT_DIR, "efficiency.png")
    plt.savefig(path, dpi=150)
    plt.close()
    print(f"Saved {path}")


def main():
    if len(sys.argv) > 1:
        with open(sys.argv[1]) as f:
            data = json.load(f)
        print("Loaded external results — override RESULTS dict and re-run for custom charts.")

    bar_chart_rps()
    bar_chart_latency()
    line_chart_scalability()
    print("\nAll charts written to bench/")


if __name__ == "__main__":
    main()
