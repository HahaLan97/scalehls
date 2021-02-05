//===----------------------------------------------------------------------===//
//
// Copyright 2020-2021 The ScaleHLS Authors.
//
//===----------------------------------------------------------------------===//

#include "scalehls/Analysis/QoREstimation.h"
#include "mlir/Analysis/LoopAnalysis.h"
#include "mlir/Analysis/Utils.h"
#include "mlir/Dialect/Affine/IR/AffineValueMap.h"
#include "scalehls/Analysis/Passes.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace std;
using namespace mlir;
using namespace scalehls;

//===----------------------------------------------------------------------===//
// LoadOp and StoreOp Related Methods
//===----------------------------------------------------------------------===//

/// Calculate the overall partition index.
void HLSCppEstimator::getPartitionIndices(Operation *op) {
  auto access = MemRefAccess(op);
  auto memrefType = access.memref.getType().cast<MemRefType>();

  // If the layout map does not exist, it means the memory is not partitioned.
  auto layoutMap = getLayoutMap(memrefType);
  if (!layoutMap) {
    auto partitionIndices = SmallVector<int64_t, 8>(memrefType.getRank(), 0);
    setAttrValue(op, "partition_indices", partitionIndices);
    return;
  }

  AffineValueMap accessMap;
  access.getAccessMap(&accessMap);

  // Replace all dims in the memory access AffineMap with (step * dims). This
  // will ensure the "cyclic" array partition can be correctly detected.
  SmallVector<AffineExpr, 4> dimReplacements;
  SmallVector<AffineExpr, 4> symReplacements;

  unsigned operandIdx = 0;
  for (auto operand : accessMap.getOperands()) {
    if (operandIdx < accessMap.getNumDims()) {
      int64_t step = 1;
      if (isForInductionVar(operand))
        step = getForInductionVarOwner(operand).getStep();

      dimReplacements.push_back(step * builder.getAffineDimExpr(operandIdx));
    } else {
      symReplacements.push_back(
          builder.getAffineSymbolExpr(operandIdx - accessMap.getNumDims()));
    }
    operandIdx++;
  }

  auto newMap = accessMap.getAffineMap().replaceDimsAndSymbols(
      dimReplacements, symReplacements, accessMap.getNumDims(),
      accessMap.getNumSymbols());

  // Compose the access map with the layout map.
  auto composeMap = layoutMap.compose(newMap);

  // Collect partition factors.
  SmallVector<int64_t, 8> factors;
  getPartitionFactors(memrefType, &factors);

  // Calculate the partition index of this load/store operation honoring the
  // partition strategy applied.
  SmallVector<int64_t, 8> partitionIndices;
  int64_t maxMuxSize = 1;
  bool hasUncertainIdx = false;

  for (int64_t dim = 0; dim < memrefType.getRank(); ++dim) {
    auto idxExpr = composeMap.getResult(dim);

    if (auto constExpr = idxExpr.dyn_cast<AffineConstantExpr>())
      partitionIndices.push_back(constExpr.getValue());
    else {
      partitionIndices.push_back(-1);
      maxMuxSize = max(maxMuxSize, factors[dim]);
      hasUncertainIdx = true;
    }
  }

  setAttrValue(op, "partition_indices", partitionIndices);
  if (hasUncertainIdx)
    setAttrValue(op, "max_mux_size", maxMuxSize);
}

