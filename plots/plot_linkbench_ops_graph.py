import os
import sys
import re

def process_linkbench_data(input_filename, output_filename):
    try:
        with open(input_filename, "r") as infile, open(output_filename, "w") as outfile:
            for line in infile:
                match = re.search(r'(\d+)/\d+ requests finished.*at (\d+\.\d+) ops/sec', line)
                if match:
                    request_count = match.group(1)
                    ops_per_sec = match.group(2)
                    outfile.write(f"{request_count},{ops_per_sec}\n")
    except FileNotFoundError:
        print(f"파일 '{input_filename}'을 찾을 수 없습니다.")
    except Exception as e:
        print(f"오류 발생: {e}")

def create_gnuplot_script(data_filename, script_filename, output_graph):
    script_content = f"""
set terminal pdf size 4,1.5 font "Helvetica, 13"
set output '{output_graph}'
set datafile separator ","
set style line 1 lc rgb '#ff7f0e' lt 1 lw 2.5 pt 6 pi 500 ps 0.7

# 그래프 위에 여백 추가
set tmargin 1.1

# 범례를 그래프 중간에 위치시키기
set key at graph 0.5,1.07 center horizontal maxrows 1 font "Helvetica, 12"

set xlabel "Number of Operations"
set ylabel "Throughput (OPS)"

# X 축 눈금을 100K 단위로 설정
set xtics ("0" 0, "5M" 5000000, "10M" 10000000, "15M" 15000000, "20M" 20000000, "25M" 25000000) 
set ytics ("0" 0, "10K" 10000, "20K" 20000, "30K" 30000, "40K" 40000, "50K" 50000)
set xtics nomirror

# Y 축 눈금을 400단위로 설정
set xrange [-1000000:26000000]
set yrange [0:50000]

set multiplot
# Plot all three linespoints with different dash types
plot "{data_filename}" using ($1):2 with linespoints ls 1 title "NV-PPL"
"""
    with open(script_filename, "w") as script_file:
        script_file.write(script_content)

def main():
    if len(sys.argv) < 2:
        print("Usage: python script.py <input_file>")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = "./plots/nvppl_processed.txt"
    gnuplot_script_file = "./plots/make_linkbench_graph.gnu"
    output_graph = "./plots/linkbench_25M_graph.pdf"

    # Process tpcc data
    process_linkbench_data(input_file, output_file)

    # Create gnuplot script
    create_gnuplot_script(output_file, gnuplot_script_file, output_graph)

    # Generate graph using gnuplot
    command = f"gnuplot {gnuplot_script_file}"
    os.system(command)
    
    # Clean up
    os.remove(output_file)
    os.remove(gnuplot_script_file)

if __name__ == "__main__":
    main()
