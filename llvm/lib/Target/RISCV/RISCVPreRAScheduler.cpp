//===-- RISCVPreRAScheduler.cpp - Pre-RA Scheduler for RISCV -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a custom Pre-RA scheduler for RISCV subtargets.
// The scheduler uses a list scheduling algorithm with configurable direction
// (top-down, bottom-up, or bi-directional) and considers multiple criteria
// including register pressure, latency, clustering, and resource usage.
//
//===----------------------------------------------------------------------===//

#include "RISCVPreRAScheduler.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/RegisterPressure.h"
#include "llvm/CodeGen/ScheduleDAG.h"
#include "llvm/CodeGen/ScheduleDAGInstrs.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSchedule.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "riscv-pre-ra-sched"

using namespace llvm;

//===----------------------------------------------------------------------===//
// RISCVPreRAScheduler Implementation
//===----------------------------------------------------------------------===//

RISCVPreRAScheduler::RISCVPreRAScheduler(const MachineSchedContext *C,
                                         const RISCVPreRASchedConfig &Config)
    : GenericScheduler(C), Config(Config) {
  LLVM_DEBUG(dbgs() << "Creating RISCVPreRAScheduler\n");
}

void RISCVPreRAScheduler::initPolicy(MachineBasicBlock::iterator Begin,
                                     MachineBasicBlock::iterator End,
                                     unsigned NumRegionInstrs) {
  LLVM_DEBUG(dbgs() << "RISCVPreRAScheduler::initPolicy - "
                    << NumRegionInstrs << " instructions\n");

  // Save region bounds
  RegionBegin = Begin;
  RegionEnd = End;

  // Call parent class to initialize base policy
  GenericScheduler::initPolicy(Begin, End, NumRegionInstrs);

  // Configure scheduling direction based on config
  switch (Config.Direction) {
  case RISCVSchedDirection::TopDown:
    LLVM_DEBUG(dbgs() << "  Direction: Top-Down\n");
    RegionPolicy.OnlyTopDown = true;
    RegionPolicy.OnlyBottomUp = false;
    break;

  case RISCVSchedDirection::BottomUp:
    LLVM_DEBUG(dbgs() << "  Direction: Bottom-Up\n");
    RegionPolicy.OnlyTopDown = false;
    RegionPolicy.OnlyBottomUp = true;
    break;

  case RISCVSchedDirection::Bidirectional:
    LLVM_DEBUG(dbgs() << "  Direction: Bi-directional\n");
    RegionPolicy.OnlyTopDown = false;
    RegionPolicy.OnlyBottomUp = false;
    break;
  }

  // Enable register pressure tracking for better scheduling decisions
  RegionPolicy.ShouldTrackPressure = true;
  RegionPolicy.ShouldTrackLaneMasks = true;

  // Configure latency heuristic based on config
  RegionPolicy.DisableLatencyHeuristic = !Config.AggressiveLatencyReduction;

  // Reset statistics for this region
  Stats.reset();
  Stats.NumInstructions = NumRegionInstrs;
}

void RISCVPreRAScheduler::initialize(ScheduleDAGMI *dag) {
  LLVM_DEBUG(dbgs() << "RISCVPreRAScheduler::initialize\n");

  // Call parent class initialization
  GenericScheduler::initialize(dag);

  // Additional initialization specific to RISCV Pre-RA scheduling
  if (DAG && DAG->hasVRegLiveness()) {
    LLVM_DEBUG(dbgs() << "  VReg liveness tracking enabled\n");
  }
}

void RISCVPreRAScheduler::enterMBB(MachineBasicBlock *MBB) {
  LLVM_DEBUG(dbgs() << "RISCVPreRAScheduler::enterMBB - "
                    << printMBBReference(*MBB) << "\n");
  CurMBB = MBB;
  GenericScheduler::enterMBB(MBB);
}

