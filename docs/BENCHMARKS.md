# Benchmarks

`bg_bench` reports completed operations per second (real, not partial). It covers:

- flow admission throughput
- allocation decision throughput (256-flow weighted water-fill)
- reservation create/release
- snapshot creation (with a large active-flow pool)
- persistence encode and decode
- N-thread submit+tick scheduling (1, 2, 4 threads)
- real bounded CUDA host-to-device transfer governance

```bash
bg_bench
```

Typical results on the validation host (RTX 5090 / CUDA 13.1) show admission and
allocation decision throughput in the tens-to-hundreds of thousands of operations
per second and a real bounded CUDA transfer rate in the multi-GB/s range.
