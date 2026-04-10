import matplotlib.pyplot as plt
import sys
import os

def plot_queue(file_path, output_image):
    if not os.path.exists(file_path):
        print(f"Error: Could not find {file_path}")
        return

    times = []
    queue_sizes = []

    with open(file_path, 'r') as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) == 2:
                times.append(float(parts[0]))
                queue_sizes.append(float(parts[1]))

    plt.figure(figsize=(10, 6))
    plt.plot(times, queue_sizes, marker='.', linestyle='none', color='b', markersize=2, alpha=0.5)
    plt.title('Variation in Queue Size over Time (SRED - Hybrid Wired/Wireless)')
    plt.xlabel('Time (s)')
    plt.ylabel('Queue Size (Packets)')
    plt.grid(True)
    
    plt.savefig(output_image)
    print(f"Queue size plot saved to {output_image}")

if __name__ == '__main__':
    data_file = 'scratch/queue_size.txt'
    if len(sys.argv) > 1:
        data_file = sys.argv[1]
    
    output_file = data_file.replace('.txt', '.png')
    plot_queue(data_file, output_file)
