# RISC-V Pre-RA Scheduler

## Overview

This document describes the custom Pre-Register Allocation (Pre-RA) scheduler implementation for RISC-V subtargets in LLVM. The scheduler operates on Machine Instructions with virtual registers before register allocation, optimizing instruction order based on multiple criteria while maintaining correctness through dependency analysis.

## Architecture

### Design Process

The backend compiler follows this design process:
1. **IR Code** - LLVM Intermediate Representation
2. **Instruction Selection** - Translate IR to DAG nodes and Machine Instructions
3. **Pre-RA Scheduling** ← **This implementation**
4. **Register Allocation** - Assign physical registers
5. **Post-RA Scheduling** - Final instruction scheduling
6. **Bundling/Finalization** - Bundle instructions for VLIW if applicable
7. **Machine Instruction Emission** - Generate final assembly

### Class Hierarchy

```
MachineSchedStrategy (interface)
  ↓
GenericSchedulerBase
  ↓
GenericScheduler
  ↓
RISCVPreRAScheduler (custom implementation)
```

## Key Features

### 1. Scheduling Directions

The scheduler supports three scheduling approaches:

#### **Top-Down Scheduling**
- Schedules instructions from the beginning of the basic block
- Prioritizes instructions with more ready successors
- Useful for reducing register pressure early
- Enable with: `-riscv-sched-direction=topdown`

#### **Bottom-Up Scheduling**
- Schedules instructions from the end of the basic block
- Prioritizes instructions with highest latency
- Helps hide execution latency
- Enable with: `-riscv-sched-direction=bottomup`

#### **Bi-Directional Scheduling (Default)**
- Combines both top-down and bottom-up approaches
- Converges from both ends toward the middle
- Balances benefits of both strategies
- Enable with: `-riscv-sched-direction=bidirectional`

### 2. Scheduling Criteria

The scheduler evaluates candidates based on four weighted criteria:

#### **Register Pressure (Default Weight: 30%)**
- Tracks the number of live virtual registers
- Minimizes register pressure to reduce spills
- Considers register pressure deltas for each candidate
- Adjust with: `-riscv-sched-regpressure-weight=<0-100>`

#### **Latency (Default Weight: 35%)**
- Considers instruction execution latency
- Prioritizes critical path instructions
- For bottom-up: schedules high-latency instructions first
- For top-down: schedules instructions closer to exit first
- Adjust with: `-riscv-sched-latency-weight=<0-100>`

#### **Clustering (Default Weight: 20%)**
- Groups related instructions together
- Enables downstream peephole optimizations
- Clusters memory operations (loads/stores)
- Helps with instruction fusion opportunities
- Adjust with: `-riscv-sched-clustering-weight=<0-100>`

#### **Critical Resources (Default Weight: 15%)**
- Balances resource usage across execution units
- Avoids resource conflicts and contentions
- Minimizes pipeline stalls
- Considers resource availability for each candidate
- Adjust with: `-riscv-sched-resource-weight=<0-100>`

## Usage

### Enabling the Scheduler

To enable the custom Pre-RA scheduler, use the following command-line option:

```bash
llc -mtriple=riscv64 -riscv-enable-prera-scheduler input.ll -o output.s
```

### Configuration Examples

#### Example 1: Aggressive Latency Optimization
```bash
llc -mtriple=riscv64 \
    -riscv-enable-prera-scheduler \
    -riscv-sched-direction=bottomup \
    -riscv-sched-latency-weight=60 \
    -riscv-sched-regpressure-weight=20 \
    -riscv-sched-clustering-weight=15 \
    -riscv-sched-resource-weight=5 \
    input.ll -o output.s
```

#### Example 2: Register Pressure Minimization
```bash
llc -mtriple=riscv64 \
    -riscv-enable-prera-scheduler \
    -riscv-sched-direction=topdown \
    -riscv-sched-regpressure-weight=50 \
    -riscv-sched-latency-weight=25 \
    -riscv-sched-clustering-weight=15 \
    -riscv-sched-resource-weight=10 \
    input.ll -o output.s
```

#### Example 3: Balanced Bi-Directional (Default)
```bash
llc -mtriple=riscv64 \
    -riscv-enable-prera-scheduler \
    -riscv-sched-direction=bidirectional \
    input.ll -o output.s
```

### Command-Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `-riscv-enable-prera-scheduler` | Enable custom Pre-RA scheduler | `false` |
| `-riscv-sched-direction` | Scheduling direction: `topdown`, `bottomup`, or `bidirectional` | `bidirectional` |
| `-riscv-sched-regpressure-weight` | Weight for register pressure (0-100) | `30` |
| `-riscv-sched-latency-weight` | Weight for latency (0-100) | `35` |
| `-riscv-sched-clustering-weight` | Weight for instruction clustering (0-100) | `20` |
| `-riscv-sched-resource-weight` | Weight for resource usage (0-100) | `15` |

