# Benchmarks

| Benchmark | What it measures |
|---|---|
| [`xdp/run.sh`](xdp/run.sh) | How fast one CPU can push 64-byte UDP datagrams through a veth pair when the receiver drops them with native XDP, generic XDP, netfilter (`lkl_nf`), or at the UDP socket lookup |

```sh
make && cmake -S . -B build -DLKL_LIBBPF_DIR=$PWD/.tools/libbpf && cmake --build build
sudo benchmarks/xdp/run.sh 1000000       # packets per mode
```

The output is a Markdown table: packets sent, seconds, packets per
second, and how many the drop point counted. veth runs the receive side
on the sending CPU, so the rate includes the cost of receiving up to the
drop point.

Results depend on the machine, and on shared CI runners they vary from
run to run. Compare the rows of one run, not separate runs. Recorded
results, with the machine they came from, are in
[docs/TESTING.md](../docs/TESTING.md). Broader benchmarks are planned
for M8 ([docs/ROADMAP.md](../docs/ROADMAP.md)).