/// Schedule load/store operation honoring the memory ports number limitation.
void HLSCppEstimator::estimateLoadStore(Operation *op, int64_t begin) {
  auto access = MemRefAccess(op);
  auto memref = access.memref;
  auto memrefType = memref.getType().cast<MemRefType>();

  SmallVector<int64_t, 8> factors;
  auto partitionNum = getPartitionFactors(memrefType, &factors);
  auto storageType = MemoryKind(memrefType.getMemorySpace());
  auto partitionIndices = getIntArrayAttrValue(op, "partition_indices");

  // Try to avoid memory port violation until a legal schedule is found. Since
  // an infinite length schedule cannot be generated, this while loop can be
  // proofed to have an end.
  int64_t resMinII = 1;
  for (;; ++begin) {
    auto &memPortInfos = memPortInfosMap[begin][memref];

    // If the memory has not been occupied by the current schedule level, it
    // should be initialized according to its storage type. Note that each
    // partition should have one PortInfo structure, where the default case is
    // BRAM_S2P.
    if (memPortInfos.empty())
      for (unsigned p = 0; p < partitionNum; ++p) {
        MemPortInfo info;

        if (storageType == MemoryKind::BRAM_1P)
          info.rdwrPort = 1;
        else if (storageType == MemoryKind::BRAM_T2P)
          info.rdwrPort = 2;
        else
          info.rdPort = 1, info.wrPort = 1;

        memPortInfos.push_back(info);
      }

    // Indicate whether the memory access operation is successfully scheduled in
    // the current schedule level.
    bool successFlag = true;

    // Walk through all partitions to check whether the current partition is
    // occupied and whether available memory ports are enough to schedule the
    // current memory access operation.
    for (int64_t idx = 0; idx < partitionNum; ++idx) {
      bool isOccupied = true;
      int64_t accumFactor = 1;

      for (int64_t dim = 0; dim < memrefType.getRank(); ++dim) {
        // If the index is -1, all ports in the current partition will be
        // occupied and a multiplexer will be generated in HLS.
        if (partitionIndices[dim] != -1 &&
            idx / accumFactor % factors[dim] != partitionIndices[dim]) {
          isOccupied = false;
          break;
        }
        accumFactor *= factors[dim];
      }

      if (isOccupied) {
        auto &info = memPortInfos[idx];
        if (isa<AffineReadOpInterface>(op)) {
          bool hasIdenticalAccess = false;
          // The rationale is as long as the current read operation has
          // identical memory access information with any scheduled read
          // operation, the schedule will success.
          for (auto rdAccess : info.rdAccesses)
            if (access == rdAccess)
              hasIdenticalAccess = true;

          if (hasIdenticalAccess)
            continue;

          if (info.rdPort > 0) {
            info.rdPort--;
            info.rdAccesses.push_back(access);
          } else if (info.rdwrPort > 0) {
            info.rdwrPort--;
            info.rdAccesses.push_back(access);
          } else {
            successFlag = false;
            break;
          }
        } else if (isa<AffineWriteOpInterface>(op)) {
          if (info.wrPort > 0)
            info.wrPort--;
          else if (info.rdwrPort > 0)
            info.rdwrPort--;
          else {
            successFlag = false;
            break;
          }
        }
      }
    }

    if (successFlag)
      break;
    resMinII++;
  }

  if (isa<AffineReadOpInterface>(op))
    setScheduleValue(op, begin, begin + 2);
  else
    setScheduleValue(op, begin, begin + 1);
}

//===----------------------------------------------------------------------===//
// AffineForOp Related Methods
//===----------------------------------------------------------------------===//

int64_t HLSCppEstimator::getResMinII(int64_t begin, int64_t end,
                                     MemAccessesMap &map) {
  int64_t II = 1;
  for (auto &pair : map) {
    auto memref = pair.first;
    auto memrefType = memref.getType().cast<MemRefType>();
    auto partitionNum = getPartitionFactors(memrefType);
    auto storageType = MemoryKind(memrefType.getMemorySpace());

    auto accessNum = SmallVector<int64_t, 16>(partitionNum, 0);
    // Prepare for BRAM_S1P memory kind.
    auto writeNum = SmallVector<int64_t, 16>(partitionNum, 0);

    // TODO: fine-tune for BRAM_T2P.
    for (int64_t level = begin; level < end; ++level)
      if (memPortInfosMap.count(level))
        if (memPortInfosMap[level].count(memref)) {
          auto &memPortInfos = memPortInfosMap[level][memref];

          for (int64_t idx = 0; idx < partitionNum; ++idx) {
            auto &info = memPortInfos[idx];
            if (storageType == MemoryKind::BRAM_1P && info.rdwrPort < 1)
              accessNum[idx]++;
            else if (storageType == MemoryKind::BRAM_T2P && info.rdwrPort < 2)
              accessNum[idx]++;
            else if (info.rdPort < 1)
              accessNum[idx]++;
            else if (info.wrPort < 1)
              writeNum[idx]++;
          }
        }

    II = max({II, *std::max_element(writeNum.begin(), writeNum.end()),
              *std::max_element(accessNum.begin(), accessNum.end())});
  }
  return II;
}