void RISCVPreRAScheduler::leaveMBB() {
  LLVM_DEBUG(dbgs() << "RISCVPreRAScheduler::leaveMBB\n");

  // Print statistics for the basic block
  LLVM_DEBUG({
    dbgs() << "  Statistics for MBB:\n";
    dbgs() << "  Instructions scheduled: " << Stats.NumInstructions << "\n";
    dbgs() << "  Max register pressure: " << Stats.MaxRegPressure << "\n";
    dbgs() << "  Total latency: " << Stats.TotalLatency << "\n";
    dbgs() << "  Resource conflicts: " << Stats.ResourceConflicts << "\n";
  });

  GenericScheduler::leaveMBB();
  CurMBB = nullptr;
}

void RISCVPreRAScheduler::dumpPolicy() const {
  dbgs() << "RISCVPreRAScheduler Policy:\n";
  dbgs() << "  Direction: ";
  switch (Config.Direction) {
  case RISCVSchedDirection::TopDown:
    dbgs() << "Top-Down\n";
    break;
  case RISCVSchedDirection::BottomUp:
    dbgs() << "Bottom-Up\n";
    break;
  case RISCVSchedDirection::Bidirectional:
    dbgs() << "Bi-directional\n";
    break;
  }
  dbgs() << "  Weights:\n";
  dbgs() << "    Register Pressure: " << Config.RegisterPressureWeight << "\n";
  dbgs() << "    Latency: " << Config.LatencyWeight << "\n";
  dbgs() << "    Clustering: " << Config.ClusteringWeight << "\n";
  dbgs() << "    Resource: " << Config.ResourceWeight << "\n";
  dbgs() << "  Features:\n";
  dbgs() << "    Clustering: " << (Config.EnableClustering ? "Yes" : "No")
         << "\n";
  dbgs() << "    Resource Balance: "
         << (Config.EnableResourceBalance ? "Yes" : "No") << "\n";
  dbgs() << "    Aggressive Latency: "
         << (Config.AggressiveLatencyReduction ? "Yes" : "No") << "\n";

  GenericScheduler::dumpPolicy();
}

bool RISCVPreRAScheduler::tryCandidate(SchedCandidate &Cand,
                                       SchedCandidate &TryCand,
                                       SchedBoundary *Zone) const {
  // First, apply parent class heuristics
  // This handles basic correctness and many standard optimizations
  bool ParentPrefers = GenericScheduler::tryCandidate(Cand, TryCand, Zone);

  // If parent class strongly prefers one candidate (due to correctness or
  // critical heuristics), respect that decision
  if (ParentPrefers && TryCand.Reason != NodeOrder) {
    LLVM_DEBUG({
      dbgs() << "  Parent class prefers TryCand (Reason: ";
      dbgs() << getReasonStr(TryCand.Reason) << ")\n";
    });
    return true;
  }

  // If we don't have a valid candidate yet, accept the new one
  if (!Cand.isValid()) {
    TryCand.Reason = NodeOrder;
    return true;
  }

  // Apply RISCV-specific Pre-RA scheduling heuristics
  // These heuristics are applied in addition to the parent class heuristics

  // Direction-specific bias
  if (Config.Direction != RISCVSchedDirection::Bidirectional && Zone) {
    if (applyDirectionBias(Cand, TryCand, Zone)) {
      LLVM_DEBUG(dbgs() << "  Direction bias prefers TryCand\n");
      return true;
    }
  }

  // Evaluate candidates using weighted scoring
  int CandScore = computeCandidateScore(Cand, Zone);
  int TryCandScore = computeCandidateScore(TryCand, Zone);

  LLVM_DEBUG({
    dbgs() << "  Candidate scores: Cand=" << CandScore
           << " TryCand=" << TryCandScore << "\n";
  });

  // Prefer candidate with better (higher) score
  if (TryCandScore > CandScore) {
    TryCand.Reason = NodeOrder; // Use NodeOrder as generic "custom heuristic"
    return true;
  }

  // If scores are equal, fall back to parent class decision
  return ParentPrefers;
}

bool RISCVPreRAScheduler::isAvailableNode(const SUnit *SU) const {
  if (!SU || SU->isScheduled)
    return false;

  // Check if all predecessors are scheduled (for top-down)
  // or all successors are scheduled (for bottom-up)
  // This is typically handled by the ScheduleDAG infrastructure,
  // but we can add additional checks here if needed

  return true;
}

