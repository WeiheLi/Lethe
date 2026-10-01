# Lethe

Lethe detects **vanished items**: an item that has arrived in at least `L` of
the `W` windows so far, and has then gone silent for `g` consecutive windows,
is reported while the stream is still running.

## Paper

Weihe Li. The Dog That Didn't Bark: Catching What Stops Arriving, While the
Stream Runs. PVLDB 2027.

`technical-report/technical-report.pdf` carries the model and the proofs.

## Files

```
cpu/lethe.h                   Lethe and Lethe-B
cpu/pandora.h stable.h ...    the baselines and an exact table
cpu/run.cc                    one run: a trace, a method, a budget -> a CSV row
cpu/trace_stats.cc            ground truth
cpu/gen_zipf.cc               a Zipf stream generator
fpga/                         the RTL core, its testbench and a reference model
```

## Compile

```
make
```

g++ with C++17. `bin/run` is Lethe and the baselines, `bin/run-b` is Lethe-B,
`bin/run-both` carries both stores and is the build to use when only accuracy
matters.

## Run

A trace is a flat binary file of 8-byte records, each two little-endian
`uint32`, one per arrival in stream order. The six real streams of the paper
are not redistributable, so generate one:

```
bin/gen_zipf 200000 4000000 1.0 7 trace.pairs
```

Then the ground truth:

```
bin/trace_stats trace.pairs 400 80 8 8 gt
```

Then a run, here at a budget of 60 KB:

```
bin/run   --trace trace.pairs --gt gt_gt.csv --method lethe  --mem 37809 \
          --W 400 --L 80 --g 8 --G 8 --match 8 --seed 1 --scan \
          --p-bits 16 --ts-bits 16 --tag r

bin/run-b --trace trace.pairs --gt gt_gt.csv --method letheb --mem 61440 \
          --W 400 --L 80 --g 8 --G 8 --match 8 --seed 1 --scan --tag r
```

Every method but Lethe takes the budget in bytes directly. Lethe is charged 13
bytes a bucket while `bin/run`'s store is 8 wide, so it is passed
`61440 * 8 / 13`, and `--p-bits 16 --ts-bits 16` are what make its fields that
wide. Without them the store narrows to 6 bytes, the budget silently buys twice
the buckets, and Lethe comes out above Lethe-B. Lethe-B is charged 10 and takes
the budget itself.

`--method` is one of `lethe`, `letheb`, `pandora`, `stable`, `hyper`, `bloom`,
`exact`. Each run prints one CSV row, 0-indexed: field 10 is precision, 11
recall, 12 `F1`, 16 the rate in M arrivals/s and 17 the bucket count.

## FPGA

See `fpga/README.md`.

## License

MIT.
