# Bandwidth Governor

**Open-source, vendor-neutral C++20 runtime for governing scarce data-movement
bandwidth across heterogeneous AI infrastructure.**

Bandwidth Governor answers one question:

> When many things want to move bytes at once, who gets how much of the scarce
> path capacity, under what guarantees, and why?

It does **not** move bytes. It arbitrates the *right* to move bytes.

## The systems boundary

**Transfer Fabric** owns data movement itself: planning transfers, routing,
staging, copying, overlapping, verification, and backend execution.

**Bandwidth Governor** owns arbitration over scarce bandwidth *capacity* between
competing flows. It treats bandwidth as a first-class governed runtime resource.
It is a serious standalone systems runtime - not a simulator wrapped around
counters.

```
   Transfer Fabric                    Bandwidth Governor
   ---------------                    -----------------
   How should bytes move?             Who gets how much of the scarce path
   plan route stage                   capacity, under what guarantees, and why?
   copy overlap verify                fairness priority latency reservations
   execute backends                   throttling preemption path-aware arbitration
```

## Why governance is distinct from execution

Execution does not know about *competing* intent. Ten flows that are individually
correct and well-formed can collectively saturate a PCIe link, a host-memory
bandwidth budget, or an inter-node TCP path. Governance exists precisely at the
point where *many* independent flows contend for the *same* capacity. It makes an
explicit, explainable decision about the share each flow gets, and it honors
reservations and service guarantees that execution alone cannot enforce.

## Architecture

The runtime is built from first-principles value types. See
`docs/ARCHITECTURE.md` for the full design. Key components:

| Component | Role |
|---|---|
| Identity | 128-bit strongly typed FlowId, TenantId, WorkloadId, LinkId, PathId, ReservationId, AttemptId, WorkerId, WorkerBootId, CoordinatorEpoch, CapacityGeneration, FlowGeneration. Never reused across authority generations. |
| Resource | A governed bandwidth resource (PCIe, NVLink-class, host-memory, pinned-memory, storage read/write, inter-node TCP, generic transport) with stable identity, capacity state, generation fence, health/readiness. |
| Flow | A unit of competing work with a guarded lifecycle: Created, Queued, Reserved, Running, Throttled, Preempted, Completed, Cancelled, Failed. Invalid transitions fail deterministically. |
| Path | Ordered resource membership. Multi-hop allocation respects the bottleneck resource; flows contend only on shared resources; disjoint paths are independent. |
| Policy | Deterministic weighted water-fill arbitration across fairness, priority, latency class, deadline pressure, starvation age, minimum guarantees, caps, and bursts. |
| Reservation | A first-class authority object binding a flow to a resource set and allocated rates, with exactly-once release. |
| Authority | Coordinator owns authoritative flow/reservation state; workers register inventory and report progress/completion. Every mutation validates CoordinatorEpoch, WorkerBootId, AttemptId, FlowGeneration, CapacityGeneration. Stale authority is rejected deterministically. |
| Persistence | Versioned, checksummed binary state with strict recovery. Truncation, corruption, duplicate IDs, invalid enums, NaNs, and trailing garbage are rejected. |
| Transport | Framed TCP coordinator + worker processes. |

All public operations are thread-safe. No blocking network, persistence, backend,
or transfer execution is performed while the governor lock is held.

## Resource model

Every governed resource exposes stable identity, resource class, source,
destination, directionality, nominal capacity, measured capacity, governed
capacity, reserved capacity, allocated capacity, instantaneous utilization,
moving-average utilization, queue depth, saturation state, measured latency,
confidence, provenance, staleness, generation, health/readiness, and
enabled/disabled state. Capacity values are bounded and validated; negative,
impossible, NaN, Inf, and overflow are rejected.

## Flow model

Flows carry identity, tenant/workload/attempt, source/destination, path, byte
count, direction, priority, latency class, deadline, requested minimum /
preferred / maximum bandwidth, burst allowance, preemptibility, resumability,
admission/retry/accounting state, and authority metadata.

## Path model