/// Calculate the minimum dependency II of function.
int64_t HLSCppEstimator::getDepMinII(int64_t II, FuncOp func,
                                     MemAccessesMap &map) {
  for (auto &pair : map) {
    auto loadStores = pair.second;

    // Walk through each pair of dependency source and destination.
    for (auto dstOp : loadStores)
      for (auto srcOp : loadStores) {
        if (dstOp == srcOp)
          continue;

        // If delay is smaller than the current II, stop and continue because
        // the minimum distance is one.
        auto delay = getIntAttrValue(dstOp, "schedule_end") -
                     getIntAttrValue(srcOp, "schedule_begin");
        if (delay <= II)
          continue;

        // Similar to getFuncDependencies() method, in some cases RAR is not
        // considered as dependency in Vivado HLS.
        auto srcMuxSize = getIntAttrValue(srcOp, "max_mux_size");
        auto dstMuxSize = getIntAttrValue(dstOp, "max_mux_size");
        if (isa<AffineReadOpInterface>(srcOp) && srcMuxSize <= 3 &&
            isa<AffineReadOpInterface>(dstOp) && dstMuxSize <= 3)
          continue;

        // Distance is always 1 thus the minimum II is equal to delay.
        // TODO: need more case study.
        if (MemRefAccess(srcOp) == MemRefAccess(dstOp))
          II = max(II, delay);
      }
  }
  return II;
}

/// Calculate the minimum dependency II of loop.
int64_t HLSCppEstimator::getDepMinII(int64_t II, AffineForOp forOp,
                                     MemAccessesMap &map) {
  // Collect start and end level of the pipeline.
  int64_t endLevel = 1;
  int64_t startLevel = 1;
  auto currentLoop = forOp;
  while (true) {
    if (auto outerLoop = dyn_cast<AffineForOp>(currentLoop->getParentOp())) {
      currentLoop = outerLoop;
      endLevel++;
      if (!getBoolAttrValue(outerLoop, "flatten"))
        startLevel++;
    } else
      break;
  }

  for (auto &pair : map) {
    auto loadStores = pair.second;

    // Walk through each pair of source and destination.
    for (auto dstOp : loadStores)
      for (auto srcOp : loadStores) {
        if (dstOp == srcOp)
          continue;

        // If delay is smaller than the current II, stop and continue because
        // the minimum distance is one.
        auto delay = getIntAttrValue(dstOp, "schedule_end") -
                     getIntAttrValue(srcOp, "schedule_begin");
        if (delay <= II)
          continue;

        // Similar to getFuncDependencies() method, in some cases RAR is not
        // considered as dependency in Vivado HLS.
        auto srcMuxSize = getIntAttrValue(srcOp, "max_mux_size");
        auto dstMuxSize = getIntAttrValue(dstOp, "max_mux_size");
        if (isa<AffineReadOpInterface>(srcOp) && srcMuxSize <= 3 &&
            isa<AffineReadOpInterface>(dstOp) && dstMuxSize <= 3)
          continue;

        // Now we must check whether carried dependency exists and calculate the
        // dependency distance if required.
        MemRefAccess dstAccess(dstOp);
        MemRefAccess srcAccess(srcOp);

        for (unsigned depth = startLevel; depth <= endLevel; ++depth) {
          FlatAffineConstraints depConstrs;
          SmallVector<DependenceComponent, 2> depComps;

          DependenceResult result = checkMemrefAccessDependence(
              srcAccess, dstAccess, depth, &depConstrs, &depComps,
              /*allowRAR=*/true);

          if (hasDependence(result)) {
            int64_t distance = 0;

            if (dstMuxSize > 3 || srcMuxSize > 3) {
              // If the two memory accesses are identical or one of them is
              // implemented as separate function call, the dependency exists.
              distance = 1;
            } else {
              SmallVector<int64_t, 8> accumTrips;
              accumTrips.push_back(1);

              // Calculate the distance of this dependency.
              for (auto i = depComps.rbegin(); i < depComps.rend(); ++i) {
                auto dep = *i;
                auto ifOp = cast<AffineForOp>(dep.op);

                auto tripCount = getIntAttrValue(ifOp, "trip_count");
                auto ub = dep.ub.getValue();
                auto lb = dep.lb.getValue();

                // If ub is more than zero, calculate the minimum positive
                // disatance. Otherwise, set distance to negative and break.
                if (ub >= 0)
                  distance +=
                      accumTrips.back() * max(lb, (int64_t)0) / ifOp.getStep();
                else {
                  distance = -1;
                  break;
                }
                accumTrips.push_back(accumTrips.back() * tripCount);
              }
            }

            // We will only consider intra-dependencies with positive distance.
            if (distance > 0) {
              int64_t minII = ceil((float)delay / distance);
              II = max(II, minII);
            }
          }
        }
      }
  }
  return II;
}

