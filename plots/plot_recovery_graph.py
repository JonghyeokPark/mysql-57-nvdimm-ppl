import os
import sys
import re

sum_time = 0
undo_time = 0

def extract_times_from_log(log_filename, output_filename):
    global sum_time, undo_time
    try:
        with open(log_filename, "r") as infile, open(output_filename, "w") as outfile:
            outfile.write("NV-PPL ")
            for line in infile:
                if "scan_time" in line:
                    time_value = line.split()[1]
                    outfile.write(f"{time_value} ")
                    sum_time += float(time_value)
                elif"redo_time" in line:
                    time_value = line.split()[1]
                    outfile.write(f"{time_value} ")
                    sum_time += float(time_value)
                elif "undo_time" in line:
                    time_value = line.split()[1]
                    outfile.write(f"{str(float(time_value) + 5)}\n")
                    sum_time += float(time_value)
                    undo_time = float(time_value)
            outfile.write("0 0 0\n")
    except FileNotFoundError:
        print(f"파일 '{log_filename}'을 찾을 수 없습니다.")
    except Exception as e:
        print(f"오류 발생: {e}")

def create_gnuplot_script(log_output_file, script_filename, output_graph):
    script_content = f"""
set terminal pdf size 8,3 font "Helvetica, 28"
set output '{output_graph}'
set key right bottom spacing 1 samplen 2.0 height 0.1 font "Helvetica, 28"
set key invert

set style line 2 lc rgb 'black' lt 1 lw 1
set grid y
set style data histograms
set style histogram rowstacked

# Histogram 간의 간격을 조절
set boxwidth 0.55

# 왼쪽 여백 조절
set offsets -0.4, 0, 0, 0

set label 1 at -0.17,{str(round(sum_time + 20, 1))} "{str(round(undo_time, 1))} sec" font "Helvetica, 28"

set xtics nomirror font "Helvetica, 28" 
set style fill pattern border -1 
set ytics 10 nomirror
set yrange [:250]
set ylabel "Time (seconds)" off 1,0
set ytics 50


plot '{log_output_file}' using 2 t "Analysis" lc rgbcolor "black", '' using 3 t "Redo" lc rgbcolor "black", '' using 4:xtic(1) t "Undo" lc rgbcolor "black" fs pattern 3
"""
    with open(script_filename, "w") as script_file:
        script_file.write(script_content)

def main():
    if len(sys.argv) < 2:
        print("Usage: python script.py <log_file>")
        sys.exit(1)
    
    log_file = sys.argv[1]
    log_output_file = "./plots/recovery_processed.txt"
    gnuplot_script_file = "./plots/make_recovery_graph.gnu"
    output_graph = "./plots/recovery_graph.pdf"

    # Extract times from log file
    extract_times_from_log(log_file, log_output_file)
    

    # # Create gnuplot script
    create_gnuplot_script(log_output_file, gnuplot_script_file, output_graph)

    # # Generate graph using gnuplot
    command = f"gnuplot {gnuplot_script_file}"
    os.system(command)
    
    # # Clean up
    os.remove(log_output_file)
    os.remove(gnuplot_script_file)

if __name__ == "__main__":
    main()