## Implementation Details

### Key Classes and Methods

#### **RISCVPreRAScheduler**

Main scheduler class that extends `GenericScheduler`.

**Key Methods:**
- `initPolicy()` - Initialize scheduling policy based on configuration
- `initialize()` - Initialize DAG for a new scheduling region
- `enterRegion()` - Called when entering a scheduling region
- `leaveRegion()` - Write results and clean up after region is scheduled
- `tryCandidate()` - Evaluate and compare scheduling candidates
- `isAvailableNode()` - Check if a node is available to schedule

**Evaluation Methods:**
- `evaluateRegPressure()` - Score candidate based on register pressure impact
- `evaluateLatency()` - Score candidate based on latency considerations
- `evaluateClustering()` - Score candidate based on clustering opportunities
- `evaluateResource()` - Score candidate based on resource usage
- `computeCandidateScore()` - Compute weighted total score for a candidate

### Data Structures

#### **RISCVPreRASchedConfig**
```cpp
struct RISCVPreRASchedConfig {
  RISCVSchedDirection Direction;
  unsigned RegisterPressureWeight;
  unsigned LatencyWeight;
  unsigned ClusteringWeight;
  unsigned ResourceWeight;
  bool EnableClustering;
  bool EnableResourceBalance;
  bool AggressiveLatencyReduction;
};
```

#### **SchedCandidate**
Used to track and compare scheduling candidates:
- `SU` - Scheduling unit (instruction)
- `Reason` - Why this candidate was chosen
- `AtTop` - Whether scheduled from top boundary
- `RPDelta` - Register pressure delta
- `ResDelta` - Resource consumption delta

### Scheduling Algorithm

The scheduler uses a list scheduling algorithm:

1. **DAG Construction**: Build dependency graph for the region
2. **Initialization**: Initialize ready queues (top and/or bottom)
3. **Candidate Selection Loop**:
   ```
   while (unscheduled instructions remain):
     - Pick available candidates from ready queue(s)
     - Evaluate each candidate using tryCandidate()
     - Select best candidate based on weighted criteria
     - Schedule the selected instruction
     - Update ready queues and dependencies
     - Update register pressure and resource tracking
   ```
4. **Finalization**: Write scheduled instructions back to the block

### Integration with LLVM

The scheduler integrates with LLVM's MachineScheduler pass through:

1. **Factory Function**: `createRISCVPreRAScheduler()`
2. **TargetPassConfig**: Override `createMachineScheduler()` in `RISCVPassConfig`
3. **DAG Mutations**: Standard mutations for copy constraints and clustering
4. **Register Pressure Tracking**: Uses `ScheduleDAGMILive` for live interval tracking

## Performance Considerations

### When to Use Each Direction

**Top-Down:**
- Code with high register pressure
- Large basic blocks with many virtual registers
- When spilling is a concern

**Bottom-Up:**
- Code with long latency operations (e.g., loads, divides)
- When hiding memory latency is critical
- Pipeline-sensitive code

**Bi-Directional (Default):**
- General-purpose applications
- Balanced workloads
- When unsure which strategy is best

### Tuning Guidelines

1. **Start with defaults** - The default weights are balanced for general code
2. **Profile your workload** - Identify bottlenecks (spills vs. stalls)
3. **Adjust weights gradually** - Make 10-20 point changes at a time
4. **Measure performance** - Use benchmarks to validate improvements
5. **Consider code characteristics**:
   - Memory-bound: Increase latency weight
   - Compute-bound: Increase resource weight
   - Large functions: Increase register pressure weight

## Debugging

### Enable Debug Output

```bash
llc -mtriple=riscv64 \
    -riscv-enable-prera-scheduler \
    -debug-only=riscv-pre-ra-sched \
    input.ll -o output.s 2> debug.log
```

### Visualize Scheduling DAG

```bash
llc -mtriple=riscv64 \
    -riscv-enable-prera-scheduler \
    -view-misched-dags \
    input.ll -o output.s
```

## Future Enhancements

Potential areas for future improvement:

1. **Machine Learning Integration**: Train weights based on profiling data
2. **Loop-Aware Scheduling**: Special handling for loop bodies
3. **VLIW Bundling**: Direct integration with bundling pass
4. **Custom Resource Models**: Target-specific resource modeling
5. **Prefetching Hints**: Insert prefetch instructions proactively
6. **Profile-Guided Scheduling**: Use PGO data to guide decisions

## References

- LLVM MachineScheduler Documentation
- "List Scheduling Algorithms for Instruction-Level Parallelism"
- RISC-V ISA Specification
- LLVM CodeGen Documentation

## Contact

For questions or issues with the scheduler, please contact the LLVM RISC-V backend maintainers.