bool HLSCppEstimator::visitOp(AffineForOp op, int64_t begin) {
  // Set an attribute indicating the trip count. For now, we assume all loops
  // have static loop bound.
  int64_t tripCount = 1;
  if (auto optionalTripCount = getConstantTripCount(op))
    tripCount = optionalTripCount.getValue();
  else {
    // TODO: A temporary approach to estimate the trip count. For now, we take
    // the average of the upper bound and lower bound of trip count as the
    // estimated trip count.
    auto lowerBound = getBoundOfAffineBound(op.getLowerBound());
    auto upperBound = getBoundOfAffineBound(op.getUpperBound());

    if (lowerBound && upperBound) {
      auto lowerTripCount =
          upperBound.getValue().second - lowerBound.getValue().first;
      auto upperTripCount =
          upperBound.getValue().first - lowerBound.getValue().second;
      tripCount = (lowerTripCount + upperTripCount + 1) / 2;
    }
  }
  setAttrValue(op, "trip_count", tripCount);

  auto end = begin;
  auto &loopBlock = *op.getBody();

  // Estimate the loop block.
  if (auto schedule = estimateBlock(loopBlock, begin)) {
    end = max(end, schedule.getValue().end);
    begin = max(begin, schedule.getValue().begin);
  } else
    return false;

  // If the current loop is annotated as pipelined loop, extra dependency and
  // resource aware II analysis will be executed.
  if (getBoolAttrValue(op, "pipeline")) {
    // Collect load and store operations in the loop block for solving
    // possible carried dependencies.
    // TODO: include CallOps, how? It seems dependencies always exist for all
    // CallOps not matter its access pattern.
    MemAccessesMap map;
    getMemAccessesMap(loopBlock, map);

    // Calculate initial interval.
    auto targetII = getIntAttrValue(op, "target_ii");
    auto resII = getResMinII(begin, end, map);
    auto depII = getDepMinII(max(targetII, resII), op, map);
    auto II = max({targetII, resII, depII});

    setAttrValue(op, "res_ii", resII);
    setAttrValue(op, "dep_ii", depII);
    setAttrValue(op, "ii", II);

    setAttrValue(op, "flatten_trip_count", tripCount);

    // Calculate latency of each iteration.
    auto iterLatency = end - begin;
    setAttrValue(op, "iter_latency", iterLatency);

    auto latency = iterLatency + II * (tripCount - 1);
    setAttrValue(op, "latency", latency);

    // Entering and leaving a loop will consume extra 2 clock cycles.
    setScheduleValue(op, begin, begin + latency + 2);

    // Estimate the loop block resource utilization.
    setResourceValue(op, estimateResource(loopBlock, II));
    return true;
  }

  // If the current loop is annotated as flatten, it will be flattened into
  // the child pipelined loop. This will increase the flattened loop trip
  // count without changing the iteration latency.
  if (getBoolAttrValue(op, "flatten")) {
    auto child = dyn_cast<AffineForOp>(op.getBody()->front());
    assert(child && "the first containing operation is not a loop");

    auto iterLatency = getIntAttrValue(child, "iter_latency");
    setAttrValue(op, "iter_latency", iterLatency);

    auto II = getIntAttrValue(child, "ii");
    setAttrValue(op, "ii", II);

    auto flattenTripCount =
        getIntAttrValue(child, "flatten_trip_count") * tripCount;
    setAttrValue(op, "flatten_trip_count", flattenTripCount);

    auto latency = iterLatency + II * (flattenTripCount - 1);
    setAttrValue(op, "latency", latency);

    // Since the loop is flattened, it will no longer be entered and left.
    setScheduleValue(op, begin, begin + latency);

    // The resource utilization of flattened loop is equal to its child's.
    setAttrValue(op, "dsp", getIntAttrValue(child, "dsp"));
    return true;
  }

  // Default case (not flattend or pipelined), calculate latency and resource
  // utilization accordingly.
  auto iterLatency = end - begin;
  setAttrValue(op, "iter_latency", iterLatency);

  auto latency = iterLatency * tripCount;
  setAttrValue(op, "latency", latency);

  setScheduleValue(op, begin, begin + latency + 2);
  setResourceValue(op, estimateResource(loopBlock));
  return true;
}