int RISCVPreRAScheduler::evaluateRegPressure(const SchedCandidate &Cand,
                                              const SchedBoundary *Zone) const {
  if (!DAG || !DAG->isTrackingPressure())
    return 0;

  // Lower score is better for register pressure
  // Calculate pressure increase caused by this candidate
  int PressureScore = 0;

  // Penalize candidates that increase register pressure
  if (Cand.RPDelta.Excess.isValid()) {
    PressureScore -= Cand.RPDelta.Excess.getUnitInc() * 100;
  }

  if (Cand.RPDelta.CriticalMax.isValid()) {
    PressureScore -= Cand.RPDelta.CriticalMax.getUnitInc() * 50;
  }

  if (Cand.RPDelta.CurrentMax.isValid()) {
    PressureScore -= Cand.RPDelta.CurrentMax.getUnitInc() * 25;
  }

  LLVM_DEBUG(dbgs() << "    RegPressure score: " << PressureScore << "\n");
  return PressureScore;
}

int RISCVPreRAScheduler::evaluateLatency(const SchedCandidate &Cand,
                                         const SchedBoundary *Zone) const {
  if (!Cand.SU || !Zone)
    return 0;

  int LatencyScore = 0;

  // For bottom-up scheduling, prioritize high-latency instructions
  // For top-down scheduling, prioritize instructions on the critical path
  if (Zone->isTop()) {
    // Top-down: prefer instructions with larger depth (closer to exit)
    LatencyScore = Cand.SU->getDepth();
  } else {
    // Bottom-up: prefer instructions with larger height (longer latency chains)
    LatencyScore = Cand.SU->getHeight();
  }

  // Add instruction latency as additional factor
  LatencyScore += Cand.SU->Latency;

  LLVM_DEBUG(dbgs() << "    Latency score: " << LatencyScore << "\n");
  return LatencyScore;
}

int RISCVPreRAScheduler::evaluateClustering(const SchedCandidate &Cand) const {
  if (!Config.EnableClustering || !DAG)
    return 0;

  int ClusterScore = 0;

  // Reward candidates that form clusters with already scheduled instructions
  // Check if this instruction is part of a cluster
  if (Cand.SU && Cand.SU->getInstr()) {
    const MachineInstr *MI = Cand.SU->getInstr();

    // Bonus for memory operations which often benefit from clustering
    if (MI->mayLoad() || MI->mayStore()) {
      ClusterScore += 30;
    }

    // Check if instruction has successors/predecessors in same cluster
    // (simplified heuristic based on dependency chains)
    int ClusteredDeps = 0;
    for (const SDep &Pred : Cand.SU->Preds) {
      if (Pred.getSUnit()->isScheduled) {
        ClusteredDeps++;
      }
    }

    // Reward instructions with recently scheduled dependencies
    if (ClusteredDeps > 0) {
      ClusterScore += ClusteredDeps * 10;
    }
  }

  LLVM_DEBUG(dbgs() << "    Clustering score: " << ClusterScore << "\n");
  return ClusterScore;
}

int RISCVPreRAScheduler::evaluateResource(const SchedCandidate &Cand,
                                          const SchedBoundary *Zone) const {
  if (!Config.EnableResourceBalance)
    return 0;

  int ResourceScore = 0;

  // Lower score for candidates that consume critical resources
  // Higher score for candidates that use underutilized resources

  // Check resource delta
  if (Cand.ResDelta.CritResources > 0) {
    ResourceScore -= Cand.ResDelta.CritResources * 30;
  }

  // Reward candidates that use demanded resources (helps balance)
  if (Cand.ResDelta.DemandedResources > 0) {
    ResourceScore += Cand.ResDelta.DemandedResources * 20;
  }

  // Note: getLatencyStallCycles is not const, so we cannot call it from
  // a const method. In a production implementation, you would need to
  // restructure the code to allow mutable access to the SchedBoundary.

  LLVM_DEBUG(dbgs() << "    Resource score: " << ResourceScore << "\n");
  return ResourceScore;
}

