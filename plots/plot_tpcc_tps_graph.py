import os
import sys

def process_tpcc_data(input_filename, output_filename):
    all_tps = 0
    with open(input_filename, "r") as infile, open(output_filename, "w") as outfile:
        for line in infile:
            if "trx:" in line:
                parts = line.split(",")
                trx_value = int(parts[1].split(":")[1].strip())
                all_tps += trx_value
                outfile.write(f"{all_tps},{trx_value}\n")

def create_gnuplot_script(data_filename, script_filename, output_graph):
    script_content = f"""
set terminal pdf size 4,1.5 font "Helvetica, 13"
set output '{output_graph}'
set datafile separator ","

# 그래프 위에 여백 추가
set tmargin 2

set multiplot

# 첫 번째 플롯: 실제 데이터 플롯 (범례 없음)
unset key

set xlabel "Number of Transactions"
set ylabel "Throughput (TPS)"
set xtics ("0" 0, "200K" 200000, "400K" 400000, "600K" 600000, "800K" 800000, "1M" 1000000)
set ytics 300
set yrange [0:1500]
set xrange [0:1000000]

plot "{data_filename}" using ($1):2 smooth unique with lines linetype 1 linecolor rgb "orange" linewidth 0.9 dashtype 1 title "NV-PPL"

set key at graph 0.5,1.07 center horizontal maxrows 1 font "Helvetica, 12"
unset tics; unset border; unset xlabel; unset ylabel

plot [][0:1] 2 title 'NV-PPL' lw 2.5 lc rgb "orange"
unset multiplot
"""
    with open(script_filename, "w") as script_file:
        script_file.write(script_content)

def main():
    if len(sys.argv) < 2:
        print("Usage: python script.py <input_file>")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = "./plots/nvppl_processed.txt"
    gnuplot_script_file = "./plots/tpcc_plot_script.gnu"
    output_graph = "./plots/tpcc_1M_graph.pdf"

    # Process tpcc data
    process_tpcc_data(input_file, output_file)

    # Create gnuplot script
    create_gnuplot_script(output_file, gnuplot_script_file, output_graph)

    # Generate graph using gnuplot
    command = f"gnuplot {gnuplot_script_file}"
    os.system(command)
    
    command = f"rm {output_file} {gnuplot_script_file}"
    os.system(command)
    
    

if __name__ == "__main__":
    main()