//===----------------------------------------------------------------------===//
// Other Operation Handlers
//===----------------------------------------------------------------------===//

bool HLSCppEstimator::visitOp(AffineIfOp op, int64_t begin) {
  auto end = begin;
  auto thenBlock = op.getThenBlock();

  // Estimate then block.
  if (auto schedule = estimateBlock(*thenBlock, begin))
    end = max(end, schedule.getValue().end);
  else
    return false;

  // Handle else block if required.
  if (op.hasElse()) {
    auto elseBlock = op.getElseBlock();

    if (auto schedule = estimateBlock(*elseBlock, begin))
      end = max(end, schedule.getValue().end);
    else
      return false;
  }

  // In our assumption, AffineIfOp is completely transparent. Therefore, we
  // set a dummy schedule begin here.
  setScheduleValue(op, end, end);
  return true;
}

bool HLSCppEstimator::visitOp(CallOp op, int64_t begin) {
  auto callee = SymbolTable::lookupNearestSymbolFrom(op, op.getCallee());
  auto subFunc = dyn_cast<FuncOp>(callee);
  assert(subFunc && "callable is not a function operation");

  auto builder = OpBuilder(subFunc);
  HLSCppEstimator estimator(builder, latencyMap);
  estimator.estimateFunc(subFunc);

  // We assume enter and leave the subfunction require extra 2 clock cycles.
  if (auto subLatency = getIntAttrValue(subFunc, "latency")) {
    setScheduleValue(op, begin, begin + subLatency + 2);
    setAttrValue(op, "dsp", getIntAttrValue(subFunc, "dsp"));
    return true;
  } else
    return false;
}

//===----------------------------------------------------------------------===//
// Block Scheduler and Estimator
//===----------------------------------------------------------------------===//

int64_t HLSCppEstimator::getDspAllocMap(Block &block, ResourceAllocMap &faddMap,
                                        ResourceAllocMap &fmulMap) {
  int64_t staticDspNum = 0;
  for (auto &op : block) {
    auto begin = getIntAttrValue(&op, "schedule_begin");
    auto end = getIntAttrValue(&op, "schedule_end");

    // Accumulate the resource utilization of each operation.
    if (isa<AddFOp, SubFOp>(op))
      for (unsigned i = begin; i < end; ++i)
        faddMap[i]++;

    else if (isa<MulFOp>(op))
      for (unsigned i = begin; i < end; ++i)
        fmulMap[i]++;

    else if (isa<AffineForOp, CallOp>(op))
      staticDspNum += getIntAttrValue(&op, "dsp");

    else if (auto ifOp = dyn_cast<AffineIfOp>(op)) {
      // AffineIfOp is transparent during scheduling, thus here we recursively
      // enter each if block.
      staticDspNum += getDspAllocMap(*ifOp.getThenBlock(), faddMap, fmulMap);
      if (ifOp.hasElse())
        staticDspNum += getDspAllocMap(*ifOp.getElseBlock(), faddMap, fmulMap);
    }
  }
  return staticDspNum;
}

Resource HLSCppEstimator::estimateResource(Block &block, int64_t interval) {
  ResourceAllocMap faddMap;
  ResourceAllocMap fmulMap;
  auto staticDspNum = getDspAllocMap(block, faddMap, fmulMap);

  // Find the max resource utilization across all schedule levels.
  int64_t maxFadd = 0;
  for (auto level : faddMap)
    maxFadd = max(maxFadd, level.second);

  int64_t maxFmul = 0;
  for (auto level : fmulMap)
    maxFmul = max(maxFmul, level.second);

  // We assume the loop resource utilization cannot be shared. Therefore, the
  // overall resource utilization is loops' plus other operstions'. According
  // to profiling, floating-point add and muliply will consume 2 and 3 DSP
  // units, respectively.
  auto shareDspNum = maxFadd * 2 + maxFmul * 3;
  auto dsp = staticDspNum + shareDspNum;

  // If the block is pipelined (interval is positive), the minimum resource
  // utilization is determined by interval.
  if (interval > 0) {
    int64_t totalFadd = 0;
    int64_t totalFmul = 0;
    block.walk([&](Operation *op) {
      if (isa<AddFOp, SubFOp>(op))
        totalFadd++;
      else if (isa<MulFOp>(op))
        totalFmul++;
    });

    auto noShareDspNum = totalFadd * 2 + totalFmul * 3;
    dsp = staticDspNum + max(shareDspNum, noShareDspNum / interval);

    // Annotate dsp utilization with & without resource sharing.
    auto parentOp = block.getParentOp();
    setAttrValue(parentOp, "share_dsp", shareDspNum);
    setAttrValue(parentOp, "noshare_dsp", noShareDspNum);
  }

  // TODO: Estimate bram, ff, lut.
  int64_t bram = 0;
  int64_t ff = 0;
  int64_t lut = 0;
  return Resource(bram, dsp, ff, lut);
}