int RISCVPreRAScheduler::computeCandidateScore(const SchedCandidate &Cand,
                                               const SchedBoundary *Zone) const {
  if (!Cand.SU)
    return 0;

  LLVM_DEBUG(dbgs() << "  Computing score for SU(" << Cand.SU->NodeNum
                    << "):\n");

  // Evaluate each criterion
  int RegPressureScore = evaluateRegPressure(Cand, Zone);
  int LatencyScore = evaluateLatency(Cand, Zone);
  int ClusterScore = evaluateClustering(Cand);
  int ResourceScore = evaluateResource(Cand, Zone);

  // Apply weights to compute final score
  int TotalScore = 0;
  TotalScore += (RegPressureScore * Config.RegisterPressureWeight) / 100;
  TotalScore += (LatencyScore * Config.LatencyWeight) / 100;
  TotalScore += (ClusterScore * Config.ClusteringWeight) / 100;
  TotalScore += (ResourceScore * Config.ResourceWeight) / 100;

  LLVM_DEBUG(dbgs() << "    Total weighted score: " << TotalScore << "\n");

  return TotalScore;
}

bool RISCVPreRAScheduler::applyDirectionBias(
    const SchedCandidate &Cand, const SchedCandidate &TryCand,
    const SchedBoundary *Zone) const {

  if (!Zone)
    return false;

  // For top-down only scheduling, prefer instructions with more ready
  // successors to maximize scheduling freedom
  if (Config.Direction == RISCVSchedDirection::TopDown && Zone->isTop()) {
    // Count ready successors for each candidate
    unsigned CandReadySuccs = 0;
    unsigned TryCandReadySuccs = 0;

    for (const SDep &Succ : Cand.SU->Succs) {
      if (!Succ.getSUnit()->isScheduled)
        CandReadySuccs++;
    }

    for (const SDep &Succ : TryCand.SU->Succs) {
      if (!Succ.getSUnit()->isScheduled)
        TryCandReadySuccs++;
    }

    if (TryCandReadySuccs > CandReadySuccs) {
      return true; // TryCand provides more scheduling freedom
    }
  }

  // For bottom-up only scheduling, prefer instructions with high latency
  // to hide execution latency
  if (Config.Direction == RISCVSchedDirection::BottomUp && !Zone->isTop()) {
    if (TryCand.SU->Latency > Cand.SU->Latency) {
      return true; // TryCand has higher latency
    }
  }

  return false;
}

void RISCVPreRAScheduler::updateStatsAfterScheduling(const SUnit *SU,
                                                     bool IsTopNode) {
  if (!SU)
    return;

  // Update latency statistics
  Stats.TotalLatency += SU->Latency;

  // Update register pressure statistics
  if (DAG && DAG->isTrackingPressure()) {
    const auto &Pressure =
        IsTopNode ? DAG->getTopPressure() : DAG->getBotPressure();

    for (unsigned i = 0, e = Pressure.MaxSetPressure.size(); i < e; ++i) {
      if (Pressure.MaxSetPressure[i] > Stats.MaxRegPressure) {
        Stats.MaxRegPressure = Pressure.MaxSetPressure[i];
      }
    }
  }

  LLVM_DEBUG(dbgs() << "  Updated stats after scheduling SU("
                    << SU->NodeNum << ")\n");
}

//===----------------------------------------------------------------------===//
// Factory Function
//===----------------------------------------------------------------------===//

ScheduleDAGMILive *
llvm::createRISCVPreRAScheduler(MachineSchedContext *C,
                                const RISCVPreRASchedConfig &Config) {
  LLVM_DEBUG(dbgs() << "Creating ScheduleDAGMILive with RISCVPreRAScheduler\n");

  // Create the ScheduleDAGMILive with our custom scheduler strategy
  ScheduleDAGMILive *DAG = new ScheduleDAGMILive(
      C, std::make_unique<RISCVPreRAScheduler>(C, Config));

  // Add standard DAG mutations for copy constraints
  // These help maintain correctness and enable optimizations
  DAG->addMutation(createCopyConstrainDAGMutation(DAG->TII, DAG->TRI));

  // Optionally add load/store clustering mutations if enabled
  if (Config.EnableClustering) {
    DAG->addMutation(createLoadClusterDAGMutation(DAG->TII, DAG->TRI));
    DAG->addMutation(createStoreClusterDAGMutation(DAG->TII, DAG->TRI));
  }

  return DAG;
}
