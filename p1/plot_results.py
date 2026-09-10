#!/usr/bin/env python3
"""
Parse fio JSON results from ./fio_results/ and generate multiple plots:
  1. Line plot: throughput vs block size, one line per (qd, jobs) combo
  2. Line plot: throughput vs iodepth, one line per (bs, jobs) combo
  3. Line plot: throughput vs num_jobs, one line per (bs, qd) combo
  4. Heatmap: block size vs iodepth (best jobs for each cell)
  5. Heatmap: block size vs num_jobs (best iodepth for each cell)
  6. Heatmap: iodepth vs num_jobs (best block size for each cell)
  7. Grouped bar chart: top-20 configurations by throughput

Output PNGs are saved to ./fio_plots/
"""

import json
import re
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np
import pandas as pd
import seaborn as sns

RESULTS_DIR = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("results/fio_results")
PLOTS_DIR = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("results/fio_plots")
PLOTS_DIR.mkdir(parents=True, exist_ok=True)

BS_ORDER = ["4k", "16k", "64k", "256k", "1m", "4m", "8m", "16m"]


def bs_to_bytes(bs: str) -> int:
    m = re.match(r"(\d+)([kmg]?)", bs.lower())
    val, unit = int(m.group(1)), m.group(2)
    return val * {"": 1, "k": 1024, "m": 1024**2, "g": 1024**3}[unit]


def load_results() -> pd.DataFrame:
    rows = []
    count = 0
    for f in sorted(RESULTS_DIR.glob("bs*_qd*_nj*.json")):
        m = re.match(r"bs(\w+)_qd(\d+)_nj(\d+)\.json", f.name)
        if not m:
            continue
        bs, qd, nj = m.group(1), int(m.group(2)), int(m.group(3))
        count += 1
        data = json.loads(f.read_text())
        bw_bytes = data["jobs"][0]["read"]["bw_bytes"]  # bytes/s
        bw_mb = bw_bytes / 1024**2
        rows.append(
            {
                "bs": bs,
                "iodepth": qd,
                "num_jobs": nj,
                "throughput_mb": bw_mb,
                "bs_bytes": bs_to_bytes(bs),
            }
        )
    if not rows:
        sys.exit("No results found in fio_results/. Run bench.sh first.")
    df = pd.DataFrame(rows)
    actual_bs = df["bs"].unique().tolist()
    ordered_bs = [b for b in BS_ORDER if b in actual_bs]
    df["bs"] = pd.Categorical(df["bs"], categories=ordered_bs, ordered=True)
    return df.sort_values(["bs_bytes", "iodepth", "num_jobs"])


def save(fig, name):
    path = PLOTS_DIR / name
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved {path}")


def plot_line_vs_x(df, x_col, x_label, group_cols, fname):
    fig, ax = plt.subplots(figsize=(10, 6))
    groups = df.groupby(group_cols)
    for key, grp in groups:
        label = ", ".join(
            f"{c}={v}"
            for c, v in zip(group_cols, key if isinstance(key, tuple) else [key])
        )
        grp_sorted = grp.sort_values(x_col if x_col != "bs" else "bs_bytes")
        x_vals = grp_sorted[x_col].astype(str).tolist()
        ax.plot(x_vals, grp_sorted["throughput_mb"], marker="o", label=label)
    ax.set_xlabel(x_label)
    ax.set_ylabel("Throughput (MB/s)")
    ax.set_title(f"Sequential Read Throughput vs {x_label}")
    ax.legend(fontsize=7, ncol=2, loc="upper left")
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: f"{x:.0f}"))
    fig.tight_layout()
    save(fig, fname)


def plot_heatmap(df, row_col, col_col, agg, title, fname):
    pivot = df.groupby([row_col, col_col])["throughput_mb"].agg(agg).unstack()
    if row_col == "bs":
        pivot = pivot.reindex([b for b in BS_ORDER if b in pivot.index])
    fig, ax = plt.subplots(figsize=(9, 6))
    sns.heatmap(
        pivot,
        ax=ax,
        annot=True,
        fmt=".0f",
        cmap="YlOrRd",
        cbar_kws={"label": "Throughput (MB/s)"},
    )
    ax.set_title(f"{title}\n(value = {agg} throughput MB/s across other params)")
    ax.set_xlabel(col_col)
    ax.set_ylabel(row_col)
    fig.tight_layout()
    save(fig, fname)


def plot_top_bar(df, n, fname):
    top = df.nlargest(n, "throughput_mb").copy()
    top["config"] = top.apply(
        lambda r: f"bs={r['bs']} qd={r['iodepth']} nj={r['num_jobs']}", axis=1
    )
    fig, ax = plt.subplots(figsize=(12, 6))
    colors = plt.cm.viridis(np.linspace(0.2, 0.9, len(top)))
    bars = ax.barh(top["config"], top["throughput_mb"], color=colors)
    ax.bar_label(bars, fmt="%.0f MB/s", padding=4, fontsize=8)
    ax.set_xlabel("Throughput (MB/s)")
    ax.set_title(f"Top {n} Configurations by Sequential Read Throughput")
    ax.invert_yaxis()
    fig.tight_layout()
    save(fig, fname)


def main():
    df = load_results()
    print(f"Loaded {len(df)} benchmark results.")

    # 1. Throughput vs block size
    plot_line_vs_x(
        df, "bs", "Block Size", ["iodepth", "num_jobs"], "01_throughput_vs_bs.png"
    )

    # 2. Throughput vs iodepth
    plot_line_vs_x(
        df,
        "iodepth",
        "I/O Depth (queue depth)",
        ["bs", "num_jobs"],
        "02_throughput_vs_iodepth.png",
    )

    # 3. Throughput vs num_jobs
    plot_line_vs_x(
        df,
        "num_jobs",
        "Number of Jobs",
        ["bs", "iodepth"],
        "03_throughput_vs_numjobs.png",
    )

    # 4–6. Heatmaps (max across the third dimension)
    plot_heatmap(
        df,
        "bs",
        "iodepth",
        "max",
        "Block Size vs I/O Depth (max over num_jobs)",
        "04_heatmap_bs_iodepth.png",
    )
    plot_heatmap(
        df,
        "bs",
        "num_jobs",
        "max",
        "Block Size vs Num Jobs (max over iodepth)",
        "05_heatmap_bs_numjobs.png",
    )
    plot_heatmap(
        df,
        "iodepth",
        "num_jobs",
        "max",
        "I/O Depth vs Num Jobs (max over block size)",
        "06_heatmap_iodepth_numjobs.png",
    )

    # 7. Top-20 bar chart
    plot_top_bar(df, 20, "07_top20_bar.png")

    print(f"\nAll plots saved to {PLOTS_DIR}/")


if __name__ == "__main__":
    main()