// Get the pointer of the scrOp's parent loop, which should locat at the same
// level with dstOp's any parent loop.
static Operation *getSameLevelDstOp(Operation *srcOp, Operation *dstOp) {
  // If srcOp and dstOp are already at the same level, return the srcOp.
  if (checkSameLevel(srcOp, dstOp))
    return dstOp;

  // Helper to get all surrouding AffineForOps. AffineIfOps are skipped.
  auto getSurroundFors =
      ([&](Operation *op, SmallVector<Operation *, 4> &nests) {
        nests.push_back(op);
        auto currentOp = op;
        while (true) {
          if (auto parentOp = currentOp->getParentOfType<AffineForOp>()) {
            nests.push_back(parentOp);
            currentOp = parentOp;
          } else if (auto parentOp = currentOp->getParentOfType<AffineIfOp>())
            currentOp = parentOp;
          else
            break;
        }
      });

  SmallVector<Operation *, 4> srcNests;
  SmallVector<Operation *, 4> dstNests;

  getSurroundFors(srcOp, srcNests);
  getSurroundFors(dstOp, dstNests);

  // If any parent of srcOp (or itself) and any parent of dstOp (or itself) are
  // at the same level, return the pointer.
  for (auto src : srcNests)
    for (auto dst : dstNests)
      if (checkSameLevel(src, dst))
        return dst;

  return nullptr;
}

/// Estimate the latency of a block with ALAP scheduling strategy, return the
/// end level of schedule. Meanwhile, the input begin will also be updated if
/// required (typically happens in AffineForOps).
Optional<Schedule> HLSCppEstimator::estimateBlock(Block &block, int64_t begin) {
  auto blockBegin = begin;
  auto blockEnd = begin;

  // Reversely walk through all operations in the block.
  for (auto i = block.rbegin(), e = block.rend(); i != e; ++i) {
    auto op = &*i;
    auto opBegin = begin;
    auto opEnd = begin;

    // Calculate the partition indices of memory load and store operations.
    if (isa<AffineLoadOp, AffineStoreOp>(op))
      getPartitionIndices(op);

    // Find the latest arrived successor depending on the current operation.
    for (auto user : op->getUsers()) {
      auto sameLevelUser = getSameLevelDstOp(op, user);
      opBegin = max(opBegin, getIntAttrValue(sameLevelUser, "schedule_end"));
    }

    // Check other dependencies and update schedule level.
    for (auto dstOp : dependsMap[op]) {
      auto sameLevelDstOp = getSameLevelDstOp(op, dstOp);
      opBegin = max(opBegin, getIntAttrValue(sameLevelDstOp, "schedule_end"));
    }

    // Check memory dependencies of the operation and update schedule level.
    for (auto operand : op->getOperands())
      if (operand.getType().isa<MemRefType>())
        // All users of the same memref value has the possibility to share
        // dependency with the current operation.
        for (auto depOp : operand.getUsers()) {
          auto depOpEnd = getIntAttrValue(depOp, "schedule_end");

          // If the depOp has not been scheduled or its schedule level will not
          // impact the current operation's scheduling, stop and continue.
          if (depOpEnd == -1 || depOpEnd <= opBegin)
            continue;

          // If either the depOp or the current operation is a function call,
          // depOpendency exists and the schedule level should be updated.
          if (isa<CallOp>(op) || isa<CallOp>(depOp)) {
            opBegin = max(opBegin, depOpEnd);
            continue;
          }

          // Now both of the depOp and the current operation must be memory
          // load/store operation.
          auto opMuxSize = getIntAttrValue(op, "max_mux_size");
          auto depOpMuxSize = getIntAttrValue(depOp, "max_mux_size");

          // In Vivado HLS, a memory access with undetermined partition index
          // will be implemented as a function call with a multiplexer if the
          // partition factor is larger than 3. Function call has dependency
          // with any load/store operation including RAR.
          if (isa<AffineReadOpInterface>(op) && opMuxSize <= 3 &&
              isa<AffineReadOpInterface>(depOp) && depOpMuxSize <= 3)
            continue;

          // Now we must check whether any dependency exists between the two
          // operations. If so, update the scheduling level.
          auto opAccess = MemRefAccess(op);
          auto depOpAccess = MemRefAccess(depOp);

          auto loopDepth = getNumCommonSurroundingLoops(*op, *depOp);
          for (unsigned depth = 1; depth <= loopDepth + 1; ++depth) {
            FlatAffineConstraints dependConstrs;

            DependenceResult result = checkMemrefAccessDependence(
                opAccess, depOpAccess, depth, &dependConstrs,
                /*dependenceComponents=*/nullptr, /*allowRAR=*/true);

            if (hasDependence(result)) {
              opBegin = max(opBegin, depOpEnd);
              break;
            }
          }
        }

    // Estimate the current operation.
    if (dispatchVisitor(op, opBegin))
      opEnd = max(opEnd, getIntAttrValue(op, "schedule_end"));
    else
      return Optional<Schedule>();

    // Update the block schedule end and begin.
    if (i == block.rbegin())
      blockBegin = opBegin;
    else
      blockBegin = min(blockBegin, opBegin);

    blockEnd = max(blockEnd, opEnd);
  }
  return Schedule(blockBegin, blockEnd);
}

