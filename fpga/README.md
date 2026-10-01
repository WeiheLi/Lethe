# Lethe on an FPGA

`lethe_core_scan.v` is the core the paper reports. One row is one true dual-port
block RAM: arrivals hold port A, port B scans the buckets. The field widths are
parameters, so the same source is both variants.

## Files

```
lethe_core_scan.v   the core
lethe_tb_scan.v     its testbench (top module lethe_tb_scan)
hwmodel_scan.cc     a reference implementation of the same update rule
letheb_tb.v         the same two at Lethe-B's field widths
letheb_hwmodel.cc     (top module lethe_tb_b)
synth_scan.tcl      out-of-context synthesis and implementation
synth_b.tcl
```

## Place and route

Vivado 2024.2, on an `xcku5p-ffvb676-2-e`:

```
vivado -mode batch -source synth_scan.tcl \
       -tclargs <addr_w> <period_ns> <out_dir> [place_dir] [route_dir]
vivado -mode batch -source synth_b.tcl \
       -tclargs <addr_w> <period_ns> <out_dir> <ts_w> <p_w> [place_dir] [route_dir]
```

`addr_w` is the per-row address width, so the table holds `2 * 2**addr_w`
buckets; the paper's five sizes are `addr_w` 9 to 13, at `period_ns` 3.
`synth_b.tcl` also takes `ts_w` and `p_w`, which are 4 and 9 at those settings.
Each run writes `<out_dir>/result.txt` with the post-route slack and the
implied maximum clock, next to the utilization and timing reports.

The paper reports the median over four runs. `place_design -seed` is not
accepted in Vivado 2024.2, so the runs differ by directive instead: Default,
Explore and WLDrivenBlockPlacement go in `place_dir`, ExtraNetDelay_high in
`route_dir`. Both default to Default.

## Equivalence check

```
g++ -O2 -o hwmodel_scan hwmodel_scan.cc
./hwmodel_scan <trace.pairs> 11 400 80 8 stim_scan.txt expect_scan.txt 1638400

xvlog lethe_core_scan.v lethe_tb_scan.v
xelab -debug typical lethe_tb_scan -s tb
xsim tb -runall

diff rtl_scan.txt  expect_scan.txt
diff rtl_state.txt model_state.txt
```

The model writes the stimulus and the reports it expects; the testbench replays
that stimulus and writes `rtl_scan.txt` and `rtl_state.txt`. Both diffs are
empty: the core and the model emit the same reports in the same order, and
every one of the 4,096 buckets agrees field by field. Any trace works; the
arguments above replay 1,638,400 arrivals in 400 windows, which is the
tightest case `M = N`.

For Lethe-B, in a separate directory so the state dumps do not collide:

```
g++ -O2 -o letheb_hwmodel letheb_hwmodel.cc
./letheb_hwmodel <trace.pairs> 11 400 80 8 stim_scan.txt expect_scan.txt 1638400

xvlog lethe_core_scan.v letheb_tb.v
xelab -debug typical lethe_tb_b -s tb
xsim tb -runall
```

Both models write `model_state.txt` and both testbenches write
`rtl_state.txt`, so running the two checks in one directory overwrites the
first. Lethe-B's report list matches Lethe's up to the withheld increment the
paper's Prop. 5.8 proves harmless: a returning entry whose silence is an exact
multiple of `2**ts_w` reads zero and skips one count.

Where an arrival report and a scan report come due in the same cycle the core
emits the arrival's and the model prints both; the author's replay contains no
such coincidence. Strip CRLF before diffing on Windows.
