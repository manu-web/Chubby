import matplotlib.pyplot as plt

# Data
clients = [1, 2, 4, 8, 16, 32, 64, 128, 256]
latency = [0.503019, 0.532083, 0.771904, 1.20623, 1.92543, 3.74174, 7.3778, 13.1213, 25.1443]
throughput = [1988, 1879.41, 1295.5, 829.028, 519.365, 267.255, 135.542, 76.2117, 39.7704]
throughput_master =[713, 2454, 4926, 7518, 8333, 8620, 8849, 8130, 7874]
throughput_master_fail =[8333, 8130, 0, 0, 0, 0, 8333, 7874, 7874, 0, 0, 0, 0, 7874, 8130, 8333, 8130]
time = [0, 10, 12, 15, 18, 21, 24, 30, 40, 41, 45, 48, 51, 57, 60, 70, 80]

# Plot
# plt.figure(figsize=(10, 6))
# plt.plot(clients, throughput_master, marker='o', linestyle='-', color='b', label="Master Throughput (rps)")
plt.plot(time, throughput_master_fail, linestyle='-', color='b')

# Labels and title
plt.xlabel("Time", fontsize=12)
plt.ylabel("Master Throughput (rps)", fontsize=12)
plt.title("Master Throughput (Failover)", fontsize=14)
plt.grid(True, which="both", linestyle="--", linewidth=0.5)
# plt.legend(fontsize=12)

# Show the plot
plt.savefig('throughput_master_failover.png')