/// Get the innermost surrounding operation, either an AffineForOp or a FuncOp.
/// In this method, AffineIfOp is transparent as well.
static Operation *getSurroundingOp(Operation *op) {
  auto currentOp = op;
  while (true) {
    if (auto parentIfOp = currentOp->getParentOfType<AffineIfOp>())
      currentOp = parentIfOp;
    else if (auto parentForOp = currentOp->getParentOfType<AffineForOp>())
      return parentForOp;
    else if (auto parentFuncOp = currentOp->getParentOfType<FuncOp>())
      return parentFuncOp;
    else
      return nullptr;
  }
}

void HLSCppEstimator::reverseSchedule(Block &block) {
  block.walk([&](Operation *op) {
    // Get schedule level.
    auto begin = getIntAttrValue(op, "schedule_begin");
    auto end = getIntAttrValue(op, "schedule_end");

    // Reverse schedule level.
    if (auto surOp = getSurroundingOp(op)) {
      if (isa<AffineForOp>(surOp)) {
        auto surOpBegin = getIntAttrValue(surOp, "schedule_begin");

        if (getBoolAttrValue(surOp, "flatten")) {
          // Handle flattened surrounding loops.
          setScheduleValue(op, surOpBegin, surOpBegin + end - begin);
        } else {
          // Handle normal cases.
          auto iterLatency = getIntAttrValue(surOp, "iter_latency");
          setScheduleValue(op, surOpBegin + iterLatency - end,
                           surOpBegin + iterLatency - begin);
        }
      } else if (isa<FuncOp>(surOp)) {
        auto latency = getIntAttrValue(surOp, "latency");
        setScheduleValue(op, latency - end, latency - begin);
      }
    }
  });
}

void HLSCppEstimator::initEstimator(Block &block) {
  // Clear global maps and scheduling information. Walk through all loops and
  // establish dependencies. The rationale here is in Vivado HLS, a loop will
  // always be blocked by other loops before it, even if no actual dependency
  // exists between them.
  dependsMap.clear();
  memPortInfosMap.clear();

  SmallVector<Operation *, 16> loops;
  block.walk([&](Operation *op) {
    op->removeAttr("schedule_begin");
    op->removeAttr("schedule_end");

    if (isa<AffineForOp>(op))
      loops.push_back(op);
  });

  unsigned idx = 1;
  for (auto srcLoop : loops) {
    for (auto dstLoop : llvm::drop_begin(loops, idx))
      if (checkSameLevel(srcLoop, dstLoop))
        dependsMap[srcLoop].push_back(dstLoop);
    idx++;
  }
};

