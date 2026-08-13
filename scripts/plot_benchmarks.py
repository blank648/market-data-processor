import json
import matplotlib.pyplot as plt
import os

def main():
    benchmarks_file = 'benchmarks/baseline_results.json'
    output_file = 'benchmarks/benchmark_chart.png'

    if not os.path.exists(benchmarks_file):
        print(f"File {benchmarks_file} not found.")
        return

    with open(benchmarks_file, 'r') as f:
        data = json.load(f)

    # Filter benchmarks to plot throughputs
    names = []
    throughputs = []

    for b in data.get('benchmarks', []):
        name = b.get('name', '')
        # Only plot throughputs for a cleaner chart
        if 'items_per_second' in b:
            if name.startswith('BM_RingBuffer'):
                display_name = name.split('/')[0].replace('BM_', '')
                val = b['items_per_second']
                if display_name not in names:
                    names.append(display_name)
                    throughputs.append(val)
            elif name.startswith('BM_TickParser_Throughput'):
                display_name = 'TickParser_Throughput'
                val = b['items_per_second']
                if display_name not in names:
                    names.append(display_name)
                    throughputs.append(val)
            elif name.startswith('BM_Normalizer_Throughput_Unique'):
                display_name = 'Normalizer_Unique'
                val = b['items_per_second']
                if display_name not in names:
                    names.append(display_name)
                    throughputs.append(val)
            elif name.startswith('BM_Pipeline_Throughput'):
                display_name = 'Pipeline_Throughput'
                val = b['items_per_second']
                if display_name not in names:
                    names.append(display_name)
                    throughputs.append(val)

    if not names:
        print("No throughput data found.")
        return

    # Convert to Millions/sec for readability
    throughputs_m = [v / 1e6 for v in throughputs]

    plt.figure(figsize=(10, 6))
    bars = plt.barh(names, throughputs_m, color=['#4C72B0', '#DD8452', '#55A868', '#C44E52', '#8172B3', '#937860'])
    plt.xlabel('Millions of items / second (Higher is better)')
    plt.title('Market Data Processor - Component Throughput')
    plt.gca().invert_yaxis()
    plt.grid(axis='x', linestyle='--', alpha=0.7)

    # Add text labels on bars
    for bar in bars:
        width = bar.get_width()
        plt.text(width * 1.05, bar.get_y() + bar.get_height()/2.0, f'{width:.1f} M', ha='left', va='center', color='black')

    plt.tight_layout()
    plt.savefig(output_file, dpi=300)
    print(f"Chart saved to {output_file}")

if __name__ == '__main__':
    main()
