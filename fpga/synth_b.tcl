# Out-of-context synthesis + implementation of lethe_core on the KCU116's
# Kintex UltraScale+ XCKU5P, at the field widths of Lethe-B: the last-seen
# field ceil(log2(g+2)) bits and the count ceil(log2(W+1)) bits, passed in as
# generics so the same core serves both rows of the table.
set part   xcku5p-ffvb676-2-e
set addr_w [lindex $argv 0]
set period [lindex $argv 1]
set outdir [lindex $argv 2]
set ts_w   [lindex $argv 3]
set p_w    [lindex $argv 4]

file mkdir $outdir
create_project -in_memory -part $part

set here [file dirname [file normalize [info script]]]
read_verilog $here/lethe_core_scan.v
synth_design -top lethe_core -part $part -mode out_of_context \
             -generic ADDR_W=$addr_w -generic TS_W=$ts_w -generic P_W=$p_w

create_clock -name clk -period $period [get_ports clk]
set_input_delay  -clock clk 0.5 [get_ports {in_valid rst}]
set_input_delay  -clock clk 0.5 [get_ports in_id*]
set_input_delay  -clock clk 0.5 [get_ports in_cw*]
set_output_delay -clock clk 0.5 [get_ports rep_valid]
set_output_delay -clock clk 0.5 [get_ports rep_id*]
set_output_delay -clock clk 0.5 [get_ports rep_p*]

set pdir [lindex $argv 5]
set rdir [lindex $argv 6]
if {$pdir eq ""} { set pdir Default }
if {$rdir eq ""} { set rdir Default }

opt_design
place_design -directive $pdir
phys_opt_design
route_design -directive $rdir

set wns [get_property SLACK [lindex [get_timing_paths -max_paths 1 -setup] 0]]
set fmax [expr {1000.0 / ($period - $wns)}]

report_utilization -file $outdir/util.rpt
report_timing_summary -file $outdir/timing.rpt

set fh [open $outdir/result.txt w]
puts $fh "part $part"
puts $fh "addr_w $addr_w"
puts $fh "ts_w $ts_w"
puts $fh "p_w $p_w"
puts $fh "bucket_bits [expr {64 + $p_w + $ts_w + 1}]"
puts $fh "buckets_per_way [expr {1 << $addr_w}]"
puts $fh "buckets_total [expr {2 * (1 << $addr_w)}]"
puts $fh "target_ns $period"
puts $fh "wns_ns $wns"
puts $fh "fmax_mhz [format %.1f $fmax]"
close $fh
puts "DONE_MARKER wns=$wns fmax=[format %.1f $fmax]"
exit