void HLSCppEstimator::estimateFunc(FuncOp func) {
  initEstimator(func.front());

  // Collect all memory access operations for later use.
  MemAccessesMap map;
  getMemAccessesMap(func.front(), map);

  // Recursively estimate blocks in the function.
  if (auto schedule = estimateBlock(func.front(), 0)) {
    auto latency = schedule.getValue().end;
    setAttrValue(func, "latency", latency);

    if (getBoolAttrValue(func, "dataflow")) {
      int64_t maxInterval = 0;
      for (auto callOp : func.getOps<CallOp>()) {
        auto latency = getIntAttrValue(callOp, "schedule_end") -
                       getIntAttrValue(callOp, "schedule_begin");
        maxInterval = max(maxInterval, latency);
      }
      setAttrValue(func, "ii", maxInterval);
    }

    // TODO: support CallOp inside of the function.
    if (getBoolAttrValue(func, "pipeline")) {
      auto targetII = getIntAttrValue(func, "target_ii");
      auto resII = getResMinII(0, latency, map);
      auto depII = getDepMinII(max(targetII, resII), func, map);
      auto II = max({targetII, resII, depII});

      setAttrValue(func, "res_ii", resII);
      setAttrValue(func, "dep_ii", depII);
      setAttrValue(func, "ii", II);
    }

    // Scheduled levels of all operations are reversed in this method, because
    // we have done the ALAP scheduling in a reverse order. Note that after
    // the reverse, the annotated scheduling level of each operation is a
    // relative level of the nearest surrounding AffineForOp or FuncOp.
    reverseSchedule(func.front());
  } else {
    // Scheduling failed due to earlier error.
    setAttrValue(func, "latency", (int64_t)-1);
  }

  // Estimate the resource utilization of the function.
  auto interval = getIntAttrValue(func, "ii");
  auto resource = estimateResource(func.front(), interval);

  // Calculate the function memrefs BRAM utilization.
  int64_t numBram = 0;
  for (auto &pair : map) {
    auto memrefType = pair.first.getType().cast<MemRefType>();
    auto partitionNum = getPartitionFactors(memrefType);
    auto storageType = MemoryKind(memrefType.getMemorySpace());

    if (storageType == MemoryKind::BRAM_1P ||
        storageType == MemoryKind::BRAM_S2P ||
        storageType == MemoryKind::BRAM_T2P) {
      // Multiply bit width of type.
      // TODO: handle index types.
      int64_t memrefSize =
          memrefType.getElementTypeBitWidth() * memrefType.getNumElements();
      numBram += ((memrefSize + 18000 - 1) / 18000) * partitionNum;
    }
  }
  resource.bram += numBram;

  setResourceValue(func, resource);
}

void HLSCppEstimator::estimateLoop(AffineForOp loop) {
  initEstimator(*loop.getBody());
  dispatchVisitor(loop, 0);
  reverseSchedule(*loop.getBody());
}

//===----------------------------------------------------------------------===//
// Entry of scalehls-opt
//===----------------------------------------------------------------------===//

void scalehls::getLatencyMap(INIReader spec, LatencyMap &latencyMap) {
  auto freq = spec.Get("specification", "frequency", "100MHz");

  latencyMap["fadd"] = spec.GetInteger(freq, "fadd", 4);
  latencyMap["fmul"] = spec.GetInteger(freq, "fmul", 3);
  latencyMap["fdiv"] = spec.GetInteger(freq, "fdiv", 15);
  latencyMap["fcmp"] = spec.GetInteger(freq, "fcmp", 1);
}

namespace {
struct QoREstimation : public scalehls::QoREstimationBase<QoREstimation> {
  void runOnOperation() override {
    auto module = getOperation();
    auto builder = OpBuilder(module);

    // Read configuration file.
    INIReader spec(targetSpec);
    if (spec.ParseError())
      emitError(module.getLoc(), "target spec file parse fail\n");

    // Collect profiling latency data, where default values are based on Xilinx
    // PYNQ-Z1 board.
    LatencyMap latencyMap;
    getLatencyMap(spec, latencyMap);

    // Estimate performance and resource utilization. If any other functions are
    // called by the top function, it will be estimated in the procedure of
    // estimating the top function.
    for (auto func : module.getOps<FuncOp>())
      if (auto topFunction = func->getAttrOfType<BoolAttr>("top_function"))
        if (topFunction.getValue())
          HLSCppEstimator(builder, latencyMap).estimateFunc(func);
  }
};
} // namespace

std::unique_ptr<Pass> scalehls::createQoREstimationPass() {
  return std::make_unique<QoREstimation>();
}
