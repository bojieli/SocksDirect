# Generic bar plot for figures with a small set of named submodes.
#   csv, out, title — same as line.plt.
# Expects CSV with at least: submode, throughput_mps.

set terminal pdf size 6,4 font "sans,11"
set output out
set title title
set datafile separator ","
set ylabel "Throughput (msg/s)"
set style data histogram
set style histogram clustered gap 1
set style fill solid 0.7 border -1
set boxwidth 0.9
set xtics rotate by -30
set key off
set grid ytics

plot csv every ::1 using 6:xticlabels(3) with histogram
