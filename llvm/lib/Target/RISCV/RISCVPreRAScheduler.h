//===-- RISCVPreRAScheduler.h - Pre-RA Scheduler for RISCV -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines a custom Pre-RA (Register Allocation) scheduler for RISCV
// subtargets. The scheduler implements a list scheduling algorithm with support
// for:
//   - Top-down scheduling (from beginning of block)
//   - Bottom-up scheduling (from end of block, prioritizing high latency)
//   - Bi-directional scheduling (converging from both directions)
//
// The scheduler considers multiple criteria when selecting instructions:
//   - Register Pressure: Number of live registers at any point
//   - Latency: Instruction execution latency
//   - Clustering: Grouping related instructions together
//   - Critical Resources: Resource conflicts and contentions
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RISCV_RISCVPRESCHEDULER_H
#define LLVM_LIB_TARGET_RISCV_RISCVPRESCHEDULER_H

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/RegisterPressure.h"
#include "llvm/CodeGen/ScheduleDAG.h"

namespace llvm {

/// Scheduling direction for Pre-RA scheduling
enum class RISCVSchedDirection {
  TopDown,      // Schedule from beginning of block
  BottomUp,     // Schedule from end, prioritize high latency instructions
  Bidirectional // Converging from both directions (default)
};

/// Configuration for the RISCV Pre-RA scheduler
struct RISCVPreRASchedConfig {
  RISCVSchedDirection Direction = RISCVSchedDirection::Bidirectional;

  // Weights for different scheduling criteria (0-100)
  unsigned RegisterPressureWeight = 30;
  unsigned LatencyWeight = 35;
  unsigned ClusteringWeight = 20;
  unsigned ResourceWeight = 15;

  // Enable/disable specific optimizations
  bool EnableClustering = true;
  bool EnableResourceBalance = true;
  bool AggressiveLatencyReduction = false;
};

/// RISCVPreRAScheduler - Custom Pre-RA scheduler that extends GenericScheduler
/// with RISCV-specific heuristics and configurable scheduling strategies.
///
/// This scheduler operates on Machine Instructions with virtual registers
/// before register allocation. It optimizes instruction order based on multiple
/// criteria while maintaining correctness through the ScheduleDAG dependency
/// graph.
class RISCVPreRAScheduler : public GenericScheduler {
public:
  RISCVPreRAScheduler(const MachineSchedContext *C,
                      const RISCVPreRASchedConfig &Config = {});

  ~RISCVPreRAScheduler() override = default;

  /// Initialize scheduling policy based on configuration
  void initPolicy(MachineBasicBlock::iterator Begin,
                  MachineBasicBlock::iterator End,
                  unsigned NumRegionInstrs) override;

  /// Initialize the scheduler with the DAG for a new scheduling region
  void initialize(ScheduleDAGMI *DAG) override;

  /// Called when entering a new basic block
  void enterMBB(MachineBasicBlock *MBB) override;

  /// Called when leaving the current basic block
  void leaveMBB() override;

  /// Called when entering a scheduling region
  void enterRegion() override;

  /// Called when leaving a scheduling region
  void leaveRegion() override;

  /// Dump the current scheduling policy
  void dumpPolicy() const override;

protected:
  /// Configuration for this scheduler instance
  RISCVPreRASchedConfig Config;

  /// Current machine basic block being scheduled
  MachineBasicBlock *CurMBB = nullptr;

  /// Iterator to the beginning of the current scheduling region
  MachineBasicBlock::iterator RegionBegin;

  /// Iterator to the end of the current scheduling region
  MachineBasicBlock::iterator RegionEnd;

  /// Statistics for the current region
  struct RegionStats {
    unsigned NumInstructions = 0;
    unsigned MaxRegPressure = 0;
    unsigned TotalLatency = 0;
    unsigned ResourceConflicts = 0;

    void reset() {
      NumInstructions = 0;
      MaxRegPressure = 0;
      TotalLatency = 0;
      ResourceConflicts = 0;
    }
  } Stats;

  /// Override tryCandidate to implement custom heuristics
  /// \param Cand - Current best candidate
  /// \param TryCand - New candidate to evaluate
  /// \param Zone - Scheduling boundary (Top or Bottom)
  /// \return true if TryCand is better than Cand
  bool tryCandidate(SchedCandidate &Cand, SchedCandidate &TryCand,
                    SchedBoundary *Zone) const override;

  /// Check if a scheduling unit is available to be scheduled
  /// \param SU - The scheduling unit to check
  /// \return true if SU can be scheduled now
  bool isAvailableNode(const SUnit *SU) const;

  /// Evaluate register pressure impact of scheduling a candidate
  /// \param Cand - Candidate to evaluate
  /// \param Zone - Scheduling boundary
  /// \return Pressure score (lower is better)
  int evaluateRegPressure(const SchedCandidate &Cand,
                          const SchedBoundary *Zone) const;

  /// Evaluate latency benefit of scheduling a candidate
  /// \param Cand - Candidate to evaluate
  /// \param Zone - Scheduling boundary
  /// \return Latency score (higher priority for high latency in bottom-up)
  int evaluateLatency(const SchedCandidate &Cand,
                      const SchedBoundary *Zone) const;

  /// Evaluate clustering benefit of scheduling a candidate
  /// \param Cand - Candidate to evaluate
  /// \return Clustering score (higher is better)
  int evaluateClustering(const SchedCandidate &Cand) const;

  /// Evaluate resource usage of scheduling a candidate
  /// \param Cand - Candidate to evaluate
  /// \param Zone - Scheduling boundary
  /// \return Resource score (lower indicates less contention)
  int evaluateResource(const SchedCandidate &Cand,
                       const SchedBoundary *Zone) const;

  /// Compute weighted score for a candidate based on all criteria
  /// \param Cand - Candidate to score
  /// \param Zone - Scheduling boundary
  /// \return Overall score for comparison
  int computeCandidateScore(const SchedCandidate &Cand,
                            const SchedBoundary *Zone) const;

  /// Apply direction-specific bias to candidate selection
  /// \param Cand - Candidate to evaluate
  /// \param TryCand - New candidate to compare
  /// \param Zone - Scheduling boundary
  /// \return true if direction-specific heuristic prefers TryCand
  bool applyDirectionBias(const SchedCandidate &Cand,
                          const SchedCandidate &TryCand,
                          const SchedBoundary *Zone) const;

  /// Update statistics after scheduling an instruction
  /// \param SU - Scheduling unit that was scheduled
  /// \param IsTopNode - Whether scheduled from top boundary
  void updateStatsAfterScheduling(const SUnit *SU, bool IsTopNode);
};

/// Factory function to create a ScheduleDAGMILive with RISCVPreRAScheduler
/// \param C - Machine scheduling context
/// \param Config - Scheduler configuration
/// \return Configured ScheduleDAGMILive instance
ScheduleDAGMILive *
createRISCVPreRAScheduler(MachineSchedContext *C,
                          const RISCVPreRASchedConfig &Config = {});

} // end namespace llvm

#endif // LLVM_LIB_TARGET_RISCV_RISCVPRESCHEDULER_H
