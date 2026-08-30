# Architecture

Bandwidth Governor is a C++20 systems runtime. This document describes the design
boundaries, the authoritative state machine, and the correctness model.

## Layers

1. **Identity & value types** - 128-bit strongly typed identities and validated
   Capacity values (bounded, no NaN/Inf/negative).
2. **Resource & path model** - governed resource inventory and ordered path
   membership with generation fencing.
3. **Flow lifecycle** - a guarded state machine with explicit, deterministic
   transitions.
4. **Policy engine** - weighted water-fill arbitration producing a per-flow grant
   within [min, preferred, max] against each bottleneck resource.
5. **Reservation authority** - exactly-once, generation-fenced capacity
   commitments.
6. **Coordinator** - thread-safe authority owning all flow/reservation state.
7. **Persistence** - versioned, checksummed binary snapshots with strict recovery.
8. **Transport** - framed TCP coordinator + worker processes.

## The coordinator is the single source of truth

The coordinator (Governor) owns authoritative flow and reservation state. Workers
register a backend inventory and drive transfers at the governed rate; they report
progress/completion and are never trusted with authoritative state. Every report
is validated against CoordinatorEpoch, WorkerBootId, AttemptId, FlowGeneration,
and CapacityGeneration fences before any mutation is applied.

## Allocation

For each governed resource, the coordinator gathers the flows whose paths traverse
that resource and computes a weighted water-fill of the resource's governed
capacity. Each flow's grant is the minimum of its per-hop grants, so no allocation
violates any resource on its path. Flows on disjoint paths never contend.

The composite arbitration weight combines priority (base^priority), latency class,
deadline pressure, starvation age, burst state, and tenant fairness (relative to
each tenant's ideal share of served bytes).

## Capacity freshness

Measured capacity carries a timestamp and a staleness threshold. Governed capacity
is discounted by freshness; a stale or unhealthy resource is never treated as if it
still has full fresh capacity. Capacity changes bump CapacityGeneration, which
fences existing reservations.

## Correctness guarantees

- No double allocation: a reservation's capacity is committed exactly once.
- No double release: reservation release is idempotent at the semantic level and
  terminal at the state level.
- No leaked reservations: cancellation, completion, failed dispatch, and worker
  loss each release correctly.
- No hidden overcommit: admission defers or rejects when guarantees cannot be
  honored; the allocator never grants beyond a resource's governed capacity.
- Stale authority never mutates current state: epoch/boot/attempt/generation
  mismatches are rejected before any mutation.
- Accounting returns exactly to zero after all flows settle.

## Concurrency model

The coordinator serializes all state mutations under a single mutex and takes
immutable snapshots for reads. No blocking network, persistence, backend, or
transfer execution is performed while the lock is held. The scheduler, connection
threads, and workers are separate threads that only briefly take the governor lock.

## Limitation / hardware boundary

The runtime is validated against physically available hardware. On the RTX 5090 /
CUDA 13.1 system, real bounded host-to-device transfers are exercised under
governor control. Unavailable fabric (multi-GPU NVLink hardware, RDMA, DPU, CXL) is
represented only through explicitly labelled synthetic models and is never
fabricated or claimed as validated.
