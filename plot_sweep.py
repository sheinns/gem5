#!/usr/bin/env python3

import pandas as pd
import matplotlib.pyplot as plt
import os
import sys

def main():
    # Use command-line arguments if provided, otherwise default to these names
    csv_on = sys.argv[1] if len(sys.argv) > 1 else "/gem5/m5out/lvp_sweep_results.csv"
    csv_off = sys.argv[2] if len(sys.argv) > 2 else "/gem5/m5out/lvp_sweep_results_off.csv"
    output_image = "/gem5/m5out/lvp_sweep_plot.png"

    if not os.path.exists(csv_on):
        print(f"Error: Could not find LVP ON data at {csv_on}")
        return
    if not os.path.exists(csv_off):
        print(f"Error: Could not find LVP OFF data at {csv_off}")
        return

    print(f"Reading LVP ON data from {csv_on}...")
    df_on = pd.read_csv(csv_on)
    
    print(f"Reading LVP OFF data from {csv_off}...")
    df_off = pd.read_csv(csv_off)
    
    if df_on.empty or df_off.empty:
        print("Error: One or both CSV files are empty.")
        return

    # Plotting
    plt.figure(figsize=(10, 6))
    
    # Plot LVP ON
    plt.plot(df_on['ITER'], df_on['simTicks'], marker='o', linestyle='-', color='b', label='LVP ON (simTicks)')
    
    # Plot LVP OFF
    plt.plot(df_off['ITER'], df_off['simTicks'], marker='x', linestyle='--', color='r', label='LVP OFF (simTicks)')
    
    # Formatting the plot
    plt.title('LVP Performance Impact: Iterations vs Simulated Ticks', fontsize=14)
    plt.xlabel('Number of Iterations (ITERS)', fontsize=12)
    plt.ylabel('Simulated Ticks', fontsize=12)
    plt.grid(True, linestyle=':', alpha=0.7)
    plt.legend()
    
    # Format y-axis to not use scientific notation if it's not too large
    plt.ticklabel_format(style='plain', axis='y')

    # Save the plot
    plt.savefig(output_image, dpi=300, bbox_inches='tight')
    print(f"Plot successfully saved to {output_image}")

if __name__ == "__main__":
    main()
