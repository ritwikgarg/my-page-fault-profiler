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

def normalized_total_util_percent(path):
    data = load_profile(path)
    if len(data) < 2:
        raise RuntimeError(f"Not enough numeric samples in {path}")

    j = data[:, 0].astype(np.float64)      # jiffies timestamp
    cpu = data[:, 3].astype(np.float64)    # interval CPU time in jiffies

    delta_j = np.diff(j)
    cpu_interval = cpu[1:]                 # interval ending at sample i

    valid = delta_j > 0
    delta_j = delta_j[valid]
    cpu_interval = cpu_interval[valid]

    util = cpu_interval / delta_j          # fraction of 1 CPU
    return util.mean() * 100.0

Ns = [5, 11, 16, 20, 22]
utils = [normalized_total_util_percent(f"case2_N{n}.data") for n in Ns]

plt.figure()
plt.plot(Ns, utils, marker="o", linewidth=2)
plt.xlabel("N")
plt.ylabel("Total CPU Utilization (%)")
plt.title("Case 2: CPU Utilization vs Degree of Multiprogramming")
plt.ylim(0, 20)
plt.grid(True)
plt.savefig("plots/case_2.png", bbox_inches="tight")
plt.close()

print("Done.")