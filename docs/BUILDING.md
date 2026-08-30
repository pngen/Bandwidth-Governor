# Building, installing, and using Bandwidth Governor

## Requirements

- CMake 3.21+ - Visual Studio 2022 (MSVC) on Windows - C++20 - Optionally
  NVIDIA CUDA 13.1 for the real-transfer backend

## Configure and build (Release)

```bash
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release --target bgcore bg bgworker bg_examples bg_bench bg_tests bg_mp
```

Zero compiler warnings is a hard requirement: MSVC builds with /W4 /WX. To enable
the CUDA transfer backend, ensure CMake can find CUDAToolkit (BG_BUILD_CUDA=ON, the
default when CUDA is present).

## Run the tests

```bash
ctest --test-dir build -C Release --output-on-failure
```

The suite has no timeouts: tests run until they naturally pass, fail, crash, or are
manually terminated after diagnosing a genuine hang.

## Install as a package

```bash
cmake --install build --config Release --prefix <prefix>
```

This installs the bgcore library, public headers, and a CMake package config that
exports the `Bandwidth::bgcore` target.

## Downstream find_package consumer

```bash
cmake -S consumer -B consumer-build -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH=<prefix>
cmake --build consumer-build --config Release
consumer-build\Release\consumer.exe
```

## CLI

`bg` operates an in-process governor and also serves the distributed coordinator:

| command | description |
|---|---|
| `bg demo <scenario>` | run a synthetic demo |
| `bg resources`, `bg paths`, `bg flows` | inspect state |
| `bg submit ...`, `bg cancel <id>` | admission control |
| `bg reservations`, `bg allocations`, `bg saturation` | inspect commitments |
| `bg explain <id>` | explain a decision |
| `bg save <path>`, `bg load <path>` | persistence |
| `bg serve --port N` | run the coordinator |

## Distributed runtime

1. `bg serve --port 21000` starts the coordinator.
2. `bgworker --coordinator 127.0.0.1:21000 --worker-id 1 --boot 10 --backend synthetic`
   registers a worker.
3. A client submits flows over the framed TCP protocol.

## Examples and benchmarks

```bash
bg_examples fairness
bg_bench
```
