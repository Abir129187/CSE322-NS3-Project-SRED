#!/usr/bin/env python3
"""
plot_results.py — Generate graphs for wired and wireless SRED simulation results.

Reads CSV files from scratch/results/ and generates plots for each metric
across parameter sweeps.

Categorizes results by network type (Wired vs Wireless).
"""

import os
import csv
import matplotlib
matplotlib.use('Agg')  # non-interactive backend
import matplotlib.pyplot as plt

# ── Configuration ──
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RESULTS_DIR = os.path.join(SCRIPT_DIR, "results")
GRAPH_DIR = os.path.join(RESULTS_DIR, "graphs")

os.makedirs(GRAPH_DIR, exist_ok=True)

TYPES = ["wired", "wireless"]

EXPERIMENTS = [
    {"suffix": "vary_nodes.csv", "x_col": 0, "x_label": "Number of Nodes", "name": "nodes"},
    {"suffix": "vary_flows.csv", "x_col": 1, "x_label": "Number of Flows", "name": "flows"},
    {"suffix": "vary_pps.csv", "x_col": 2, "x_label": "Packets Per Second", "name": "pps"},
    {"suffix": "vary_speed.csv", "x_col": 3, "x_label": "Node Speed (m/s)", "name": "speed", "type": "wireless"},
    {"suffix": "vary_range.csv", "x_col": 3, "x_label": "Range Scale (x Tx_range)", "name": "range", "type": "wired"},
]

METRICS = [
    {"col": 4, "label": "Network Throughput (kbps)", "name": "throughput"},
    {"col": 5, "label": "Average End-to-End Delay (ms)", "name": "delay"},
    {"col": 6, "label": "Packet Delivery Ratio", "name": "pdr"},
    {"col": 7, "label": "Packet Drop Ratio", "name": "drop_ratio"},
    {"col": 8, "label": "Energy Consumption (J)", "name": "energy"},
]

plt.rcParams.update({
    'font.family': 'sans-serif', 'font.size': 12, 'axes.grid': True,
    'figure.figsize': (10, 6), 'figure.dpi': 150
})

COLORS = ['#2196F3', '#4CAF50', '#FF9800', '#E91E63', '#9C27B0']

def read_csv(filepath):
    rows = []
    if not os.path.exists(filepath): return []
    with open(filepath, 'r') as f:
        reader = csv.reader(f)
        next(reader, None) # skip header
        for row in reader:
            if row: rows.append([float(v) for v in row])
    return rows

def plot_single(x, y, x_label, y_label, title, path, color):
    plt.figure()
    plt.plot(x, y, 'o-', color=color, markersize=8, markerfacecolor='white', markeredgewidth=2)
    plt.xlabel(x_label)
    plt.ylabel(y_label)
    plt.title(title)
    plt.grid(True, alpha=0.3)
    plt.savefig(path)
    plt.close()

def main():
    print(f"Generating graphs for results in: {RESULTS_DIR}")
    for t in TYPES:
        print(f"Processing type: {t}")
        for exp in EXPERIMENTS:
            if "type" in exp and exp["type"] != t: continue
            
            fname = f"{t}_{exp['suffix']}"
            rows = read_csv(os.path.join(RESULTS_DIR, fname))
            if not rows: continue
            
            print(f"  Processing {fname}")
            x = [r[exp['x_col']] for r in rows]
            
            for i, metric in enumerate(METRICS):
                if metric["name"] == "energy" and t == "wired": continue # Skip energy for wired
                
                y = [r[metric['col']] for r in rows]
                title = f"{t.capitalize()}: {metric['label']} vs {exp['x_label']}"
                out_name = f"{t}_{exp['name']}_{metric['name']}.png"
                plot_single(x, y, exp['x_label'], metric['label'], title, 
                            os.path.join(GRAPH_DIR, out_name), COLORS[i % len(COLORS)])

    print("Graphs generated successfully.")

if __name__ == "__main__":
    main()
