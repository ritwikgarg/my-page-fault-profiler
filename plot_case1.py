import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path

Path("plots").mkdir(exist_ok=True)

def load_profile(path):
    rows = []
    with open(path, "r") as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) != 4:
                continue
            try:
                rows.append([int(x) for x in parts])
            except ValueError:
                continue
    return np.array(rows, dtype=np.int64)

def get_case1_series(path):
    data = load_profile(path)
    if len(data) == 0:
        raise RuntimeError(f"No numeric samples found in {path}")

    soft = data[:, 1]
    hard = data[:, 2]
    faults = soft + hard
    acc_faults = np.cumsum(faults)
    time_sec = np.arange(len(data)) / 20.0
    return time_sec, acc_faults

def plot_case1(time_sec, acc_faults, outfile, title, y_max):
    plt.figure()
    plt.plot(time_sec, acc_faults, linewidth=2)
    plt.xlabel("Time (s)")
    plt.ylabel("Accumulated Page Fault Count")
    plt.title(title)
    plt.ylim(0, y_max)
    plt.grid(True)
    plt.savefig(outfile, bbox_inches="tight")
    plt.close()

# Load both datasets first
t1, acc1 = get_case1_series("profile1.data")
t2, acc2 = get_case1_series("profile2.data")

# Use the same y-axis range for both plots
shared_y_max = max(acc1.max(), acc2.max()) * 1.05

plot_case1(
    t1,
    acc1,
    "plots/case_1_work_1_2.png",
    "Case 1: 1024MB R 50000 + 1024MB R 10000",
    shared_y_max
)

plot_case1(
    t2,
    acc2,
    "plots/case_1_work_3_4.png",
    "Case 1: 1024MB R 50000 + 1024MB L 10000",
    shared_y_max
)

print("Done.")