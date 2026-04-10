import os
import subprocess
import numpy as np
import matplotlib.pyplot as plt

# Configuration
RESULTS_DIR = "scratch/results/paper/"
os.makedirs(RESULTS_DIR, exist_ok=True)
SIM_TIME = 50


def run_simulations():
    flows_symmetrical = [100]

    # # Symmetrical Runs
    # for flows in flows_symmetrical:
    #     print(f"Running symmetrical topology simulation with {flows} flows...")
    #     cmd = f'./ns3 run "sred-paper-sim-original --nFlows={flows} --topologyType=symmetrical --simTime={SIM_TIME}"'
    #     subprocess.run(cmd, shell=True)

    # Asymmetrical Run
    print("Running asymmetrical topology simulation with 100 flows...")
    cmd = f'./ns3 run "sred-paper-sim-original --nFlows=100 --topologyType=asymmetrical --simTime={SIM_TIME}"'
    subprocess.run(cmd, shell=True)


def plot_symmetrical_buffer():
    flows_symmetrical = [100]

    fig, axes = plt.subplots(1, 1, figsize=(12, 4))

    for i, flows in enumerate(flows_symmetrical):
        fn = os.path.join(RESULTS_DIR, f"buffer_symmetrical_{flows}.txt")
        

        data = np.loadtxt(fn)
        if len(data) == 0:
            continue

        time = data[:, 0]
        occupancy = data[:, 1]

        ax = axes
        ax.fill_between(time, occupancy, color='black', linewidth=0)
        ax.set_title(f"SRED with {flows} persistent connections, buffer occupation", fontsize=9)
        ax.set_xlabel("Time (sec)", fontsize=9, color='blue')
        ax.set_ylabel("buffer occupancy in bytes", fontsize=9, color='blue')
        ax.set_xlim(0, SIM_TIME)
        ax.set_ylim(0, 500000)
        ax.tick_params(axis='x', colors='blue')
        ax.tick_params(axis='y', colors='blue')
        for spine in ax.spines.values():
            spine.set_edgecolor('blue')
        ax.grid(False)

    plt.tight_layout()
    out = os.path.join(RESULTS_DIR, "symmetrical_buffer_occupancy.png")
    plt.savefig(out, dpi=300)
    print(f"Saved symmetrical buffer plots to {out}")


def plot_asymmetrical_throughput():
    fn = os.path.join(RESULTS_DIR, "throughput_asymmetrical_100.txt")
    

    data = np.loadtxt(fn, skiprows=1)
    if len(data) == 0:
        return

    source_number = data[:, 0]
    normalized_tp = data[:, 2]

    fig, ax = plt.subplots(figsize=(6, 4))
    ax.bar(source_number, normalized_tp, width=0.8, color='black', linewidth=0)
    ax.set_xlabel("source number", fontsize=9, color='blue')
    ax.set_ylabel("normalized total throughput", fontsize=9, color='blue')
    ax.set_xlim(0, 100)
    ax.set_ylim(0, 0.02)
    ax.tick_params(axis='x', colors='blue')
    ax.tick_params(axis='y', colors='blue')
    for spine in ax.spines.values():
        spine.set_edgecolor('blue')
    ax.grid(False)

    plt.tight_layout()
    out = os.path.join(RESULTS_DIR, "asymmetrical_throughput.png")
    plt.savefig(out, dpi=300)
    print(f"Saved asymmetrical throughput plot to {out}")


if __name__ == "__main__":
    import sys

    if "--run" in sys.argv:
        run_simulations()

    # plot_symmetrical_buffer()
    plot_asymmetrical_throughput()