Paths carry ordered resource membership with explicit identity and generation.
For multi-hop flows the effective allocation respects the bottleneck resource; no
allocation violates any governed resource; unrelated flows do not interfere.
Path capacity is recalculated when resource generations change; stale paths are
invalidated or rebuilt.

## Fairness and policy

Arbitration is explicit and deterministic. A weighted water-fill computes each
flow's grant within its [minimum, preferred, maximum] envelope, honoring minimum
guarantees and reservation pressure, applying priority, latency-class,
deadline-pressure and starvation weighting, and bounding the aggregate by the
bottleneck resource. There is no hidden overcommit. Admission defers or rejects
work when guarantees cannot be honored rather than pretending capacity exists.

## Reservations

Reservations are transactionally safe first-class authority objects. They bind a
flow to a resource set and allocated rates and are fenced by CoordinatorEpoch,
WorkerBootId, AttemptId, FlowGeneration, and CapacityGeneration. They are
released exactly once. Retry obtains a fresh AttemptId; a stale reservation report
cannot mutate current accounting; recovery cannot resurrect obsolete authority.

## Explainability

Every important admission, scheduling, throttling, preemption, rejection, and
allocation decision carries a structured explanation: requested/granted/minimum/
preferred/maximum rates, bottleneck resource, path, priority, latency class,
deadline pressure, tenant/workload fairness state, starvation age, burst state,
saturation state, competing flows, reservation pressure, capacity generation, and
the specific defer / reject / throttle / preempt reason.

## Failure recovery

Capacity observations can become stale. Freshness is modeled with measurement
timestamp, staleness threshold, confidence, provenance, and CapacityGeneration.
Stale measurements are never silently treated as fresh; when capacity drops below
committed allocation the governor deterministically retains the guarantee,
throttles, preempts, defers, or rejects new work - never creating hidden
overcommit.

## CUDA proof

On validated hardware (NVIDIA GeForce RTX 5090 / Blackwell sm_120, CUDA 13.1) the
runtime performs real bounded host-to-device transfers under governor control for
competing flows with distinct priority and caps and asserts observed movement
conforms to governed allocations. Payload integrity and resource cleanup are
verified. Unavailable fabric (multi-GPU links, RDMA, DPU, CXL, NVLink hardware)
is never fabricated; it is represented only through explicitly labeled synthetic
models.

## Testing

Tests cover identity, flow lifecycle, resource lifecycle, path modeling, bottleneck
calculation, capacity updates, admission, fairness, weighted fairness, starvation
prevention, priority, latency, deadlines, minimum guarantees, maximum caps, bursts,
throttling, backpressure, preemption, cancellation, retry, reservations, exactly-
once release, shared-path contention, disjoint-path independence, capacity-
generation fencing, stale authority, worker restart, persistence, recovery,
corruption/truncation rejection, malformed protocol frames, concurrency,
deterministic replay, accounting-to-zero, and the real CUDA transfer proof.
Fixed-seed property tests and high-contention concurrency tests are included.
**There are no test timeouts anywhere** - no CTest TIMEOUT properties, no watchdog
wrappers, no process time limits.

## Benchmarks

The benchmark suite reports completed operations for admission throughput,
allocation decision throughput, bottleneck computation, reservation create/release,
explanation generation, snapshot creation, capacity updates, N-thread scheduling,
large active-flow pools, shared-link contention, disjoint workloads, persistence,
recovery, and real bounded CUDA transfer governance where measurable.

## Build, install, use

```bash
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
ctest --test-dir build -C Release
bg demo fairness
bg_examples fairness
bg_bench
```

Install and consume as a downstream package:

```bash
cmake --install build --config Release --prefix <prefix>
cmake -S consumer -B consumer-build -DCMAKE_PREFIX_PATH=<prefix>
cmake --build consumer-build --config Release
```

See docs/BUILDING.md for details, docs/ARCHITECTURE.md for the design,
docs/EXAMPLES.md for scenarios, and docs/BENCHMARKS.md for methodology.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs.
