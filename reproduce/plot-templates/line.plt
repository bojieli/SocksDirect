# Generic line plot. Caller passes:
#   csv   = path to result.csv
#   out   = path to output PDF
#   title = plot title
#
# Expects CSV columns: iter,bench,submode,mode,msg_size,throughput_mps,p50_ns
# Plots throughput vs msg_size (latency on twin y-axis if present).

set terminal pdf size 6,4 font "sans,11"
set output out
set title title
set datafile separator ","
set xlabel "Message size (bytes)"
set ylabel "Throughput (msg/s)"
set logscale x 2
set key top left
set grid

# Skip the header row.
plot csv every ::1 using ((stringcolumn(4) eq "throughput") ? column(5) : 1/0):6 \
        with linespoints lw 2 pt 7 ps 0.6 title "throughput"
