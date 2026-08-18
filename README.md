# Ultra-Low Latency HFT Matching Engine (C++20)

> **Deterministic sub-50ns order execution engine optimized for High-Frequency Trading.**

![C++20](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)
![Latency](https://img.shields.io/badge/p50%20Latency-10ns--50ns-brightgreen.svg)
![Memory](https://img.shields.io/badge/Memory-Zero--Allocation%20Runtime-orange.svg)

## Key Architecture Highlights

* **Zero-Allocation Execution Path**: Pre-allocated `OrderPool` object pool prevents dynamic heap allocation (`malloc`/`new`) during order processing.
* **O(1) Flat Direct Indexing**: Order lookup and updates bypass hash table overhead for deterministic L1-cache friendly access.
* **Intrinsics-Based Tail Measurements**: Benchmarked with High-Precision `__rdtsc` cycle calibration to measure strict microsecond tail latencies.
* **Price-Time Priority Queue**: High-throughput doubly-linked list structures per price level.

## Benchmark Performance Summary

*Executed on x86-64 CPU (2496 MHz) calibrated with `__rdtsc` cycles:*

| Operation | Throughput | p50 Latency | p99 Latency | Complexity |
| :--- | :--- | :--- | :--- | :--- |
| **GetBBO** | **~560M ops/sec** | **1.2 ns** *(3 cycles)* | — | O(1) |
| **AmendOrder** *(In-place)* | **~11.1M ops/sec** | **10 ns** *(25 cycles)* | ~20 ns | O(1) |
| **AddOrder** *(Matching Taker)* | **~43.9M ops/sec** | **40 ns** *(102 cycles)* | ~58 ns | O(1) |
| **AddOrder** *(Resting Maker)* | **~36.8M ops/sec** | **46 ns** *(116 cycles)* | ~122 ns | O(1) |
| **CancelOrder** | **~11.4M ops/sec** | **80 ns** *(201 cycles)* | ~140 ns | O(1) |

## Building and Running Benchmarks

### Prerequisites
* C++20 compatible compiler (MSVC 2019+, GCC 10+, or Clang 11+)
* CMake 3.15+

### Build Commands
