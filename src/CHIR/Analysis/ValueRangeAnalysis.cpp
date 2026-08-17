// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/Analysis/ValueRangeAnalysis.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <unordered_set>
#include "cangjie/CHIR/Checker/OverflowChecking.h"
#include "cangjie/CHIR/Analysis/Arithmetic.h"
#include "cangjie/CHIR/Analysis/Engine.h"

namespace Cangjie::CHIR {
namespace {
struct LoopRangeSnapshotEntry {
    std::unique_ptr<SIntRange> range;
    Type* type;
    bool preserveDuringBodyWidening;
};
using LoopRangeSnapshot = std::unordered_map<Value*, LoopRangeSnapshotEntry>;
using LoopRangeSnapshots = std::unordered_map<const Block*, LoopRangeSnapshot>;
std::unordered_map<const RangeAnalysis*, LoopRangeSnapshots> loopRangeSnapshots;
std::mutex loopRangeSnapshotsMtx;

struct LoopDescriptor {
    const Block* header{nullptr};
    std::unordered_set<const Block*> blocks;
    std::vector<const Block*> latches;
    std::vector<const Block*> incoming;
    std::vector<std::pair<const Block*, const Block*>> exits;
    std::optional<size_t> parent;

    bool Contains(const Block* block) const
    {
        return block != nullptr && blocks.find(block) != blocks.end();
    }

    bool IsLatch(const Block* block) const
    {
        return std::find(latches.begin(), latches.end(), block) != latches.end();
    }

    bool IsExitEdge(const Block* from, const Block* to) const
    {
        return std::find(exits.begin(), exits.end(), std::make_pair(from, to)) != exits.end();
    }
};

class LoopForest {
public:
    explicit LoopForest(const Function* function)
    {
        Build(function);
    }

    const LoopDescriptor* FindByHeader(const Block* header) const
    {
        auto found = headerToLoop.find(header);
        return found == headerToLoop.end() ? nullptr : &loops[found->second];
    }

    const LoopDescriptor* FindInnermost(const Block* block) const
    {
        auto found = blockToLoops.find(block);
        if (found == blockToLoops.end() || found->second.empty()) {
            return nullptr;
        }
        return &loops[found->second.front()];
    }

    bool IsBackedge(const Block* predecessor, const Block* header) const
    {
        auto loop = FindByHeader(header);
        return loop != nullptr && loop->IsLatch(predecessor);
    }

    bool IsLoopBodySuccessor(const Block* header, const Block* successor) const
    {
        auto loop = FindByHeader(header);
        return loop != nullptr && successor != header && loop->Contains(successor);
    }

    bool IsExitEdge(const Block* from, const Block* to) const
    {
        for (const auto& loop : loops) {
            if (loop.IsExitEdge(from, to)) {
                return true;
            }
        }
        return false;
    }

private:
    using BlockSet = std::unordered_set<const Block*>;
    using DominatorMap = std::unordered_map<const Block*, BlockSet>;

    static BlockSet Intersect(const BlockSet& lhs, const BlockSet& rhs)
    {
        const auto* smaller = &lhs;
        const auto* larger = &rhs;
        if (lhs.size() > rhs.size()) {
            std::swap(smaller, larger);
        }
        BlockSet result;
        for (auto block : *smaller) {
            if (larger->find(block) != larger->end()) {
                result.emplace(block);
            }
        }
        return result;
    }

    static void MarkReachable(const Block* root, const BlockSet& allBlocks, BlockSet& reachable)
    {
        if (root == nullptr) {
            return;
        }
        std::vector<const Block*> worklist{root};
        while (!worklist.empty()) {
            auto block = worklist.back();
            worklist.pop_back();
            if (allBlocks.find(block) == allBlocks.end() || !reachable.emplace(block).second) {
                continue;
            }
            for (auto successor : block->GetSuccessors()) {
                worklist.emplace_back(successor);
            }
        }
    }

    static DominatorMap ComputeDominators(
        const Function* function, const std::vector<const Block*>& blocks, const BlockSet& allBlocks)
    {
        BlockSet roots;
        if (function != nullptr && function->GetEntryBlock() != nullptr) {
            roots.emplace(function->GetEntryBlock());
        }
        for (auto block : blocks) {
            const auto& predecessors = block->GetPredecessors();
            if (predecessors.empty() || std::none_of(predecessors.begin(), predecessors.end(),
                    [&allBlocks](const Block* pred) { return allBlocks.find(pred) != allBlocks.end(); })) {
                roots.emplace(block);
            }
        }
        BlockSet reachable;
        for (auto root : roots) {
            MarkReachable(root, allBlocks, reachable);
        }
        for (auto block : blocks) {
            if (reachable.find(block) == reachable.end()) {
                roots.emplace(block);
                MarkReachable(block, allBlocks, reachable);
            }
        }

        DominatorMap dominators;
        for (auto block : blocks) {
            dominators.emplace(block, roots.find(block) != roots.end() ? BlockSet{block} : allBlocks);
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto block : blocks) {
                if (roots.find(block) != roots.end()) {
                    continue;
                }
                std::optional<BlockSet> intersection;
                for (auto predecessor : block->GetPredecessors()) {
                    auto found = dominators.find(predecessor);
                    if (found == dominators.end()) {
                        continue;
                    }
                    intersection = intersection.has_value()
                        ? Intersect(intersection.value(), found->second)
                        : found->second;
                }
                BlockSet next = intersection.value_or(BlockSet{});
                next.emplace(block);
                if (next != dominators.at(block)) {
                    dominators[block] = std::move(next);
                    changed = true;
                }
            }
        }
        return dominators;
    }

    static BlockSet BuildNaturalLoop(
        const Block* header, const Block* latch, const DominatorMap& dominators)
    {
        BlockSet result{header, latch};
        std::vector<const Block*> worklist;
        if (latch != header) {
            worklist.emplace_back(latch);
        }
        while (!worklist.empty()) {
            auto block = worklist.back();
            worklist.pop_back();
            for (auto predecessor : block->GetPredecessors()) {
                auto found = dominators.find(predecessor);
                if (found == dominators.end() ||
                    found->second.find(header) == found->second.end() ||
                    !result.emplace(predecessor).second || predecessor == header) {
                    continue;
                }
                worklist.emplace_back(predecessor);
            }
        }
        return result;
    }

    void FinalizeDescriptors()
    {
        for (auto& loop : loops) {
            for (auto predecessor : loop.header->GetPredecessors()) {
                if (loop.Contains(predecessor)) {
                    if (std::find(loop.latches.begin(), loop.latches.end(), predecessor) == loop.latches.end()) {
                        loop.latches.emplace_back(predecessor);
                    }
                } else {
                    loop.incoming.emplace_back(predecessor);
                }
            }
            for (auto block : loop.blocks) {
                for (auto successor : block->GetSuccessors()) {
                    if (!loop.Contains(successor)) {
                        loop.exits.emplace_back(block, successor);
                    }
                }
            }
        }
        for (size_t child = 0; child < loops.size(); ++child) {
            size_t bestSize = std::numeric_limits<size_t>::max();
            for (size_t candidate = 0; candidate < loops.size(); ++candidate) {
                if (child == candidate || loops[candidate].blocks.size() <= loops[child].blocks.size()) {
                    continue;
                }
                const bool contains = std::all_of(loops[child].blocks.begin(), loops[child].blocks.end(),
                    [this, candidate](const Block* block) { return loops[candidate].Contains(block); });
                if (contains && loops[candidate].blocks.size() < bestSize) {
                    loops[child].parent = candidate;
                    bestSize = loops[candidate].blocks.size();
                }
            }
        }
        for (size_t index = 0; index < loops.size(); ++index) {
            headerToLoop.emplace(loops[index].header, index);
            for (auto block : loops[index].blocks) {
                blockToLoops[block].emplace_back(index);
            }
        }
        for (auto& [_, indices] : blockToLoops) {
            std::sort(indices.begin(), indices.end(), [this](size_t lhs, size_t rhs) {
                return loops[lhs].blocks.size() < loops[rhs].blocks.size();
            });
        }
    }

    void Build(const Function* function)
    {
        if (function == nullptr || function->GetBody() == nullptr) {
            return;
        }
        auto mutableBlocks = function->GetBody()->GetAllBlocks();
        std::vector<const Block*> blocks(mutableBlocks.begin(), mutableBlocks.end());
        BlockSet allBlocks(blocks.begin(), blocks.end());
        auto dominators = ComputeDominators(function, blocks, allBlocks);
        std::unordered_map<const Block*, size_t> loopByHeader;
        for (auto latch : blocks) {
            for (auto header : latch->GetSuccessors()) {
                auto found = dominators.find(latch);
                if (found == dominators.end() || found->second.find(header) == found->second.end()) {
                    continue;
                }
                auto descriptor = loopByHeader.find(header);
                if (descriptor == loopByHeader.end()) {
                    descriptor = loopByHeader.emplace(header, loops.size()).first;
                    loops.emplace_back();
                    loops.back().header = header;
                }
                auto& loop = loops[descriptor->second];
                auto natural = BuildNaturalLoop(header, latch, dominators);
                loop.blocks.insert(natural.begin(), natural.end());
                loop.latches.emplace_back(latch);
            }
        }
        FinalizeDescriptors();
    }

    std::vector<LoopDescriptor> loops;
    std::unordered_map<const Block*, size_t> headerToLoop;
    std::unordered_map<const Block*, std::vector<size_t>> blockToLoops;
};

std::unordered_map<const Function*, std::unique_ptr<LoopForest>> loopForestCache;
std::mutex loopForestCacheMtx;

const LoopForest* GetLoopForest(const Function* function)
{
    if (function == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(loopForestCacheMtx);
    auto found = loopForestCache.find(function);
    if (found == loopForestCache.end()) {
        found = loopForestCache.emplace(function, std::make_unique<LoopForest>(function)).first;
    }
    return found->second.get();
}

const LoopForest* GetLoopForest(const Block* block)
{
    return block == nullptr ? nullptr : GetLoopForest(block->GetTopLevelFunc());
}

void ClearLoopForestCache()
{
    std::lock_guard<std::mutex> lock(loopForestCacheMtx);
    loopForestCache.clear();
}

thread_local std::unordered_set<const Block*> queryRefinementBlocks;
thread_local std::unordered_set<const Value*> queryRefinementValues;
thread_local std::unordered_set<const Value*> queryRefinementRoots;
thread_local const RangeAnalysis* boundedLoopEvaluationOwner{nullptr};
using BoundedAggregateStoreMap = std::unordered_map<Value*, Value*>;
thread_local BoundedAggregateStoreMap* boundedAggregateStores{nullptr};
struct RecordedCallContexts {
    std::unordered_set<std::string> keys;
    bool complete{true};
};
using BoundedLoopCallContextMap = std::unordered_map<const Expression*, RecordedCallContexts>;
using ParentCallContextMap = std::unordered_map<std::string, BoundedLoopCallContextMap>;
thread_local BoundedLoopCallContextMap* boundedLoopCallContextRecorder{nullptr};
ParentCallContextMap analyzedCallContexts;
ParentCallContextMap boundedLoopCallContexts;
std::unordered_map<std::string, std::optional<std::vector<GlobalVar*>>>
    contextEffectClosureCache;
size_t analyzedCallContextCount{0};
bool analyzedCallContextRecordingComplete{true};
std::mutex boundedLoopCallContextsMtx;
std::mutex contextEffectClosureCacheMtx;
constexpr size_t MAX_GLOBAL_CONTEXT_PER_FUNCTION = 8;
constexpr size_t MAX_BOUNDED_LOOP_CONTEXT_PER_FUNCTION = 1024;
constexpr size_t MAX_TOTAL_BOUNDED_LOOP_CONTEXT_SUMMARIES = 2048;
constexpr size_t MAX_CONTEXT_GLOBALS = 64;
constexpr size_t MAX_CONTEXT_CALL_CLOSURE_FUNCTIONS = 128;
constexpr size_t MAX_LAMBDA_CONTEXTS_PER_ANALYSIS = 32;
constexpr uint64_t MAX_TOTAL_CONTEXT_WORK_UNITS = 200000;
constexpr auto MAX_CONTEXT_ANALYSIS_WALL_TIME = std::chrono::seconds(15);
constexpr size_t MAX_ARRAY_METADATA_CANDIDATES = 256;

struct RangeAnalysisConfig {
    size_t loopWideningStartTimes{3};
    size_t maxInqueueTimes{24};
    size_t maxContextPerFunction{16};
    size_t maxTotalContextSummaries{1024};
    size_t maxContextAnalysisDepth{24};
    size_t maxExactIntSetSize{64};
    size_t maxBoundedLoopObservations{1024};
    size_t maxRecordedCallContexts{8192};
};

size_t ReadRangeAnalysisConfigValue(
    const char* name, size_t defaultValue, size_t minimum, size_t maximum)
{
    const char* rawValue = std::getenv(name);
    if (rawValue == nullptr || *rawValue == '\0') {
        return defaultValue;
    }
    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoull(rawValue, &end, 10);
    if (errno == ERANGE || end == rawValue || *end != '\0' ||
        parsed > std::numeric_limits<size_t>::max()) {
        return defaultValue;
    }
    const auto value = static_cast<size_t>(parsed);
    return value < minimum || value > maximum ? defaultValue : value;
}

RangeAnalysisConfig LoadRangeAnalysisConfig()
{
    RangeAnalysisConfig config;
    config.loopWideningStartTimes =
        ReadRangeAnalysisConfigValue("CJ_RA_WIDEN_START", config.loopWideningStartTimes, 2, 8);
    config.maxInqueueTimes =
        ReadRangeAnalysisConfigValue("CJ_RA_MAX_INQUEUE", config.maxInqueueTimes, 16, 64);
    config.maxContextPerFunction = ReadRangeAnalysisConfigValue(
        "CJ_RA_CONTEXT_PER_FUNCTION", config.maxContextPerFunction, 8, 64);
    config.maxTotalContextSummaries = ReadRangeAnalysisConfigValue(
        "CJ_RA_TOTAL_CONTEXT_SUMMARIES", config.maxTotalContextSummaries, 128, 1024);
    config.maxContextAnalysisDepth = ReadRangeAnalysisConfigValue(
        "CJ_RA_CONTEXT_DEPTH", config.maxContextAnalysisDepth, 4, 24);
    config.maxExactIntSetSize =
        ReadRangeAnalysisConfigValue("CJ_RA_EXACT_SET_SIZE", config.maxExactIntSetSize, 8, 128);
    config.maxBoundedLoopObservations = ReadRangeAnalysisConfigValue(
        "CJ_RA_LOOP_OBSERVATIONS", config.maxBoundedLoopObservations, 512, 8192);
    config.maxRecordedCallContexts = ReadRangeAnalysisConfigValue(
        "CJ_RA_RECORDED_CONTEXTS", config.maxRecordedCallContexts, 1024, 8192);
    if (config.maxInqueueTimes <= config.loopWideningStartTimes) {
        config.loopWideningStartTimes = RangeAnalysisConfig{}.loopWideningStartTimes;
        config.maxInqueueTimes = RangeAnalysisConfig{}.maxInqueueTimes;
    }
    return config;
}

const RangeAnalysisConfig rangeAnalysisConfig = LoadRangeAnalysisConfig();

size_t MaxTotalReachableContextSummaries()
{
    return rangeAnalysisConfig.maxTotalContextSummaries + MAX_TOTAL_BOUNDED_LOOP_CONTEXT_SUMMARIES;
}

size_t MaxContextObjectIdentities()
{
    return rangeAnalysisConfig.maxTotalContextSummaries * 256;
}

struct RangeAnalysisPerfStats {
    uint64_t finiteClassResolveCalls{0};
    uint64_t finiteClassResolveNodes{0};
    uint64_t finiteClassMemoHits{0};
    uint64_t finiteClassBudgetRejects{0};
    uint64_t finiteClassResolveNanos{0};
    uint64_t finiteInvokeCalls{0};
    uint64_t finiteInvokeNanos{0};
    uint64_t finiteDispatchMergeCalls{0};
    uint64_t finiteDispatchMergeNanos{0};
    uint64_t finiteDispatchStateCopies{0};
    uint64_t finiteDispatchStateJoins{0};
    uint64_t dispatchTargetResolveCalls{0};
    uint64_t dispatchTargetResolveNanos{0};
    uint64_t contextEffectClosureCalls{0};
    uint64_t contextEffectClosureCacheHits{0};
    uint64_t contextEffectClosureNanos{0};
    uint64_t boundedInvokeReplays{0};
    uint64_t contextKeyCalls{0};
    uint64_t contextKeyNanos{0};
    uint64_t contextAnalysisCalls{0};
    uint64_t contextAnalysisNanos{0};
    uint64_t contextReadyCacheHits{0};
    uint64_t contextFailedCacheHits{0};
    uint64_t contextComputingCacheHits{0};
    uint64_t contextBudgetRejects{0};
    uint64_t contextSummaryInstantiations{0};
    uint64_t contextSummaryInstantiationNanos{0};
    uint64_t contextNewAnalyses{0};
    uint64_t contextFixpointNanos{0};
    uint64_t contextSummaryBuildNanos{0};
    uint64_t reachableVisitCalls{0};
    uint64_t reachableVisitNanos{0};
    size_t maxFiniteClassDepth{0};
    size_t maxFiniteClassCount{0};
    size_t maxFiniteTargetCount{0};
    size_t maxReachableContexts{0};
};

struct RangeAnalysisTuningStats {
    uint64_t loopWideningApplications{0};
    uint64_t inqueueLimitHits{0};
    uint64_t exactSetLimitRejects{0};
    size_t maxInqueueCount{0};
};

RangeAnalysisPerfStats rangeAnalysisPerfStats;
RangeAnalysisTuningStats rangeAnalysisTuningStats;
std::unordered_map<std::string, uint64_t> finiteClassFailureReasons;
std::unordered_map<std::string, uint64_t> contextTopSources;
std::string firstContextTopSource;
std::string firstContextTopFunction;
uint64_t firstContextTopLine{0};
size_t overrideTraceInvokeLogCount{0};
bool rangeAnalysisStatsSessionActive{false};
std::chrono::steady_clock::time_point rangeAnalysisStatsSessionStart;
thread_local const Function* overrideTraceFunction{nullptr};
thread_local const Expression* overrideTraceCallsite{nullptr};

bool IsOverrideTraceEnabled()
{
    static const bool enabled = []() {
        auto value = std::getenv("CJ_RA_TRACE_OVERRIDE");
        return value != nullptr && std::string(value) == "1";
    }();
    return enabled;
}

const char* GetRangeAnalysisStatsFile()
{
    static const char* path = std::getenv("CJ_RA_STATS_FILE");
    return path != nullptr && *path != '\0' ? path : nullptr;
}

bool IsRangeAnalysisStatsEnabled()
{
    return GetRangeAnalysisStatsFile() != nullptr;
}

bool IsRangeAnalysisPerfTraceEnabled()
{
    static const bool enabled =
        std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr || IsOverrideTraceEnabled();
    return enabled;
}

bool IsRangeAnalysisPerfCollectionEnabled()
{
    return IsRangeAnalysisPerfTraceEnabled() || IsRangeAnalysisStatsEnabled();
}

void RecordExactSetLimitReject()
{
    if (IsRangeAnalysisPerfCollectionEnabled()) {
        ++rangeAnalysisTuningStats.exactSetLimitRejects;
    }
}

bool ExceedsExactIntSetLimit(size_t count)
{
    if (count <= rangeAnalysisConfig.maxExactIntSetSize) {
        return false;
    }
    RecordExactSetLimitReject();
    return true;
}

bool ExceedsExactIntSetLimit(__int128 count)
{
    if (count <= static_cast<__int128>(rangeAnalysisConfig.maxExactIntSetSize)) {
        return false;
    }
    RecordExactSetLimitReject();
    return true;
}

bool ExceedsExactIntSetLimit(unsigned __int128 count)
{
    if (count <= static_cast<unsigned __int128>(rangeAnalysisConfig.maxExactIntSetSize)) {
        return false;
    }
    RecordExactSetLimitReject();
    return true;
}

bool ReachedExactIntSetLimit(size_t count)
{
    if (count < rangeAnalysisConfig.maxExactIntSetSize) {
        return false;
    }
    RecordExactSetLimitReject();
    return true;
}

bool ExceedsExactIntSetProduct(size_t lhsSize, size_t rhsSize)
{
    if (rhsSize == 0 || lhsSize <= rangeAnalysisConfig.maxExactIntSetSize / rhsSize) {
        return false;
    }
    RecordExactSetLimitReject();
    return true;
}

void RecordFiniteClassFailure(const char* reason)
{
    if (IsRangeAnalysisPerfCollectionEnabled()) {
        ++finiteClassFailureReasons[reason];
    }
}

void RecordContextTopSource(const char* source)
{
    if (!IsRangeAnalysisPerfCollectionEnabled()) {
        return;
    }
    ++contextTopSources[source];
    if (firstContextTopSource.empty()) {
        firstContextTopSource = source;
        firstContextTopFunction =
            overrideTraceFunction == nullptr ? "<none>" : overrideTraceFunction->GetIdentifier();
        firstContextTopLine = overrideTraceCallsite == nullptr
            ? 0
            : overrideTraceCallsite->GetDebugLocation().GetBeginPos().line;
    }
}

class ScopedOverrideTraceLocation {
public:
    ScopedOverrideTraceLocation(const Function* function, const Expression* callsite)
        : previousFunction(overrideTraceFunction), previousCallsite(overrideTraceCallsite)
    {
        overrideTraceFunction = function;
        overrideTraceCallsite = callsite;
    }

    ~ScopedOverrideTraceLocation()
    {
        overrideTraceFunction = previousFunction;
        overrideTraceCallsite = previousCallsite;
    }

private:
    const Function* previousFunction;
    const Expression* previousCallsite;
};

class ScopedRangeAnalysisPerfTimer {
public:
    explicit ScopedRangeAnalysisPerfTimer(uint64_t* destination)
        : destination(IsRangeAnalysisPerfCollectionEnabled() ? destination : nullptr),
          start(this->destination == nullptr ? Clock::time_point{} : Clock::now())
    {
    }

    ~ScopedRangeAnalysisPerfTimer()
    {
        if (destination == nullptr) {
            return;
        }
        *destination += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    }

private:
    using Clock = std::chrono::steady_clock;
    uint64_t* destination;
    Clock::time_point start;
};

double RangeAnalysisPerfMilliseconds(uint64_t nanoseconds)
{
    return static_cast<double>(nanoseconds) / 1000000.0;
}

void WriteJsonString(std::ostream& output, const std::string& value)
{
    output << '"';
    for (char character : value) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                output << character;
                break;
        }
    }
    output << '"';
}

void PrintAndResetRangeAnalysisPerfStats()
{
    if (IsRangeAnalysisPerfTraceEnabled() &&
        (rangeAnalysisPerfStats.finiteClassResolveCalls != 0 ||
            rangeAnalysisPerfStats.contextAnalysisCalls != 0 ||
            rangeAnalysisPerfStats.reachableVisitCalls != 0)) {
        std::cerr << "[RangeAnalysisPerf]"
                  << " finite-resolve-calls=" << rangeAnalysisPerfStats.finiteClassResolveCalls
                  << " finite-resolve-nodes=" << rangeAnalysisPerfStats.finiteClassResolveNodes
                  << " finite-resolve-cache-hits=" << rangeAnalysisPerfStats.finiteClassMemoHits
                  << " finite-resolve-budget-rejects="
                  << rangeAnalysisPerfStats.finiteClassBudgetRejects
                  << " finite-resolve-ms="
                  << RangeAnalysisPerfMilliseconds(rangeAnalysisPerfStats.finiteClassResolveNanos)
                  << " finite-invoke-calls=" << rangeAnalysisPerfStats.finiteInvokeCalls
                  << " finite-invoke-ms="
                  << RangeAnalysisPerfMilliseconds(rangeAnalysisPerfStats.finiteInvokeNanos)
                  << " dispatch-merges=" << rangeAnalysisPerfStats.finiteDispatchMergeCalls
                  << " dispatch-merge-ms="
                  << RangeAnalysisPerfMilliseconds(rangeAnalysisPerfStats.finiteDispatchMergeNanos)
                  << " dispatch-state-copies="
                  << rangeAnalysisPerfStats.finiteDispatchStateCopies
                  << " dispatch-state-joins="
                  << rangeAnalysisPerfStats.finiteDispatchStateJoins
                  << " target-resolve-calls="
                  << rangeAnalysisPerfStats.dispatchTargetResolveCalls
                  << " target-resolve-ms="
                  << RangeAnalysisPerfMilliseconds(
                         rangeAnalysisPerfStats.dispatchTargetResolveNanos)
                  << " effect-closure-calls="
                  << rangeAnalysisPerfStats.contextEffectClosureCalls
                  << " effect-closure-cache-hits="
                  << rangeAnalysisPerfStats.contextEffectClosureCacheHits
                  << " effect-closure-ms="
                  << RangeAnalysisPerfMilliseconds(
                         rangeAnalysisPerfStats.contextEffectClosureNanos)
                  << " bounded-invoke-replays="
                  << rangeAnalysisPerfStats.boundedInvokeReplays
                  << " context-key-calls=" << rangeAnalysisPerfStats.contextKeyCalls
                  << " context-key-ms="
                  << RangeAnalysisPerfMilliseconds(rangeAnalysisPerfStats.contextKeyNanos)
                  << " context-analysis-calls=" << rangeAnalysisPerfStats.contextAnalysisCalls
                  << " context-analysis-ms="
                  << RangeAnalysisPerfMilliseconds(rangeAnalysisPerfStats.contextAnalysisNanos)
                  << " context-ready-hits="
                  << rangeAnalysisPerfStats.contextReadyCacheHits
                  << " context-failed-hits="
                  << rangeAnalysisPerfStats.contextFailedCacheHits
                  << " context-computing-hits="
                  << rangeAnalysisPerfStats.contextComputingCacheHits
                  << " context-budget-rejects="
                  << rangeAnalysisPerfStats.contextBudgetRejects
                  << " context-summary-instantiations="
                  << rangeAnalysisPerfStats.contextSummaryInstantiations
                  << " context-summary-instantiation-ms="
                  << RangeAnalysisPerfMilliseconds(
                         rangeAnalysisPerfStats.contextSummaryInstantiationNanos)
                  << " context-new-analyses="
                  << rangeAnalysisPerfStats.contextNewAnalyses
                  << " context-fixpoint-ms="
                  << RangeAnalysisPerfMilliseconds(rangeAnalysisPerfStats.contextFixpointNanos)
                  << " context-summary-build-ms="
                  << RangeAnalysisPerfMilliseconds(
                         rangeAnalysisPerfStats.contextSummaryBuildNanos)
                  << " reachable-visits=" << rangeAnalysisPerfStats.reachableVisitCalls
                  << " reachable-visit-ms="
                  << RangeAnalysisPerfMilliseconds(rangeAnalysisPerfStats.reachableVisitNanos)
                  << " max-class-depth=" << rangeAnalysisPerfStats.maxFiniteClassDepth
                  << " max-classes=" << rangeAnalysisPerfStats.maxFiniteClassCount
                  << " max-targets=" << rangeAnalysisPerfStats.maxFiniteTargetCount
                  << " max-reachable-contexts=" << rangeAnalysisPerfStats.maxReachableContexts
                  << '\n';
        if (!finiteClassFailureReasons.empty()) {
            std::cerr << "[RangeAnalysisFiniteClassFailures]";
            for (const auto& [reason, count] : finiteClassFailureReasons) {
                std::cerr << ' ' << reason << '=' << count;
            }
            std::cerr << '\n';
        }
        if (!contextTopSources.empty()) {
            std::cerr << "[RangeAnalysisTopSources]"
                      << " first=" << firstContextTopSource
                      << " first-function=" << firstContextTopFunction
                      << " first-line=" << firstContextTopLine;
            for (const auto& [source, count] : contextTopSources) {
                std::cerr << ' ' << source << '=' << count;
            }
            std::cerr << '\n';
        }
    }
    rangeAnalysisPerfStats = RangeAnalysisPerfStats{};
    finiteClassFailureReasons.clear();
    contextTopSources.clear();
    firstContextTopSource.clear();
    firstContextTopFunction.clear();
    firstContextTopLine = 0;
}

enum class ContextSummaryState : uint8_t {
    UNSEEN,
    COMPUTING,
    READY,
    FAILED
};

std::unordered_set<std::string> failedContextKeys;
size_t contextSummaryAnalysisCount{0};
size_t contextSummaryCacheHitCount{0};
size_t contextSummaryRecursiveHitCount{0};
size_t contextSummaryBudgetRejectCount{0};
size_t contextSummaryFailedCount{0};
size_t contextSummaryPeakDepth{0};
thread_local size_t contextSummaryAnalysisDepth{0};
thread_local std::vector<const Function*> contextSummaryFunctionStack;

std::unordered_map<const Value*, size_t> contextObjectIdentities;
std::mutex contextObjectIdentitiesMtx;
std::atomic<size_t> nextSyntheticContextObjectIdentity{1};
std::atomic<uint64_t> contextAnalysisWorkUnits{0};
std::atomic<bool> contextAnalysisBudgetExhausted{false};
enum class ContextBudgetExhaustionReason : uint8_t {
    NONE,
    WALL_TIME,
    WORK_UNITS
};
std::atomic<ContextBudgetExhaustionReason> contextBudgetExhaustionReason{
    ContextBudgetExhaustionReason::NONE};
std::atomic<uint64_t> contextBudgetFirstElapsedMilliseconds{0};
thread_local const char* contextAnalysisStage{"outside-context-analysis"};
std::string contextBudgetFirstStage;
std::string contextBudgetFirstFunction;
uint64_t contextBudgetFirstLine{0};
std::mutex contextBudgetTraceMtx;
std::chrono::steady_clock::time_point contextAnalysisBudgetStart =
    std::chrono::steady_clock::now();

class ScopedContextAnalysisStage {
public:
    explicit ScopedContextAnalysisStage(const char* stage)
        : previous(contextAnalysisStage)
    {
        contextAnalysisStage = stage;
    }

    ~ScopedContextAnalysisStage()
    {
        contextAnalysisStage = previous;
    }

private:
    const char* previous;
};

std::chrono::milliseconds GetContextAnalysisWallTime()
{
    auto defaultBudget =
        std::chrono::duration_cast<std::chrono::milliseconds>(MAX_CONTEXT_ANALYSIS_WALL_TIME);
    if (!IsRangeAnalysisPerfTraceEnabled()) {
        return defaultBudget;
    }
    auto configured = std::getenv("CANGJIE_RA_CONTEXT_BUDGET_MS");
    if (configured == nullptr) {
        return defaultBudget;
    }
    char* end = nullptr;
    auto milliseconds = std::strtoull(configured, &end, 10);
    if (end == configured || *end != '\0' || milliseconds == 0 ||
        milliseconds > static_cast<unsigned long long>(defaultBudget.count())) {
        return defaultBudget;
    }
    return std::chrono::milliseconds(milliseconds);
}

void MarkContextAnalysisBudgetExhausted(ContextBudgetExhaustionReason reason)
{
    bool expected = false;
    if (!contextAnalysisBudgetExhausted.compare_exchange_strong(
            expected, true, std::memory_order_relaxed)) {
        return;
    }
    contextBudgetExhaustionReason.store(reason, std::memory_order_relaxed);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - contextAnalysisBudgetStart);
    contextBudgetFirstElapsedMilliseconds.store(
        static_cast<uint64_t>(elapsed.count()), std::memory_order_relaxed);
    if (IsRangeAnalysisPerfCollectionEnabled()) {
        std::lock_guard<std::mutex> lock(contextBudgetTraceMtx);
        contextBudgetFirstStage = contextAnalysisStage;
        contextBudgetFirstFunction =
            overrideTraceFunction == nullptr ? "<none>" : overrideTraceFunction->GetIdentifier();
        contextBudgetFirstLine = overrideTraceCallsite == nullptr
            ? 0
            : overrideTraceCallsite->GetDebugLocation().GetBeginPos().line;
    }
}

size_t AllocateSyntheticContextObjectIdentity()
{
    constexpr size_t SYNTHETIC_ID_TAG = size_t{1} << (sizeof(size_t) * CHAR_BIT - 1);
    return SYNTHETIC_ID_TAG |
        nextSyntheticContextObjectIdentity.fetch_add(1, std::memory_order_relaxed);
}

bool IsContextAnalysisBudgetExhausted()
{
    if (contextAnalysisBudgetExhausted.load(std::memory_order_relaxed)) {
        return true;
    }
    if (std::chrono::steady_clock::now() - contextAnalysisBudgetStart >=
        GetContextAnalysisWallTime()) {
        MarkContextAnalysisBudgetExhausted(ContextBudgetExhaustionReason::WALL_TIME);
        return true;
    }
    return false;
}

bool ConsumeContextAnalysisWork(const Block* block)
{
    if (IsContextAnalysisBudgetExhausted()) {
        return false;
    }
    const auto expressionCount =
        block == nullptr ? size_t{0} : block->GetExpressions().size();
    const auto work = static_cast<uint64_t>(expressionCount + 1);
    const auto total =
        contextAnalysisWorkUnits.fetch_add(work, std::memory_order_relaxed) + work;
    if (total > MAX_TOTAL_CONTEXT_WORK_UNITS) {
        MarkContextAnalysisBudgetExhausted(ContextBudgetExhaustionReason::WORK_UNITS);
        return false;
    }
    return true;
}

size_t GetContextObjectIdentity(const Value* object)
{
    if (object == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(contextObjectIdentitiesMtx);
    auto found = contextObjectIdentities.find(object);
    return found == contextObjectIdentities.end()
        ? reinterpret_cast<size_t>(object)
        : found->second;
}

std::optional<size_t> FindBoundContextObjectIdentity(const Value* object)
{
    if (object == nullptr) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(contextObjectIdentitiesMtx);
    auto found = contextObjectIdentities.find(object);
    return found == contextObjectIdentities.end()
        ? std::nullopt
        : std::optional<size_t>{found->second};
}

bool SetContextObjectIdentity(const Value* object, size_t identity)
{
    if (object == nullptr || identity == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(contextObjectIdentitiesMtx);
    auto found = contextObjectIdentities.find(object);
    if (found != contextObjectIdentities.end()) {
        if (found->second != identity) {
            found->second = 0;
            return false;
        }
        return true;
    }
    if (contextObjectIdentities.size() >= MaxContextObjectIdentities()) {
        return false;
    }
    contextObjectIdentities.emplace(object, identity);
    return true;
}

void ClearContextObjectIdentities()
{
    std::lock_guard<std::mutex> lock(contextObjectIdentitiesMtx);
    contextObjectIdentities.clear();
    nextSyntheticContextObjectIdentity.store(1, std::memory_order_relaxed);
}

using BoundedLoopObservationMap = std::unordered_map<const Expression*, std::unique_ptr<ValueRange>>;
BoundedLoopObservationMap boundedLoopObservations;
BoundedLoopObservationMap boundedLoopPrefixObservations;
std::unordered_map<const RangeAnalysis*, BoundedLoopObservationMap>
    localBoundedLoopPrefixObservations;
std::unordered_map<const RangeAnalysis*, std::unordered_set<std::string>>
    completedBoundedLoopPrefixAttempts;
bool boundedLoopObservationsComplete{true};
std::mutex boundedLoopObservationsMtx;

std::optional<std::unique_ptr<ValueRange>> JoinBoundedLoopObservationRanges(
    const ValueRange& current, const ValueRange& incoming);

void CommitBoundedLoopObservations(const BoundedLoopObservationMap& observations)
{
    std::lock_guard<std::mutex> lock(boundedLoopObservationsMtx);
    if (!boundedLoopObservationsComplete) {
        return;
    }
    size_t newEntries = 0;
    for (const auto& [expression, range] : observations) {
        if (expression != nullptr && range != nullptr &&
            boundedLoopObservations.find(expression) == boundedLoopObservations.end()) {
            ++newEntries;
        }
    }
    if (newEntries >
        rangeAnalysisConfig.maxBoundedLoopObservations - boundedLoopObservations.size()) {
        boundedLoopObservationsComplete = false;
        return;
    }
    for (const auto& [expression, range] : observations) {
        if (expression == nullptr || range == nullptr) {
            continue;
        }
        auto current = boundedLoopObservations.find(expression);
        if (current == boundedLoopObservations.end()) {
            boundedLoopObservations.emplace(expression, range->Clone());
            if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                const auto& location = expression->GetDebugLocation();
                std::cerr << "[RangeAnalysisBoundedCommit] expression=" << expression
                          << " kind=" << static_cast<int>(expression->GetExprKind())
                          << " line=" << location.GetBeginPos().line
                          << " range=" << range->ToString() << '\n';
            }
            continue;
        }
        if (auto joined = JoinBoundedLoopObservationRanges(*current->second, *range); joined.has_value()) {
            current->second = std::move(joined.value());
        }
    }
}

void CommitBoundedLoopPrefixObservations(const BoundedLoopObservationMap& observations)
{
    std::lock_guard<std::mutex> lock(boundedLoopObservationsMtx);
    for (const auto& [expression, range] : observations) {
        if (expression == nullptr || range == nullptr) {
            continue;
        }
        auto current = boundedLoopPrefixObservations.find(expression);
        if (current == boundedLoopPrefixObservations.end()) {
            if (boundedLoopPrefixObservations.size() >=
                rangeAnalysisConfig.maxBoundedLoopObservations) {
                continue;
            }
            boundedLoopPrefixObservations.emplace(expression, range->Clone());
            continue;
        }
        if (auto joined = JoinBoundedLoopObservationRanges(*current->second, *range);
            joined.has_value()) {
            current->second = std::move(joined.value());
        }
    }
}

void MergeBoundedLoopCallContexts(
    BoundedLoopCallContextMap& destination, const BoundedLoopCallContextMap& source)
{
    size_t destinationCount = 0;
    for (const auto& [_, contexts] : destination) {
        destinationCount += contexts.keys.size();
    }
    for (const auto& [expression, contexts] : source) {
        auto& destinationContexts = destination[expression];
        destinationContexts.complete = destinationContexts.complete && contexts.complete;
        for (const auto& context : contexts.keys) {
            if (destinationContexts.keys.find(context) != destinationContexts.keys.end()) {
                continue;
            }
            if (destinationCount >= rangeAnalysisConfig.maxRecordedCallContexts) {
                destinationContexts.complete = false;
                break;
            }
            destinationContexts.keys.emplace(context);
            ++destinationCount;
        }
    }
}

void CommitBoundedLoopCallContexts(
    const std::string& parentContext, const BoundedLoopCallContextMap& contexts)
{
    std::lock_guard<std::mutex> lock(boundedLoopCallContextsMtx);
    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
        for (const auto& [expression, recorded] : contexts) {
            std::cerr << "[RangeAnalysisBoundedContexts] publish=" << expression
                      << " count=" << recorded.keys.size() << '\n';
        }
    }
    auto& destination = boundedLoopCallContexts[parentContext];
    for (const auto& [expression, recorded] : contexts) {
        auto& destinationContexts = destination[expression];
        destinationContexts.complete = destinationContexts.complete && recorded.complete;
        for (const auto& context : recorded.keys) {
            if (destinationContexts.keys.find(context) != destinationContexts.keys.end()) {
                continue;
            }
            if (analyzedCallContextCount >= rangeAnalysisConfig.maxRecordedCallContexts) {
                destinationContexts.complete = false;
                break;
            }
            destinationContexts.keys.emplace(context);
            ++analyzedCallContextCount;
        }
    }
}

void MarkBoundedLoopCallContextsIncomplete(
    BoundedLoopCallContextMap& contexts, const std::vector<const Expression*>& expressions)
{
    for (auto expression : expressions) {
        contexts[expression].complete = false;
    }
}

void MarkBoundedLoopCallContextsIncomplete(
    const std::string& parentContext, const std::vector<const Expression*>& expressions)
{
    std::lock_guard<std::mutex> lock(boundedLoopCallContextsMtx);
    MarkBoundedLoopCallContextsIncomplete(boundedLoopCallContexts[parentContext], expressions);
}

void RecordAnalyzedCallContext(
    const std::string& parentContext, const Expression* expression, const std::string& childContext)
{
    if (expression == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(boundedLoopCallContextsMtx);
    auto& contexts = analyzedCallContexts[parentContext][expression];
    if (contexts.keys.find(childContext) != contexts.keys.end()) {
        return;
    }
    if (analyzedCallContextCount >= rangeAnalysisConfig.maxRecordedCallContexts) {
        contexts.complete = false;
        analyzedCallContextRecordingComplete = false;
        return;
    }
    contexts.keys.emplace(childContext);
    ++analyzedCallContextCount;
}

void RecordFullyModeledCallContext(
    const std::string& parentContext, const Expression* expression)
{
    if (expression == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(boundedLoopCallContextsMtx);
    (void)analyzedCallContexts[parentContext][expression];
}

void RecordUnmodeledCallContext(
    const std::string& parentContext, const Expression* expression)
{
    if (expression == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(boundedLoopCallContextsMtx);
    analyzedCallContexts[parentContext][expression].complete = false;
    if (boundedLoopCallContextRecorder != nullptr) {
        (*boundedLoopCallContextRecorder)[expression].complete = false;
    }
}

RecordedCallContexts GetRecordedChildContexts(const std::string& parentContext)
{
    std::lock_guard<std::mutex> lock(boundedLoopCallContextsMtx);
    RecordedCallContexts result;
    result.complete = analyzedCallContextRecordingComplete;
    auto analyzedParent = analyzedCallContexts.find(parentContext);
    auto boundedParent = boundedLoopCallContexts.find(parentContext);
    std::unordered_set<const Expression*> expressions;
    if (analyzedParent != analyzedCallContexts.end()) {
        for (const auto& [expression, _] : analyzedParent->second) {
            expressions.emplace(expression);
        }
    }
    if (boundedParent != boundedLoopCallContexts.end()) {
        for (const auto& [expression, _] : boundedParent->second) {
            expressions.emplace(expression);
        }
    }
    for (auto expression : expressions) {
        const RecordedCallContexts* bounded = nullptr;
        const RecordedCallContexts* analyzed = nullptr;
        if (boundedParent != boundedLoopCallContexts.end()) {
            auto found = boundedParent->second.find(expression);
            if (found != boundedParent->second.end()) {
                bounded = &found->second;
            }
        }
        if (analyzedParent != analyzedCallContexts.end()) {
            auto found = analyzedParent->second.find(expression);
            if (found != analyzedParent->second.end()) {
                analyzed = &found->second;
            }
        }
        const auto* selected =
            bounded != nullptr && bounded->complete && !bounded->keys.empty()
            ? bounded
            : analyzed;
        if (selected != nullptr) {
            result.complete = result.complete && selected->complete;
            result.keys.insert(selected->keys.begin(), selected->keys.end());
        } else if (bounded != nullptr && !bounded->complete) {
            result.complete = false;
        }
    }
    return result;
}

void ClearBoundedLoopCallContexts()
{
    std::lock_guard<std::mutex> lock(boundedLoopCallContextsMtx);
    analyzedCallContexts.clear();
    boundedLoopCallContexts.clear();
    analyzedCallContextCount = 0;
    analyzedCallContextRecordingComplete = true;
}

std::string BuildRootAnalysisContextKey(const Function* function)
{
    std::stringstream key;
    key << "$root@" << static_cast<const void*>(function);
    return key.str();
}

struct StructArrayLiteralInfo {
    Value* rawArray{nullptr};
    Value* start{nullptr};
    Value* length{nullptr};
};

std::unordered_map<Value*, StructArrayLiteralInfo> structArrayLiteralInfos;
std::mutex aggregateLiteralMtx;

std::vector<SInt> NormalizeExactIntValues(std::vector<SInt> values)
{
    std::sort(values.begin(), values.end(), [](const SInt& lhs, const SInt& rhs) {
        if (lhs.Width() != rhs.Width()) {
            return static_cast<unsigned>(lhs.Width()) < static_cast<unsigned>(rhs.Width());
        }
        return lhs.UVal() < rhs.UVal();
    });
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

std::optional<std::vector<SInt>> NormalizeExactIntSet(std::vector<SInt> values)
{
    values = NormalizeExactIntValues(std::move(values));
    if (values.empty() || ExceedsExactIntSetLimit(values.size())) {
        return std::nullopt;
    }
    return values;
}

unsigned __int128 GcdWide(unsigned __int128 lhs, unsigned __int128 rhs)
{
    while (rhs != 0) {
        auto remainder = lhs % rhs;
        lhs = rhs;
        rhs = remainder;
    }
    return lhs;
}

__int128 ToMathematicalInteger(const SInt& value, bool isUnsigned)
{
    return isUnsigned ? static_cast<__int128>(value.UVal()) : static_cast<__int128>(value.SVal());
}

unsigned __int128 AbsoluteDifference(__int128 lhs, __int128 rhs)
{
    return lhs >= rhs ? static_cast<unsigned __int128>(lhs - rhs)
                      : static_cast<unsigned __int128>(rhs - lhs);
}

uint64_t CanonicalResidue(__int128 value, uint64_t stride)
{
    CJC_ASSERT(stride > 1);
    auto wideStride = static_cast<__int128>(stride);
    auto remainder = value % wideStride;
    if (remainder < 0) {
        remainder += wideStride;
    }
    return static_cast<uint64_t>(remainder);
}

std::optional<SIntCongruence> NormalizeCongruence(std::optional<SIntCongruence> congruence)
{
    if (!congruence.has_value() || !congruence->IsUseful()) {
        return std::nullopt;
    }
    congruence->residue %= congruence->stride;
    return congruence;
}

uint64_t GetSIntWidthMask(IntWidth width)
{
    return width == IntWidth::I64 ? UINT64_MAX : ((uint64_t{1} << static_cast<unsigned>(width)) - 1U);
}

std::optional<SIntKnownBits> NormalizeKnownBits(
    std::optional<SIntKnownBits> knownBits, IntWidth width)
{
    if (!knownBits.has_value()) {
        return std::nullopt;
    }
    auto mask = GetSIntWidthMask(width);
    knownBits->knownZero &= mask;
    knownBits->knownOne &= mask;
    if ((knownBits->knownZero & knownBits->knownOne) != 0 || !knownBits->IsUseful()) {
        return std::nullopt;
    }
    return knownBits;
}

std::optional<SIntKnownBits> InferKnownBitsFromExactValues(
    const std::optional<std::vector<SInt>>& exactValues, IntWidth width)
{
    if (!exactValues.has_value() || exactValues->empty()) {
        return std::nullopt;
    }
    auto mask = GetSIntWidthMask(width);
    uint64_t commonOne = mask;
    uint64_t anyOne = 0;
    for (const auto& value : *exactValues) {
        auto raw = value.UVal() & mask;
        commonOne &= raw;
        anyOne |= raw;
    }
    return NormalizeKnownBits(SIntKnownBits{(~anyOne) & mask, commonOne}, width);
}

std::optional<SIntKnownBits> InferKnownBitsFromCongruence(
    const std::optional<SIntCongruence>& congruence, IntWidth width)
{
    auto normalized = NormalizeCongruence(congruence);
    if (!normalized.has_value()) {
        return std::nullopt;
    }
    auto fixedLowBits = static_cast<unsigned>(__builtin_ctzll(normalized->stride));
    if (fixedLowBits == 0) {
        return std::nullopt;
    }
    auto lowMask = (uint64_t{1} << fixedLowBits) - 1U;
    auto knownOne = normalized->residue & lowMask;
    return NormalizeKnownBits(SIntKnownBits{(~knownOne) & lowMask, knownOne}, width);
}

std::optional<SIntKnownBits> InferKnownBitsFromInterval(const SIntDomain& domain)
{
    if (domain.IsBottom()) {
        return std::nullopt;
    }
    const auto& interval = domain.NumericBound();
    uint64_t minimum = 0;
    uint64_t maximum = 0;
    if (domain.IsUnsigned()) {
        if (interval.IsWrappedSet()) {
            return std::nullopt;
        }
        minimum = interval.UMinValue().UVal();
        maximum = interval.UMaxValue().UVal();
    } else {
        if (interval.IsSignWrappedSet()) {
            return std::nullopt;
        }
        auto signedMinimum = interval.SMinValue();
        auto signedMaximum = interval.SMaxValue();
        if (signedMinimum.Slt(0) != signedMaximum.Slt(0)) {
            return std::nullopt;
        }
        minimum = signedMinimum.UVal();
        maximum = signedMaximum.UVal();
    }

    const auto width = domain.Width();
    const auto widthMask = GetSIntWidthMask(width);
    minimum &= widthMask;
    maximum &= widthMask;
    auto varyingMask = minimum ^ maximum;
    varyingMask |= varyingMask >> 1U;
    varyingMask |= varyingMask >> 2U;
    varyingMask |= varyingMask >> 4U;
    varyingMask |= varyingMask >> 8U;
    varyingMask |= varyingMask >> 16U;
    varyingMask |= varyingMask >> 32U;
    const auto fixedMask = widthMask & ~varyingMask;
    return NormalizeKnownBits(
        SIntKnownBits{(~minimum) & fixedMask, minimum & fixedMask}, width);
}

std::optional<SIntKnownBits> IntersectSIntKnownBits(const std::optional<SIntKnownBits>& lhs,
    const std::optional<SIntKnownBits>& rhs, IntWidth width, bool& compatible)
{
    compatible = true;
    auto normalizedLhs = NormalizeKnownBits(lhs, width);
    auto normalizedRhs = NormalizeKnownBits(rhs, width);
    if (!normalizedLhs.has_value()) {
        return normalizedRhs;
    }
    if (!normalizedRhs.has_value()) {
        return normalizedLhs;
    }
    SIntKnownBits result{normalizedLhs->knownZero | normalizedRhs->knownZero,
        normalizedLhs->knownOne | normalizedRhs->knownOne};
    if ((result.knownZero & result.knownOne) != 0) {
        compatible = false;
        return std::nullopt;
    }
    return NormalizeKnownBits(result, width);
}

std::optional<SIntKnownBits> GetEffectiveKnownBits(const SIntDomain& domain,
    const std::optional<std::vector<SInt>>& exactValues,
    const std::optional<SIntCongruence>& congruence,
    const std::optional<SIntKnownBits>& explicitKnownBits)
{
    bool compatible = true;
    auto result = IntersectSIntKnownBits(explicitKnownBits,
        InferKnownBitsFromExactValues(exactValues, domain.Width()), domain.Width(), compatible);
    if (!compatible) {
        return std::nullopt;
    }
    result = IntersectSIntKnownBits(result,
        InferKnownBitsFromCongruence(congruence, domain.Width()), domain.Width(), compatible);
    if (!compatible) {
        return std::nullopt;
    }
    return IntersectSIntKnownBits(
        result, InferKnownBitsFromInterval(domain), domain.Width(), compatible);
}

std::optional<SIntKnownBits> MergeSIntKnownBitsForUnion(const SIntDomain& lhsDomain,
    const std::optional<SIntKnownBits>& lhs, const SIntDomain& rhsDomain,
    const std::optional<SIntKnownBits>& rhs)
{
    if (lhsDomain.IsBottom()) {
        return NormalizeKnownBits(rhs, rhsDomain.Width());
    }
    if (rhsDomain.IsBottom()) {
        return NormalizeKnownBits(lhs, lhsDomain.Width());
    }
    if (lhsDomain.Width() != rhsDomain.Width() || lhsDomain.IsUnsigned() != rhsDomain.IsUnsigned()) {
        return std::nullopt;
    }
    auto normalizedLhs = NormalizeKnownBits(lhs, lhsDomain.Width());
    auto normalizedRhs = NormalizeKnownBits(rhs, rhsDomain.Width());
    if (!normalizedLhs.has_value() || !normalizedRhs.has_value()) {
        return std::nullopt;
    }
    return NormalizeKnownBits(SIntKnownBits{normalizedLhs->knownZero & normalizedRhs->knownZero,
        normalizedLhs->knownOne & normalizedRhs->knownOne}, lhsDomain.Width());
}

std::optional<SIntCongruence> InferCongruenceFromKnownBits(
    const std::optional<SIntKnownBits>& knownBits, IntWidth width)
{
    auto normalized = NormalizeKnownBits(knownBits, width);
    if (!normalized.has_value()) {
        return std::nullopt;
    }
    auto fixed = normalized->knownZero | normalized->knownOne;
    unsigned fixedLowBits = 0;
    while (fixedLowBits < static_cast<unsigned>(width) &&
        (fixed & (uint64_t{1} << fixedLowBits)) != 0) {
        ++fixedLowBits;
    }
    if (fixedLowBits == 0 || fixedLowBits >= 64) {
        return std::nullopt;
    }
    auto stride = uint64_t{1} << fixedLowBits;
    return SIntCongruence{stride, normalized->knownOne & (stride - 1U)};
}

SIntDomain DomainFromKnownBits(
    const std::optional<SIntKnownBits>& knownBits, IntWidth width, bool isUnsigned)
{
    auto normalized = NormalizeKnownBits(knownBits, width);
    if (!normalized.has_value()) {
        return SIntDomain::Top(width, isUnsigned);
    }
    auto mask = GetSIntWidthMask(width);
    auto unknown = (~(normalized->knownZero | normalized->knownOne)) & mask;
    auto signBit = uint64_t{1} << (static_cast<unsigned>(width) - 1U);
    if (!isUnsigned && (unknown & signBit) != 0) {
        return SIntDomain::Top(width, false);
    }
    SInt minimum{width, normalized->knownOne};
    SInt maximum{width, normalized->knownOne | unknown};
    auto lower = ConstantRange::From(RelationalOperation::GE, minimum, !isUnsigned);
    auto upper = ConstantRange::From(RelationalOperation::LE, maximum, !isUnsigned);
    return SIntDomain{lower.IntersectWith(upper, PreferFromBool(isUnsigned)), isUnsigned};
}

struct CongruenceSummary {
    bool constrained{false};
    unsigned __int128 modulus{1};
    __int128 representative{0};
};

CongruenceSummary DescribeCongruence(const std::optional<std::vector<SInt>>& exactValues,
    const std::optional<SIntCongruence>& congruence, bool isUnsigned)
{
    if (exactValues.has_value() && !exactValues->empty()) {
        auto representative = ToMathematicalInteger(exactValues->front(), isUnsigned);
        unsigned __int128 modulus = 0;
        for (size_t i = 1; i < exactValues->size(); ++i) {
            modulus = GcdWide(modulus,
                AbsoluteDifference(representative, ToMathematicalInteger((*exactValues)[i], isUnsigned)));
        }
        if (modulus == 1) {
            return {};
        }
        return CongruenceSummary{true, modulus, representative};
    }
    auto normalized = NormalizeCongruence(congruence);
    if (!normalized.has_value()) {
        return {};
    }
    return CongruenceSummary{true, normalized->stride, static_cast<__int128>(normalized->residue)};
}

std::optional<SIntCongruence> InferCongruenceFromExactValues(
    const std::optional<std::vector<SInt>>& exactValues, bool isUnsigned)
{
    auto summary = DescribeCongruence(exactValues, std::nullopt, isUnsigned);
    if (!summary.constrained || summary.modulus <= 1 || summary.modulus > UINT64_MAX) {
        return std::nullopt;
    }
    auto stride = static_cast<uint64_t>(summary.modulus);
    return SIntCongruence{stride, CanonicalResidue(summary.representative, stride)};
}

std::optional<SIntCongruence> MergeSIntCongruencesForUnion(const SIntDomain& lhsDomain,
    const std::optional<std::vector<SInt>>& lhsExactValues, const std::optional<SIntCongruence>& lhsCongruence,
    const SIntDomain& rhsDomain, const std::optional<std::vector<SInt>>& rhsExactValues,
    const std::optional<SIntCongruence>& rhsCongruence)
{
    if (lhsDomain.IsBottom()) {
        auto inferred = InferCongruenceFromExactValues(rhsExactValues, rhsDomain.IsUnsigned());
        return inferred.has_value() ? inferred : NormalizeCongruence(rhsCongruence);
    }
    if (rhsDomain.IsBottom()) {
        auto inferred = InferCongruenceFromExactValues(lhsExactValues, lhsDomain.IsUnsigned());
        return inferred.has_value() ? inferred : NormalizeCongruence(lhsCongruence);
    }
    if (lhsDomain.Width() != rhsDomain.Width() || lhsDomain.IsUnsigned() != rhsDomain.IsUnsigned()) {
        return std::nullopt;
    }
    auto lhs = DescribeCongruence(lhsExactValues, lhsCongruence, lhsDomain.IsUnsigned());
    auto rhs = DescribeCongruence(rhsExactValues, rhsCongruence, rhsDomain.IsUnsigned());
    if (!lhs.constrained || !rhs.constrained) {
        return std::nullopt;
    }
    auto modulus = GcdWide(lhs.modulus, rhs.modulus);
    modulus = GcdWide(modulus, AbsoluteDifference(lhs.representative, rhs.representative));
    if (modulus <= 1 || modulus > UINT64_MAX) {
        return std::nullopt;
    }
    auto stride = static_cast<uint64_t>(modulus);
    return SIntCongruence{stride, CanonicalResidue(lhs.representative, stride)};
}

std::optional<SIntCongruence> IntersectSIntCongruences(const std::optional<SIntCongruence>& lhs,
    const std::optional<SIntCongruence>& rhs, bool& compatible)
{
    compatible = true;
    auto normalizedLhs = NormalizeCongruence(lhs);
    auto normalizedRhs = NormalizeCongruence(rhs);
    if (!normalizedLhs.has_value()) {
        return normalizedRhs;
    }
    if (!normalizedRhs.has_value()) {
        return normalizedLhs;
    }
    auto common = std::gcd(normalizedLhs->stride, normalizedRhs->stride);
    auto residueDifference = normalizedLhs->residue >= normalizedRhs->residue
        ? normalizedLhs->residue - normalizedRhs->residue
        : normalizedRhs->residue - normalizedLhs->residue;
    if (residueDifference % common != 0) {
        compatible = false;
        return std::nullopt;
    }
    if (normalizedLhs->stride % normalizedRhs->stride == 0 &&
        normalizedLhs->residue % normalizedRhs->stride == normalizedRhs->residue) {
        return normalizedLhs;
    }
    if (normalizedRhs->stride % normalizedLhs->stride == 0 &&
        normalizedRhs->residue % normalizedLhs->stride == normalizedLhs->residue) {
        return normalizedRhs;
    }
    // The exact CRT intersection may require an unrepresentable lcm.  Keeping
    // the current component is a sound no-op for narrowing.
    return normalizedLhs;
}

std::optional<std::vector<SInt>> MergeExactIntSets(
    const std::optional<std::vector<SInt>>& lhs, const std::optional<std::vector<SInt>>& rhs)
{
    if (!lhs.has_value() || !rhs.has_value()) {
        return std::nullopt;
    }
    std::vector<SInt> values;
    values.reserve(lhs->size() + rhs->size());
    values.insert(values.end(), lhs->begin(), lhs->end());
    values.insert(values.end(), rhs->begin(), rhs->end());
    return NormalizeExactIntSet(std::move(values));
}

bool ExactValueSatisfiesDomain(const SInt& value, const SIntDomain& domain)
{
    SIntDomain singleton{ConstantRange{value}, domain.IsUnsigned()};
    return !SIntDomain::Intersects(singleton, domain).IsBottom();
}

constexpr size_t MAX_EXCLUDED_SINT_VALUES = 8;
constexpr size_t MAX_DISJOINT_SINT_INTERVALS = 4;

std::optional<std::vector<SInt>> NormalizeExcludedSIntValues(
    std::vector<SInt> values, const SIntDomain& domain)
{
    values = NormalizeExactIntValues(std::move(values));
    values.erase(std::remove_if(values.begin(), values.end(), [&domain](const SInt& value) {
        return value.Width() != domain.Width() || !ExactValueSatisfiesDomain(value, domain);
    }), values.end());
    if (values.empty() || values.size() > MAX_EXCLUDED_SINT_VALUES) {
        return std::nullopt;
    }
    return values;
}

std::optional<std::vector<SIntIntervalFragment>> NormalizeSIntIntervalFragments(
    std::vector<SIntIntervalFragment> fragments, const SIntDomain& domain)
{
    if (fragments.empty() || domain.IsBottom()) {
        return std::nullopt;
    }
    const auto mathematical = [&domain](const SInt& value) {
        return ToMathematicalInteger(value, domain.IsUnsigned());
    };
    const auto& numeric = domain.NumericBound();
    const bool hasConvexBounds = numeric.IsFullSet() ||
        !(domain.IsUnsigned() ? numeric.IsWrappedSet() : numeric.IsSignWrappedSet());
    const auto domainLower = domain.IsUnsigned()
        ? static_cast<__int128>(numeric.IsFullSet()
                ? SInt::UMinValue(domain.Width()).UVal()
                : numeric.MinValue(true).UVal())
        : static_cast<__int128>(numeric.IsFullSet()
                ? SInt::SMinValue(domain.Width()).SVal()
                : numeric.MinValue(false).SVal());
    const auto domainUpper = domain.IsUnsigned()
        ? static_cast<__int128>(numeric.IsFullSet()
                ? SInt::UMaxValue(domain.Width()).UVal()
                : numeric.MaxValue(true).UVal())
        : static_cast<__int128>(numeric.IsFullSet()
                ? SInt::SMaxValue(domain.Width()).SVal()
                : numeric.MaxValue(false).SVal());
    fragments.erase(std::remove_if(fragments.begin(), fragments.end(),
        [&](SIntIntervalFragment& fragment) {
            if (fragment.stride <= 1) {
                fragment.stride = 1;
                fragment.residue = 0;
            } else {
                fragment.residue %= fragment.stride;
            }
            if (fragment.lower.Width() != domain.Width() ||
                fragment.upper.Width() != domain.Width() ||
                mathematical(fragment.lower) > mathematical(fragment.upper)) {
                return true;
            }
            if (!hasConvexBounds) {
                return !ExactValueSatisfiesDomain(fragment.lower, domain) ||
                    !ExactValueSatisfiesDomain(fragment.upper, domain);
            }
            auto lower = std::max(mathematical(fragment.lower), domainLower);
            auto upper = std::min(mathematical(fragment.upper), domainUpper);
            if (lower > upper) {
                return true;
            }
            fragment.lower = SInt{domain.Width(), static_cast<uint64_t>(lower)};
            fragment.upper = SInt{domain.Width(), static_cast<uint64_t>(upper)};
            return false;
        }), fragments.end());

    // Keep each fragment's endpoints on its own progression. This matters for
    // signed negative ranges as well, so align in the mathematical domain and
    // only then convert back to the fixed-width representation.
    fragments.erase(std::remove_if(fragments.begin(), fragments.end(),
        [&](SIntIntervalFragment& fragment) {
            if (fragment.stride == 1) {
                return false;
            }
            const auto stride = static_cast<__int128>(fragment.stride);
            const auto residue = static_cast<__int128>(fragment.residue);
            auto lower = mathematical(fragment.lower);
            auto upper = mathematical(fragment.upper);
            auto lowerResidue = lower % stride;
            if (lowerResidue < 0) {
                lowerResidue += stride;
            }
            auto upperResidue = upper % stride;
            if (upperResidue < 0) {
                upperResidue += stride;
            }
            lower += (residue - lowerResidue + stride) % stride;
            upper -= (upperResidue - residue + stride) % stride;
            if (lower > upper) {
                return true;
            }
            fragment.lower = SInt{domain.Width(), static_cast<uint64_t>(lower)};
            fragment.upper = SInt{domain.Width(), static_cast<uint64_t>(upper)};
            return false;
        }), fragments.end());
    std::sort(fragments.begin(), fragments.end(),
        [&](const SIntIntervalFragment& lhs, const SIntIntervalFragment& rhs) {
            if (mathematical(lhs.lower) != mathematical(rhs.lower)) {
                return mathematical(lhs.lower) < mathematical(rhs.lower);
            }
            if (mathematical(lhs.upper) != mathematical(rhs.upper)) {
                return mathematical(lhs.upper) < mathematical(rhs.upper);
            }
            if (lhs.stride != rhs.stride) {
                return lhs.stride < rhs.stride;
            }
            return lhs.residue < rhs.residue;
        });

    std::vector<SIntIntervalFragment> normalized;
    normalized.reserve(fragments.size());
    for (const auto& fragment : fragments) {
        const bool sameProgression = !normalized.empty() &&
            fragment.stride == normalized.back().stride &&
            fragment.residue == normalized.back().residue;
        if (!sameProgression || mathematical(fragment.lower) >
                mathematical(normalized.back().upper) +
                    static_cast<__int128>(fragment.stride)) {
            normalized.emplace_back(fragment);
            continue;
        }
        if (mathematical(fragment.upper) > mathematical(normalized.back().upper)) {
            normalized.back().upper = fragment.upper;
        }
    }

    const auto contains = [&](const SIntIntervalFragment& outer,
                              const SIntIntervalFragment& inner) {
        if (mathematical(outer.lower) > mathematical(inner.lower) ||
            mathematical(outer.upper) < mathematical(inner.upper)) {
            return false;
        }
        if (outer.stride == 1) {
            return true;
        }
        if (inner.lower == inner.upper) {
            return CanonicalResidue(
                mathematical(inner.lower), outer.stride) == outer.residue;
        }
        return inner.stride % outer.stride == 0 &&
            inner.residue % outer.stride == outer.residue;
    };
    std::vector<SIntIntervalFragment> reduced;
    reduced.reserve(normalized.size());
    for (const auto& fragment : normalized) {
        if (std::any_of(reduced.begin(), reduced.end(),
                [&](const auto& existing) { return contains(existing, fragment); })) {
            continue;
        }
        reduced.erase(std::remove_if(reduced.begin(), reduced.end(),
            [&](const auto& existing) { return contains(fragment, existing); }), reduced.end());
        reduced.emplace_back(fragment);
    }
    std::sort(reduced.begin(), reduced.end(),
        [&](const SIntIntervalFragment& lhs, const SIntIntervalFragment& rhs) {
            return mathematical(lhs.lower) < mathematical(rhs.lower);
        });
    if (reduced.empty() || reduced.size() > MAX_DISJOINT_SINT_INTERVALS) {
        return std::nullopt;
    }
    return reduced;
}

std::optional<std::vector<SIntIntervalFragment>> RefineSIntIntervalFragmentsByCongruence(
    std::optional<std::vector<SIntIntervalFragment>> fragments,
    const std::optional<SIntCongruence>& congruence, const SIntDomain& domain)
{
    auto normalizedCongruence = NormalizeCongruence(congruence);
    if (!fragments.has_value() || !normalizedCongruence.has_value()) {
        return fragments;
    }
    const auto mathematical = [&domain](const SInt& value) {
        return ToMathematicalInteger(value, domain.IsUnsigned());
    };
    fragments->erase(std::remove_if(fragments->begin(), fragments->end(),
        [&](SIntIntervalFragment& fragment) {
            if (fragment.lower == fragment.upper) {
                return CanonicalResidue(mathematical(fragment.lower),
                           normalizedCongruence->stride) !=
                    normalizedCongruence->residue;
            }
            if (fragment.stride <= 1) {
                fragment.stride = normalizedCongruence->stride;
                fragment.residue = normalizedCongruence->residue;
                return false;
            }
            const auto common = std::gcd(
                fragment.stride, normalizedCongruence->stride);
            const auto difference = fragment.residue >= normalizedCongruence->residue
                ? fragment.residue - normalizedCongruence->residue
                : normalizedCongruence->residue - fragment.residue;
            if (difference % common != 0) {
                return true;
            }
            if (normalizedCongruence->stride % fragment.stride == 0 &&
                normalizedCongruence->residue % fragment.stride == fragment.residue) {
                fragment.stride = normalizedCongruence->stride;
                fragment.residue = normalizedCongruence->residue;
            }
            return false;
        }), fragments->end());
    return NormalizeSIntIntervalFragments(std::move(*fragments), domain);
}

std::optional<std::vector<SIntIntervalFragment>> GetSIntIntervalFragmentsForUnion(
    const SIntRange& range)
{
    if (range.GetIntervalFragments().has_value()) {
        return range.GetIntervalFragments();
    }
    const auto& domain = range.GetVal();
    const auto& numeric = domain.NumericBound();
    if (domain.IsBottom()) {
        return std::vector<SIntIntervalFragment>{};
    }
    if (numeric.IsFullSet() || numeric.IsEmptySet() ||
        (domain.IsUnsigned() ? numeric.IsWrappedSet() : numeric.IsSignWrappedSet())) {
        return std::nullopt;
    }
    if (range.GetExactValues().has_value() && range.GetExactValues()->size() == 1) {
        return std::vector<SIntIntervalFragment>{{
            range.GetExactValues()->front(), range.GetExactValues()->front(), 1, 0}};
    }
    auto congruence = NormalizeCongruence(range.GetCongruence());
    return std::vector<SIntIntervalFragment>{{
        numeric.MinValue(domain.IsUnsigned()), numeric.MaxValue(domain.IsUnsigned()),
        congruence.has_value() ? congruence->stride : 1,
        congruence.has_value() ? congruence->residue : 0}};
}

std::optional<std::vector<SIntIntervalFragment>> MergeSIntIntervalFragmentsForUnion(
    const SIntRange& lhs, const SIntRange& rhs, const SIntDomain& joinedDomain)
{
    if (lhs.GetVal().IsBottom()) {
        return rhs.GetIntervalFragments();
    }
    if (rhs.GetVal().IsBottom()) {
        return lhs.GetIntervalFragments();
    }

    // Ordinary joins overwhelmingly combine two convex ranges. Detect the
    // common overlapping/adjacent case without constructing and sorting two
    // temporary vectors; allocate fragment storage only when a real gap is
    // present or an operand already carries a non-convex union.
    if (!lhs.GetIntervalFragments().has_value() && !rhs.GetIntervalFragments().has_value()) {
        const auto& lhsDomain = lhs.GetVal();
        const auto& rhsDomain = rhs.GetVal();
        const auto& lhsNumeric = lhsDomain.NumericBound();
        const auto& rhsNumeric = rhsDomain.NumericBound();
        const bool lhsUnsupported = lhsNumeric.IsFullSet() || lhsNumeric.IsEmptySet() ||
            (lhsDomain.IsUnsigned() ? lhsNumeric.IsWrappedSet() : lhsNumeric.IsSignWrappedSet());
        const bool rhsUnsupported = rhsNumeric.IsFullSet() || rhsNumeric.IsEmptySet() ||
            (rhsDomain.IsUnsigned() ? rhsNumeric.IsWrappedSet() : rhsNumeric.IsSignWrappedSet());
        if (lhsUnsupported || rhsUnsupported || lhsDomain.Width() != rhsDomain.Width() ||
            lhsDomain.IsUnsigned() != rhsDomain.IsUnsigned()) {
            return std::nullopt;
        }

        const auto lhsLower = ToMathematicalInteger(
            lhsNumeric.MinValue(lhsDomain.IsUnsigned()), lhsDomain.IsUnsigned());
        const auto lhsUpper = ToMathematicalInteger(
            lhsNumeric.MaxValue(lhsDomain.IsUnsigned()), lhsDomain.IsUnsigned());
        const auto rhsLower = ToMathematicalInteger(
            rhsNumeric.MinValue(rhsDomain.IsUnsigned()), rhsDomain.IsUnsigned());
        const auto rhsUpper = ToMathematicalInteger(
            rhsNumeric.MaxValue(rhsDomain.IsUnsigned()), rhsDomain.IsUnsigned());
        if (lhsLower <= rhsUpper + 1 && rhsLower <= lhsUpper + 1) {
            return std::nullopt;
        }

        auto lhsFragments = GetSIntIntervalFragmentsForUnion(lhs);
        auto rhsFragments = GetSIntIntervalFragmentsForUnion(rhs);
        CJC_ASSERT(lhsFragments.has_value() && rhsFragments.has_value());
        lhsFragments->insert(
            lhsFragments->end(), rhsFragments->begin(), rhsFragments->end());
        return NormalizeSIntIntervalFragments(std::move(*lhsFragments), joinedDomain);
    }

    auto lhsFragments = GetSIntIntervalFragmentsForUnion(lhs);
    auto rhsFragments = GetSIntIntervalFragmentsForUnion(rhs);
    if (!lhsFragments.has_value() || !rhsFragments.has_value()) {
        return std::nullopt;
    }
    lhsFragments->insert(
        lhsFragments->end(), rhsFragments->begin(), rhsFragments->end());
    return NormalizeSIntIntervalFragments(std::move(*lhsFragments), joinedDomain);
}

std::optional<std::vector<SIntIntervalFragment>> SelectSIntIntervalFragmentsForNarrowing(
    const SIntRange& current, const SIntRange& candidate, const SIntDomain& intersection)
{
    // A narrowing candidate is a fresh sound evaluation under the current
    // over-approximation. Prefer its non-convex partition when available; if
    // it is convex, retain an older partition only when it still lies inside
    // the narrowed interval. Intersecting two fragment unions exactly is not
    // required for soundness here: either partition is already a superset of
    // the concrete intersection.
    if (candidate.GetIntervalFragments().has_value()) {
        return NormalizeSIntIntervalFragments(
            *candidate.GetIntervalFragments(), intersection);
    }
    if (current.GetIntervalFragments().has_value()) {
        return NormalizeSIntIntervalFragments(
            *current.GetIntervalFragments(), intersection);
    }
    return std::nullopt;
}

bool IsExplicitlyExcluded(const std::optional<std::vector<SInt>>& excludedValues, const SInt& value)
{
    return excludedValues.has_value() &&
        std::find(excludedValues->begin(), excludedValues->end(), value) != excludedValues->end();
}

bool SIntRangeMayContain(const SIntDomain& domain,
    const std::optional<std::vector<SInt>>& excludedValues, const SInt& value)
{
    return ExactValueSatisfiesDomain(value, domain) && !IsExplicitlyExcluded(excludedValues, value);
}

void AddSmallGapExclusions(const SIntDomain& lhsDomain, const SIntDomain& rhsDomain,
    std::vector<SInt>& candidates)
{
    if (lhsDomain.Width() != rhsDomain.Width() || lhsDomain.IsUnsigned() != rhsDomain.IsUnsigned()) {
        return;
    }
    const auto& lhs = lhsDomain.NumericBound();
    const auto& rhs = rhsDomain.NumericBound();
    if (lhs.IsEmptySet() || rhs.IsEmptySet() || lhs.IsFullSet() || rhs.IsFullSet() ||
        (lhsDomain.IsUnsigned() ? lhs.IsWrappedSet() || rhs.IsWrappedSet()
                                : lhs.IsSignWrappedSet() || rhs.IsSignWrappedSet())) {
        return;
    }
    auto lhsMin = ToMathematicalInteger(lhs.MinValue(lhsDomain.IsUnsigned()), lhsDomain.IsUnsigned());
    auto lhsMax = ToMathematicalInteger(lhs.MaxValue(lhsDomain.IsUnsigned()), lhsDomain.IsUnsigned());
    auto rhsMin = ToMathematicalInteger(rhs.MinValue(rhsDomain.IsUnsigned()), rhsDomain.IsUnsigned());
    auto rhsMax = ToMathematicalInteger(rhs.MaxValue(rhsDomain.IsUnsigned()), rhsDomain.IsUnsigned());
    auto gapLower = lhsMax < rhsMin ? lhsMax + 1 : rhsMax < lhsMin ? rhsMax + 1 : __int128{1};
    auto gapUpper = lhsMax < rhsMin ? rhsMin - 1 : rhsMax < lhsMin ? lhsMin - 1 : __int128{0};
    if (gapLower > gapUpper) {
        return;
    }
    auto gapSize = gapUpper - gapLower + 1;
    if (gapSize > static_cast<__int128>(MAX_EXCLUDED_SINT_VALUES) ||
        candidates.size() + static_cast<size_t>(gapSize) > MAX_EXCLUDED_SINT_VALUES) {
        return;
    }
    for (auto value = gapLower; value <= gapUpper; ++value) {
        candidates.emplace_back(lhsDomain.Width(), static_cast<uint64_t>(value));
    }
}

std::optional<std::vector<SInt>> MergeExcludedSIntValuesForUnion(const SIntDomain& lhsDomain,
    const std::optional<std::vector<SInt>>& lhsExcluded, const SIntDomain& rhsDomain,
    const std::optional<std::vector<SInt>>& rhsExcluded, const SIntDomain& joinedDomain)
{
    if (!lhsExcluded.has_value() && !rhsExcluded.has_value()) {
        return std::nullopt;
    }
    std::vector<SInt> candidates;
    if (lhsExcluded.has_value()) {
        candidates.insert(candidates.end(), lhsExcluded->begin(), lhsExcluded->end());
    }
    if (rhsExcluded.has_value()) {
        candidates.insert(candidates.end(), rhsExcluded->begin(), rhsExcluded->end());
    }
    candidates = NormalizeExactIntValues(std::move(candidates));
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
        [&](const SInt& value) {
            return SIntRangeMayContain(lhsDomain, lhsExcluded, value) ||
                SIntRangeMayContain(rhsDomain, rhsExcluded, value);
        }), candidates.end());
    return NormalizeExcludedSIntValues(std::move(candidates), joinedDomain);
}

std::optional<std::unique_ptr<ValueRange>> JoinBoundedLoopObservationRanges(
    const ValueRange& current, const ValueRange& incoming)
{
    auto joined = current.Join(incoming);
    if (!joined.has_value() || current.GetRangeKind() != ValueRange::RangeKind::SINT ||
        incoming.GetRangeKind() != ValueRange::RangeKind::SINT) {
        return joined;
    }
    const auto& lhs = StaticCast<const SIntRange&>(current);
    const auto& rhs = StaticCast<const SIntRange&>(incoming);
    auto* joinedRange = StaticCast<const SIntRange*>(joined.value().get());
    const auto& joinedCongruence = joinedRange->GetCongruence();
    const auto& joinedKnownBits = joinedRange->GetKnownBits();
    if (joinedRange->GetIntervalFragments().has_value()) {
        return joined;
    }
    if ((joinedCongruence.has_value() && joinedCongruence->stride > 1) ||
        (joinedKnownBits.has_value() && joinedKnownBits->IsUseful())) {
        // Congruence and KnownBits already retain regular non-convex spacing.
        // Enumerating those gaps as exclusions adds no precision and makes
        // every bounded-loop observation allocate short-lived hole sets.
        return joined;
    }
    std::vector<SInt> excluded;
    if (joinedRange->GetExcludedValues().has_value()) {
        excluded = *joinedRange->GetExcludedValues();
    }
    AddSmallGapExclusions(lhs.GetVal(), rhs.GetVal(), excluded);
    auto excludedValues = NormalizeExcludedSIntValues(std::move(excluded), joinedRange->GetVal());
    if (excludedValues == joinedRange->GetExcludedValues()) {
        return joined;
    }
    std::unique_ptr<ValueRange> refined = std::make_unique<SIntRange>(joinedRange->GetVal(),
        joinedRange->GetExactValues(), joinedRange->GetCongruence(), joinedRange->GetKnownBits(),
        std::move(excludedValues));
    return std::optional<std::unique_ptr<ValueRange>>{std::move(refined)};
}

bool SIntFragmentContainsValue(
    const SIntIntervalFragment& fragment, const SInt& value, bool isUnsigned)
{
    const auto mathematical = [isUnsigned](const SInt& candidate) {
        return ToMathematicalInteger(candidate, isUnsigned);
    };
    const auto candidate = mathematical(value);
    return candidate >= mathematical(fragment.lower) &&
        candidate <= mathematical(fragment.upper) &&
        (fragment.stride <= 1 ||
            CanonicalResidue(candidate, fragment.stride) == fragment.residue);
}

bool SIntRangeContainsExactValue(const SIntRange& range, const SInt& value)
{
    if (value.Width() != range.GetVal().Width()) {
        return false;
    }
    if (range.GetExactValues().has_value()) {
        return std::find(range.GetExactValues()->begin(),
                   range.GetExactValues()->end(), value) !=
            range.GetExactValues()->end();
    }
    if (!SIntRangeMayContain(range.GetVal(), range.GetExcludedValues(), value)) {
        return false;
    }
    if (range.GetCongruence().has_value() &&
        CanonicalResidue(ToMathematicalInteger(value, range.GetVal().IsUnsigned()),
            range.GetCongruence()->stride) != range.GetCongruence()->residue) {
        return false;
    }
    if (range.GetKnownBits().has_value()) {
        const auto raw = value.UVal();
        if ((raw & range.GetKnownBits()->knownZero) != 0 ||
            (raw & range.GetKnownBits()->knownOne) !=
                range.GetKnownBits()->knownOne) {
            return false;
        }
    }
    if (range.GetIntervalFragments().has_value()) {
        return std::any_of(range.GetIntervalFragments()->begin(),
            range.GetIntervalFragments()->end(), [&](const auto& fragment) {
                return SIntFragmentContainsValue(
                    fragment, value, range.GetVal().IsUnsigned());
            });
    }
    return true;
}

std::optional<std::vector<SInt>> IntersectExcludedSIntValues(const SIntRange& lhs,
    const SIntRange& rhs, const SIntDomain& intersection)
{
    std::vector<SInt> excluded;
    if (lhs.GetExcludedValues().has_value()) {
        excluded.insert(excluded.end(), lhs.GetExcludedValues()->begin(), lhs.GetExcludedValues()->end());
    }
    if (rhs.GetExcludedValues().has_value()) {
        excluded.insert(excluded.end(), rhs.GetExcludedValues()->begin(), rhs.GetExcludedValues()->end());
    }
    return NormalizeExcludedSIntValues(std::move(excluded), intersection);
}

std::optional<std::vector<SInt>> FilterExactValuesByExclusions(
    std::optional<std::vector<SInt>> exactValues,
    const std::optional<std::vector<SInt>>& excludedValues)
{
    if (!exactValues.has_value() || !excludedValues.has_value()) {
        return exactValues;
    }
    exactValues->erase(std::remove_if(exactValues->begin(), exactValues->end(),
        [&](const SInt& value) { return IsExplicitlyExcluded(excludedValues, value); }), exactValues->end());
    return exactValues->empty() ? std::nullopt : NormalizeExactIntSet(std::move(*exactValues));
}

std::optional<std::vector<SInt>> IntersectExactIntSets(
    const SIntRange& lhs, const SIntRange& rhs, const SIntDomain& intersection)
{
    const auto& lhsValues = lhs.GetExactValues();
    const auto& rhsValues = rhs.GetExactValues();
    if (!lhsValues.has_value() && !rhsValues.has_value()) {
        return std::nullopt;
    }
    const auto& source = lhsValues.has_value() ? *lhsValues : *rhsValues;
    std::vector<SInt> values;
    values.reserve(source.size());
    for (const auto& value : source) {
        if (!ExactValueSatisfiesDomain(value, intersection)) {
            continue;
        }
        if (lhsValues.has_value() &&
            std::find(lhsValues->begin(), lhsValues->end(), value) == lhsValues->end()) {
            continue;
        }
        if (rhsValues.has_value() &&
            std::find(rhsValues->begin(), rhsValues->end(), value) == rhsValues->end()) {
            continue;
        }
        values.emplace_back(value);
    }
    return NormalizeExactIntSet(std::move(values));
}

bool IsUsefulNarrowingSeed(const SIntRange& range)
{
    const auto& domain = range.GetVal();
    if (range.GetExactValues().has_value() || range.GetCongruence().has_value() ||
        range.GetKnownBits().has_value() || range.GetExcludedValues().has_value() ||
        range.GetIntervalFragments().has_value() || domain.IsSingleValue()) {
        return true;
    }
    const auto& numeric = domain.NumericBound();
    if (numeric.IsFullSet() || numeric.IsEmptySet() || numeric.IsWrappedSet() || numeric.IsSignWrappedSet()) {
        return false;
    }
    const auto bitWidth = static_cast<unsigned>(domain.Width());
    const auto cardinality = domain.IsUnsigned()
        ? static_cast<__int128>(numeric.UMaxValue().UVal()) -
            static_cast<__int128>(numeric.UMinValue().UVal()) + 1
        : static_cast<__int128>(numeric.SMaxValue().SVal()) -
            static_cast<__int128>(numeric.SMinValue().SVal()) + 1;
    const auto usefulCardinalityLimit = static_cast<__int128>(1) << (bitWidth - 2U);
    return cardinality > 0 && cardinality <= usefulCardinalityLimit;
}

bool NarrowRangeValue(RangeValueDomain& current, const RangeValueDomain& candidate)
{
    using ValueKind = RangeValueDomain::ValueKind;
    if (candidate.GetKind() == ValueKind::TOP || candidate.GetKind() == ValueKind::BOTTOM ||
        candidate.GetKind() == ValueKind::REF || current.GetKind() == ValueKind::BOTTOM ||
        current.GetKind() == ValueKind::REF) {
        return false;
    }
    if (current.GetKind() == ValueKind::TOP) {
        auto candidateRange = candidate.CheckAbsVal();
        CJC_ASSERT(candidateRange != nullptr);
        if (candidateRange->GetRangeKind() == ValueRange::RangeKind::SINT) {
            const auto& sintRange = StaticCast<const SIntRange&>(*candidateRange);
            if (!IsUsefulNarrowingSeed(sintRange)) {
                return false;
            }
        }
        current = candidate;
        return true;
    }

    auto currentRange = current.CheckAbsVal();
    auto candidateRange = candidate.CheckAbsVal();
    CJC_ASSERT(currentRange != nullptr && candidateRange != nullptr);
    if (currentRange->GetRangeKind() != candidateRange->GetRangeKind()) {
        return false;
    }
    if (currentRange->GetRangeKind() == ValueRange::RangeKind::BOOL) {
        const auto& lhs = StaticCast<const BoolRange&>(*currentRange);
        const auto& rhs = StaticCast<const BoolRange&>(*candidateRange);
        auto intersection = lhs.GetVal() & rhs.GetVal();
        if (intersection.IsBottom() || lhs.GetVal().IsSame(intersection)) {
            return false;
        }
        current = std::make_unique<BoolRange>(std::move(intersection));
        return true;
    }

    const auto& lhs = StaticCast<const SIntRange&>(*currentRange);
    const auto& rhs = StaticCast<const SIntRange&>(*candidateRange);
    if (lhs.GetVal().IsTop() && !IsUsefulNarrowingSeed(rhs)) {
        return false;
    }
    auto intersection = SIntDomain::Intersects(lhs.GetVal(), rhs.GetVal());
    if (intersection.IsBottom()) {
        return false;
    }
    auto exactValues = IntersectExactIntSets(lhs, rhs, intersection);
    if ((lhs.GetExactValues().has_value() || rhs.GetExactValues().has_value()) && !exactValues.has_value()) {
        return false;
    }
    bool congruencesCompatible = true;
    auto congruence = IntersectSIntCongruences(
        lhs.GetCongruence(), rhs.GetCongruence(), congruencesCompatible);
    if (!congruencesCompatible) {
        return false;
    }
    bool knownBitsCompatible = true;
    auto knownBits = IntersectSIntKnownBits(
        lhs.GetKnownBits(), rhs.GetKnownBits(), intersection.Width(), knownBitsCompatible);
    if (!knownBitsCompatible) {
        return false;
    }
    auto excludedValues = IntersectExcludedSIntValues(lhs, rhs, intersection);
    exactValues = FilterExactValuesByExclusions(std::move(exactValues), excludedValues);
    auto intervalFragments =
        SelectSIntIntervalFragmentsForNarrowing(lhs, rhs, intersection);
    if (lhs.GetVal().IsSame(intersection) && lhs.GetExactValues() == exactValues &&
        lhs.GetCongruence() == congruence && lhs.GetKnownBits() == knownBits &&
        lhs.GetExcludedValues() == excludedValues &&
        lhs.GetIntervalFragments() == intervalFragments) {
        return false;
    }
    current = std::make_unique<SIntRange>(
        std::move(intersection), std::move(exactValues), std::move(congruence), std::move(knownBits),
        std::move(excludedValues), std::move(intervalFragments));
    return true;
}

SIntDomain DomainFromExactIntValues(const std::vector<SInt>& values, bool isUnsigned)
{
    CJC_ASSERT(!values.empty());
    SIntDomain domain{ConstantRange{values.front()}, isUnsigned};
    for (size_t i = 1; i < values.size(); ++i) {
        domain = SIntDomain::Unions(domain, SIntDomain{ConstantRange{values[i]}, isUnsigned});
    }
    return domain;
}

Type* GetRefRootBaseType(Type* type)
{
    if (type == nullptr || !type->IsRef()) {
        return nullptr;
    }
    return StaticCast<RefType*>(type)->GetRootBaseType();
}

bool IsSupportedContextRootType(Type* type)
{
    return type != nullptr && (type->IsBoolean() || type->IsInteger() || type->IsClass() ||
        type->IsStruct() || type->IsTuple() || type->IsRawArray());
}

bool IsSupportedLambdaArgumentType(Type* type)
{
    if (type == nullptr) {
        return false;
    }
    if (type->IsBoolean() || type->IsInteger()) {
        return true;
    }
    auto baseType = GetRefRootBaseType(type);
    return IsSupportedContextRootType(baseType);
}

bool IsSupportedLambdaResultType(Type* type)
{
    if (type == nullptr) {
        return false;
    }
    if (type->IsBoolean() || type->IsInteger()) {
        return true;
    }
    auto baseType = GetRefRootBaseType(type);
    return baseType != nullptr && (baseType->IsBoolean() || baseType->IsInteger());
}

bool CanAnalyzeLambdaContext(const Lambda* lambda)
{
    if (lambda == nullptr || lambda->GetBody() == nullptr || lambda->GetReturnValue() == nullptr ||
        !IsSupportedLambdaResultType(lambda->GetReturnValue()->GetType())) {
        return false;
    }
    const auto params = lambda->GetParams();
    const auto captures = lambda->GetCapturedVariables();
    return std::all_of(params.begin(), params.end(), [](Value* param) {
        return param != nullptr && IsSupportedLambdaArgumentType(param->GetType());
    }) && std::all_of(captures.begin(), captures.end(), [](Value* captured) {
        return captured != nullptr && IsSupportedLambdaArgumentType(captured->GetType());
    });
}

Type* GetTrackedMutableGlobalBaseType(Value* location);
AbstractObject* EnsureMutableGlobalValueInitialized(RangeDomain& state, GlobalVar* global);

struct TrackedMutableGlobals {
    std::vector<GlobalVar*> values;
    bool complete{true};

    auto begin() const
    {
        return values.begin();
    }

    auto end() const
    {
        return values.end();
    }

    size_t size() const
    {
        return values.size();
    }

    GlobalVar* operator[](size_t index) const
    {
        return values[index];
    }
};

TrackedMutableGlobals CollectTrackedMutableGlobals(const Function* function, const Package* package = nullptr);
TrackedMutableGlobals CollectTrackedMutableGlobals(const Lambda* lambda, const Package* package = nullptr);
void ClearTrackedMutableGlobalCache();
const Expression* GetDefiningExpr(Value* value);
std::optional<SInt> GetSingleIntFromDefiningConstant(Value* value);
RelationalOperation ToRelationalOperation(ExprKind kind);
RelationalOperation NegateRelation(RelationalOperation rel);
}

ClassType* ResolveExactAllocatedClass(Value* value, std::unordered_set<Value*>& visited, unsigned depth = 0);
Function* ResolveExactInvokeTarget(const Invoke* invoke, ClassType* exactClass, CHIRBuilder& builder);
Function* ResolveExactInvokeTarget(
    const InvokeWithException* invoke, ClassType* exactClass, CHIRBuilder& builder);

struct RangeAnalysis::ContextAbstractValue {
    enum class Kind : uint8_t { TOP, BOOL, SINT, CLASS, OBJECT, LAMBDA };

    Kind kind{Kind::TOP};
    std::optional<BoolDomain> boolValue;
    std::unique_ptr<SIntDomain> sintValue;
    std::optional<std::vector<SInt>> exactSIntValues;
    std::optional<SIntCongruence> sintCongruence;
    std::optional<SIntKnownBits> sintKnownBits;
    std::optional<std::vector<SInt>> excludedSIntValues;
    std::optional<std::vector<SIntIntervalFragment>> sintIntervalFragments;
    ClassType* classValue{nullptr};
    const Lambda* lambdaValue{nullptr};
    std::vector<ContextAbstractValue> lambdaCapturedValues;
    std::vector<ContextAbstractValue> objectFields;
    size_t aliasGroup{0};
    size_t objectIdentity{0};

    ContextAbstractValue() = default;

    explicit ContextAbstractValue(BoolDomain value) : kind(Kind::BOOL), boolValue(std::move(value))
    {
    }

    explicit ContextAbstractValue(const SIntDomain& value, std::optional<std::vector<SInt>> exactValues = std::nullopt,
        std::optional<SIntCongruence> congruence = std::nullopt,
        std::optional<SIntKnownBits> knownBits = std::nullopt,
        std::optional<std::vector<SInt>> excludedValues = std::nullopt,
        std::optional<std::vector<SIntIntervalFragment>> intervalFragments = std::nullopt)
        : kind(Kind::SINT), sintValue(std::make_unique<SIntDomain>(value)),
          exactSIntValues(exactValues.has_value() ? NormalizeExactIntSet(std::move(*exactValues)) : std::nullopt),
          sintCongruence(NormalizeCongruence(std::move(congruence))),
          sintKnownBits(NormalizeKnownBits(std::move(knownBits), value.Width())),
          excludedSIntValues(excludedValues.has_value()
                  ? NormalizeExcludedSIntValues(std::move(*excludedValues), value)
                  : std::nullopt),
          sintIntervalFragments(intervalFragments.has_value()
                  ? NormalizeSIntIntervalFragments(std::move(*intervalFragments), value)
                  : std::nullopt)
    {
        if (!sintCongruence.has_value()) {
            sintCongruence = InferCongruenceFromExactValues(exactSIntValues, value.IsUnsigned());
        }
        if (!sintCongruence.has_value()) {
            sintCongruence = InferCongruenceFromKnownBits(sintKnownBits, value.Width());
        }
    }

    explicit ContextAbstractValue(ClassType* value) : kind(Kind::CLASS), classValue(value)
    {
    }

    explicit ContextAbstractValue(const Lambda* value) : kind(Kind::LAMBDA), lambdaValue(value)
    {
    }

    ContextAbstractValue(const Lambda* value, std::vector<ContextAbstractValue> captures)
        : kind(Kind::LAMBDA), lambdaValue(value), lambdaCapturedValues(std::move(captures))
    {
    }

    ContextAbstractValue(ClassType* value, std::vector<ContextAbstractValue> fields, size_t identity)
        : kind(Kind::OBJECT), classValue(value), objectFields(std::move(fields)), objectIdentity(identity)
    {
    }

    ContextAbstractValue(const ContextAbstractValue& other)
        : kind(other.kind), boolValue(other.boolValue), exactSIntValues(other.exactSIntValues),
          sintCongruence(other.sintCongruence), sintKnownBits(other.sintKnownBits),
          excludedSIntValues(other.excludedSIntValues),
          sintIntervalFragments(other.sintIntervalFragments),
          classValue(other.classValue), lambdaValue(other.lambdaValue),
          lambdaCapturedValues(other.lambdaCapturedValues), objectFields(other.objectFields),
          aliasGroup(other.aliasGroup), objectIdentity(other.objectIdentity)
    {
        if (other.sintValue) {
            sintValue = std::make_unique<SIntDomain>(*other.sintValue);
        }
    }

    ContextAbstractValue(ContextAbstractValue&& other) noexcept = default;

    ContextAbstractValue& operator=(const ContextAbstractValue& other)
    {
        if (this == &other) {
            return *this;
        }
        kind = other.kind;
        boolValue = other.boolValue;
        sintValue = other.sintValue ? std::make_unique<SIntDomain>(*other.sintValue) : nullptr;
        exactSIntValues = other.exactSIntValues;
        sintCongruence = other.sintCongruence;
        sintKnownBits = other.sintKnownBits;
        excludedSIntValues = other.excludedSIntValues;
        sintIntervalFragments = other.sintIntervalFragments;
        classValue = other.classValue;
        lambdaValue = other.lambdaValue;
        lambdaCapturedValues = other.lambdaCapturedValues;
        objectFields = other.objectFields;
        aliasGroup = other.aliasGroup;
        objectIdentity = other.objectIdentity;
        return *this;
    }

    ContextAbstractValue& operator=(ContextAbstractValue&& other) noexcept = default;

    bool IsTop() const
    {
        return kind == Kind::TOP;
    }

    bool IsSingleValue() const
    {
        if (kind == Kind::BOOL && boolValue.has_value()) {
            return boolValue->IsSingleValue();
        }
        if (kind == Kind::SINT && sintValue != nullptr) {
            return sintValue->IsSingleValue();
        }
        if (kind == Kind::CLASS) {
            return classValue != nullptr;
        }
        if (kind == Kind::LAMBDA) {
            return lambdaValue != nullptr &&
                std::all_of(lambdaCapturedValues.begin(), lambdaCapturedValues.end(),
                    [](const auto& captured) { return captured.IsSingleValue(); });
        }
        return kind == Kind::OBJECT &&
            std::all_of(objectFields.begin(), objectFields.end(), [](const auto& field) {
                return field.IsSingleValue();
            });
    }

    std::string ToKeyString(Type* type,
        std::unordered_map<size_t, size_t>& canonicalObjectIdentities,
        size_t& nextCanonicalObjectIdentity) const
    {
        std::stringstream ss;
        if (aliasGroup != 0) {
            ss << "alias:" << aliasGroup << ':';
        }
        if (IsTop()) {
            ss << "top:" << (type == nullptr ? "<null>" : type->ToString());
            return ss.str();
        }
        if (kind == Kind::BOOL && boolValue.has_value()) {
            ss << "bool:" << *boolValue;
            return ss.str();
        }
        if (kind == Kind::SINT && sintValue) {
            ss << "sint:" << *sintValue;
            if (exactSIntValues.has_value()) {
                ss << ":exact{";
                for (size_t i = 0; i < exactSIntValues->size(); ++i) {
                    if (i != 0) {
                        ss << ",";
                    }
                    ss << (*exactSIntValues)[i].UVal();
                }
                ss << "}";
            }
            if (sintCongruence.has_value()) {
                ss << ":congruence{" << sintCongruence->residue << "mod"
                   << sintCongruence->stride << "}";
            }
            if (sintKnownBits.has_value()) {
                ss << ":knownbits{" << sintKnownBits->knownZero << ","
                   << sintKnownBits->knownOne << "}";
            }
            if (excludedSIntValues.has_value()) {
                ss << ":exclude{";
                for (size_t i = 0; i < excludedSIntValues->size(); ++i) {
                    if (i != 0) {
                        ss << ",";
                    }
                    ss << (*excludedSIntValues)[i].UVal();
                }
                ss << "}";
            }
            if (sintIntervalFragments.has_value()) {
                ss << ":fragments{";
                for (size_t i = 0; i < sintIntervalFragments->size(); ++i) {
                    if (i != 0) {
                        ss << ",";
                    }
                    ss << "[" << (*sintIntervalFragments)[i].lower.UVal() << ","
                       << (*sintIntervalFragments)[i].upper.UVal() << "]";
                }
                ss << "}";
            }
            return ss.str();
        }
        if (kind == Kind::CLASS && classValue != nullptr) {
            ss << "class:" << static_cast<const void*>(classValue);
            return ss.str();
        }
        if (kind == Kind::LAMBDA && lambdaValue != nullptr) {
            ss << "lambda:" << static_cast<const void*>(lambdaValue) << '{';
            for (size_t i = 0; i < lambdaCapturedValues.size(); ++i) {
                if (i != 0) {
                    ss << ',';
                }
                ss << lambdaCapturedValues[i].ToKeyString(
                    nullptr, canonicalObjectIdentities, nextCanonicalObjectIdentity);
            }
            ss << '}';
            return ss.str();
        }
        if (kind == Kind::OBJECT) {
            size_t canonicalIdentity = 0;
            if (objectIdentity != 0) {
                auto [found, inserted] =
                    canonicalObjectIdentities.emplace(objectIdentity, nextCanonicalObjectIdentity);
                if (inserted) {
                    ++nextCanonicalObjectIdentity;
                }
                canonicalIdentity = found->second;
            }
            ss << "object:" << static_cast<const void*>(classValue) << ":id:" << canonicalIdentity << '{';
            for (size_t i = 0; i < objectFields.size(); ++i) {
                if (i != 0) {
                    ss << ',';
                }
                ss << objectFields[i].ToKeyString(
                    nullptr, canonicalObjectIdentities, nextCanonicalObjectIdentity);
            }
            ss << '}';
            return ss.str();
        }
        ss << "top:" << (type == nullptr ? "<null>" : type->ToString());
        return ss.str();
    }

    void CollectRawObjectIdentities(std::unordered_map<size_t, size_t>& identities) const
    {
        if (kind == Kind::OBJECT && objectIdentity != 0) {
            identities.emplace(objectIdentity, objectIdentity);
        }
        for (const auto& captured : lambdaCapturedValues) {
            captured.CollectRawObjectIdentities(identities);
        }
        for (const auto& field : objectFields) {
            field.CollectRawObjectIdentities(identities);
        }
    }

    std::string ToKeyString(Type* type) const
    {
        std::unordered_map<size_t, size_t> canonicalObjectIdentities;
        CollectRawObjectIdentities(canonicalObjectIdentities);
        size_t nextCanonicalObjectIdentity = 1;
        return ToKeyString(type, canonicalObjectIdentities, nextCanonicalObjectIdentity);
    }

    bool BindObjectIdentities(
        const ContextAbstractValue& current, std::unordered_map<size_t, size_t>& identities) const
    {
        if (kind != current.kind || aliasGroup != current.aliasGroup) {
            return false;
        }
        if (kind == Kind::OBJECT) {
            if (classValue != current.classValue || objectFields.size() != current.objectFields.size() ||
                (objectIdentity == 0) != (current.objectIdentity == 0)) {
                return false;
            }
            if (objectIdentity != 0) {
                auto [found, inserted] = identities.emplace(objectIdentity, current.objectIdentity);
                if (!inserted && found->second != current.objectIdentity) {
                    return false;
                }
            }
        }
        if (kind == Kind::LAMBDA &&
            (lambdaValue != current.lambdaValue ||
                lambdaCapturedValues.size() != current.lambdaCapturedValues.size())) {
            return false;
        }
        for (size_t i = 0; i < lambdaCapturedValues.size(); ++i) {
            if (!lambdaCapturedValues[i].BindObjectIdentities(
                    current.lambdaCapturedValues[i], identities)) {
                return false;
            }
        }
        for (size_t i = 0; i < objectFields.size(); ++i) {
            if (!objectFields[i].BindObjectIdentities(current.objectFields[i], identities)) {
                return false;
            }
        }
        return true;
    }

    void RebindObjectIdentities(std::unordered_map<size_t, size_t>& identities)
    {
        if (kind == Kind::OBJECT && objectIdentity != 0) {
            auto [found, inserted] =
                identities.emplace(objectIdentity, AllocateSyntheticContextObjectIdentity());
            (void)inserted;
            objectIdentity = found->second;
        }
        for (auto& captured : lambdaCapturedValues) {
            captured.RebindObjectIdentities(identities);
        }
        for (auto& field : objectFields) {
            field.RebindObjectIdentities(identities);
        }
    }

};

namespace {
struct AffineRecursiveSummaryDescriptor {
    Type* type{nullptr};
    Parameter* parameter{nullptr};
    RelationalOperation baseRelation{RelationalOperation::EQ};
    __int128 bound{0};
    __int128 parameterStep{0};
    __int128 baseResult{0};
    __int128 resultStep{0};
};

std::optional<__int128> GetMathematicalConstant(Value* value, bool isUnsigned)
{
    auto constant = GetSingleIntFromDefiningConstant(value);
    if (!constant.has_value()) {
        return std::nullopt;
    }
    return ToMathematicalInteger(constant.value(), isUnsigned);
}

bool IsRelationalKindForRecursiveSummary(ExprKind kind)
{
    return kind == ExprKind::LT || kind == ExprKind::LE || kind == ExprKind::GT ||
        kind == ExprKind::GE || kind == ExprKind::EQUAL || kind == ExprKind::NOTEQUAL;
}

std::optional<__int128> GetAffineConstantDelta(Value* value, Value* base)
{
    if (value == base) {
        return __int128{0};
    }
    auto expression = GetDefiningExpr(value);
    if (expression == nullptr ||
        (expression->GetExprKind() != ExprKind::ADD && expression->GetExprKind() != ExprKind::SUB)) {
        return std::nullopt;
    }
    auto binary = StaticCast<const BinaryExpression*>(expression);
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    const bool isUnsigned = base->GetType()->IsUnsignedInteger();
    if (lhs == base) {
        auto constant = GetMathematicalConstant(rhs, isUnsigned);
        if (!constant.has_value()) {
            return std::nullopt;
        }
        return expression->GetExprKind() == ExprKind::ADD
            ? constant.value()
            : -constant.value();
    }
    if (expression->GetExprKind() == ExprKind::ADD && rhs == base) {
        return GetMathematicalConstant(lhs, isUnsigned);
    }
    return std::nullopt;
}

std::optional<AffineRecursiveSummaryDescriptor> ExtractAffineRecursiveSummaryDescriptor(const Function* callee)
{
    if (callee == nullptr || callee->GetBody() == nullptr || !callee->HasReturnValue()) {
        return std::nullopt;
    }
    auto params = callee->GetParams();
    auto returnValue = callee->GetReturnValue();
    auto rawReturnType = returnValue == nullptr ? nullptr : returnValue->GetType();
    auto returnType = rawReturnType != nullptr && rawReturnType->IsRef()
        ? GetRefRootBaseType(rawReturnType)
        : rawReturnType;
    if (params.size() != 1 || params.front() == nullptr || returnValue == nullptr ||
        params.front()->GetType() == nullptr || returnType == nullptr ||
        !params.front()->GetType()->IsInteger() || !returnType->IsInteger() ||
        ToWidth(*params.front()->GetType()) != ToWidth(*returnType) ||
        params.front()->GetType()->IsUnsignedInteger() != returnType->IsUnsignedInteger()) {
        return std::nullopt;
    }
    const auto& blocks = callee->GetBody()->GetBlocks();
    auto entry = callee->GetEntryBlock();
    if (blocks.size() != 3 || entry == nullptr || entry->GetTerminator() == nullptr ||
        entry->GetTerminator()->GetExprKind() != ExprKind::BRANCH) {
        return std::nullopt;
    }
    auto branch = StaticCast<const Branch*>(entry->GetTerminator());
    auto conditionExpression = GetDefiningExpr(branch->GetCondition());
    if (conditionExpression == nullptr || !IsRelationalKindForRecursiveSummary(conditionExpression->GetExprKind())) {
        return std::nullopt;
    }
    auto condition = StaticCast<const BinaryExpression*>(conditionExpression);
    auto parameter = params.front();
    auto lhs = condition->GetLHSOperand();
    auto rhs = condition->GetRHSOperand();
    const bool isUnsigned = parameter->GetType()->IsUnsignedInteger();
    RelationalOperation trueRelation;
    std::optional<__int128> bound;
    if (lhs == parameter) {
        trueRelation = ToRelationalOperation(conditionExpression->GetExprKind());
        bound = GetMathematicalConstant(rhs, isUnsigned);
    } else if (rhs == parameter) {
        auto relation = ToRelationalOperation(conditionExpression->GetExprKind());
        switch (relation) {
            case RelationalOperation::LT:
                trueRelation = RelationalOperation::GT;
                break;
            case RelationalOperation::LE:
                trueRelation = RelationalOperation::GE;
                break;
            case RelationalOperation::GT:
                trueRelation = RelationalOperation::LT;
                break;
            case RelationalOperation::GE:
                trueRelation = RelationalOperation::LE;
                break;
            case RelationalOperation::EQ:
            case RelationalOperation::NE:
                trueRelation = relation;
                break;
        }
        bound = GetMathematicalConstant(lhs, isUnsigned);
    } else {
        return std::nullopt;
    }
    if (!bound.has_value()) {
        return std::nullopt;
    }

    const Apply* recursiveApply = nullptr;
    std::vector<const Store*> returnStores;
    std::vector<const BinaryExpression*> binaryExpressions;
    for (auto block : blocks) {
        auto terminator = block->GetTerminator();
        if (block == entry) {
            if (terminator != branch) {
                return std::nullopt;
            }
        } else if (terminator == nullptr || terminator->GetExprKind() != ExprKind::EXIT) {
            return std::nullopt;
        }
        for (auto expression : block->GetNonTerminatorExpressions()) {
            switch (expression->GetExprKind()) {
                case ExprKind::CONSTANT:
                case ExprKind::DEBUGEXPR:
                    break;
                case ExprKind::ALLOCATE:
                    if (expression->GetResult() != returnValue) {
                        return std::nullopt;
                    }
                    break;
                case ExprKind::ADD:
                case ExprKind::SUB:
                case ExprKind::LT:
                case ExprKind::LE:
                case ExprKind::GT:
                case ExprKind::GE:
                case ExprKind::EQUAL:
                case ExprKind::NOTEQUAL:
                    binaryExpressions.emplace_back(StaticCast<const BinaryExpression*>(expression));
                    break;
                case ExprKind::APPLY: {
                    auto apply = StaticCast<const Apply*>(expression);
                    if (recursiveApply != nullptr || DynamicCast<Function*>(apply->GetCallee()) != callee) {
                        return std::nullopt;
                    }
                    recursiveApply = apply;
                    break;
                }
                case ExprKind::STORE: {
                    auto store = StaticCast<const Store*>(expression);
                    if (store->GetLocation() != returnValue) {
                        return std::nullopt;
                    }
                    returnStores.emplace_back(store);
                    break;
                }
                default:
                    return std::nullopt;
            }
        }
    }
    if (recursiveApply == nullptr || recursiveApply->GetArgs().size() != 1 || returnStores.size() != 2) {
        return std::nullopt;
    }

    const Store* baseStore = nullptr;
    const Store* recursiveStore = nullptr;
    std::optional<__int128> baseResult;
    for (auto store : returnStores) {
        auto constant = GetMathematicalConstant(store->GetValue(), isUnsigned);
        if (constant.has_value()) {
            if (baseStore != nullptr) {
                return std::nullopt;
            }
            baseStore = store;
            baseResult = constant;
        } else {
            if (recursiveStore != nullptr) {
                return std::nullopt;
            }
            recursiveStore = store;
        }
    }
    if (baseStore == nullptr || recursiveStore == nullptr || !baseResult.has_value()) {
        return std::nullopt;
    }
    auto baseBlock = baseStore->GetParentBlock();
    auto recursiveBlock = recursiveStore->GetParentBlock();
    if (baseBlock == nullptr || recursiveBlock == nullptr || baseBlock == recursiveBlock ||
        recursiveApply->GetParentBlock() != recursiveBlock ||
        !((branch->GetTrueBlock() == baseBlock && branch->GetFalseBlock() == recursiveBlock) ||
            (branch->GetFalseBlock() == baseBlock && branch->GetTrueBlock() == recursiveBlock))) {
        return std::nullopt;
    }
    auto parameterStep = GetAffineConstantDelta(recursiveApply->GetArgs().front(), parameter);
    auto resultStep = GetAffineConstantDelta(recursiveStore->GetValue(), recursiveApply->GetResult());
    if (!parameterStep.has_value() || !resultStep.has_value() || parameterStep.value() == 0) {
        return std::nullopt;
    }
    std::unordered_set<const BinaryExpression*> requiredBinaries{condition};
    if (recursiveApply->GetArgs().front() != parameter) {
        requiredBinaries.emplace(StaticCast<const BinaryExpression*>(
            GetDefiningExpr(recursiveApply->GetArgs().front())));
    }
    if (recursiveStore->GetValue() != recursiveApply->GetResult()) {
        requiredBinaries.emplace(
            StaticCast<const BinaryExpression*>(GetDefiningExpr(recursiveStore->GetValue())));
    }
    if (binaryExpressions.size() != requiredBinaries.size() ||
        std::any_of(binaryExpressions.begin(), binaryExpressions.end(),
            [&requiredBinaries](const BinaryExpression* binary) {
                return requiredBinaries.find(binary) == requiredBinaries.end();
            })) {
        return std::nullopt;
    }

    auto baseRelation = branch->GetTrueBlock() == baseBlock
        ? trueRelation
        : NegateRelation(trueRelation);
    const bool descendsToLowerBase =
        baseRelation == RelationalOperation::LT || baseRelation == RelationalOperation::LE;
    const bool ascendsToUpperBase =
        baseRelation == RelationalOperation::GT || baseRelation == RelationalOperation::GE;
    if ((!descendsToLowerBase && !ascendsToUpperBase) ||
        (descendsToLowerBase && parameterStep.value() >= 0) ||
        (ascendsToUpperBase && parameterStep.value() <= 0)) {
        return std::nullopt;
    }
    return AffineRecursiveSummaryDescriptor{parameter->GetType(), parameter, baseRelation,
        bound.value(), parameterStep.value(), baseResult.value(), resultStep.value()};
}

bool RelationHolds(__int128 value, RelationalOperation relation, __int128 bound)
{
    switch (relation) {
        case RelationalOperation::LT:
            return value < bound;
        case RelationalOperation::LE:
            return value <= bound;
        case RelationalOperation::GT:
            return value > bound;
        case RelationalOperation::GE:
            return value >= bound;
        case RelationalOperation::EQ:
            return value == bound;
        case RelationalOperation::NE:
            return value != bound;
    }
}

std::optional<uint64_t> GetAffineRecursionIterationCount(
    __int128 input, const AffineRecursiveSummaryDescriptor& descriptor)
{
    if (RelationHolds(input, descriptor.baseRelation, descriptor.bound)) {
        return uint64_t{0};
    }
    unsigned __int128 distance = 0;
    unsigned __int128 step = 0;
    bool strict = false;
    switch (descriptor.baseRelation) {
        case RelationalOperation::LE:
            distance = static_cast<unsigned __int128>(input - descriptor.bound);
            step = static_cast<unsigned __int128>(-descriptor.parameterStep);
            break;
        case RelationalOperation::LT:
            distance = static_cast<unsigned __int128>(input - descriptor.bound);
            step = static_cast<unsigned __int128>(-descriptor.parameterStep);
            strict = true;
            break;
        case RelationalOperation::GE:
            distance = static_cast<unsigned __int128>(descriptor.bound - input);
            step = static_cast<unsigned __int128>(descriptor.parameterStep);
            break;
        case RelationalOperation::GT:
            distance = static_cast<unsigned __int128>(descriptor.bound - input);
            step = static_cast<unsigned __int128>(descriptor.parameterStep);
            strict = true;
            break;
        case RelationalOperation::EQ:
        case RelationalOperation::NE:
            return std::nullopt;
    }
    auto count = strict ? distance / step + 1 : (distance + step - 1) / step;
    if (count > std::numeric_limits<uint64_t>::max()) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(count);
}

bool IsWithin(__int128 value, __int128 lower, __int128 upper)
{
    return value >= lower && value <= upper;
}

std::optional<__int128> EvaluateAffineRecursiveResult(__int128 input,
    const AffineRecursiveSummaryDescriptor& descriptor, __int128 resultLower, __int128 resultUpper)
{
    auto iterations = GetAffineRecursionIterationCount(input, descriptor);
    if (!iterations.has_value()) {
        return std::nullopt;
    }
    auto count = static_cast<__int128>(iterations.value());
    if (descriptor.resultStep > 0 &&
        count > (resultUpper - descriptor.baseResult) / descriptor.resultStep) {
        return std::nullopt;
    }
    if (descriptor.resultStep < 0 &&
        count > (descriptor.baseResult - resultLower) / -descriptor.resultStep) {
        return std::nullopt;
    }
    auto result = descriptor.baseResult + count * descriptor.resultStep;
    return IsWithin(result, resultLower, resultUpper)
        ? std::optional<__int128>{result}
        : std::nullopt;
}

SInt MakeSIntFromMathematicalInteger(IntWidth width, __int128 value)
{
    return SInt{width, static_cast<uint64_t>(value)};
}
}

std::optional<RangeAnalysis::ContextAbstractValue> RangeAnalysis::TryAnalyzeAffineRecursiveCallee(
    const Function* callee, const ContextArguments& arguments,
    const ContextGlobalValues& globalValues) const
{
    if (callee == nullptr || arguments.size() != 1 || !globalValues.empty() ||
        arguments.front().kind != ContextAbstractValue::Kind::SINT ||
        arguments.front().sintValue == nullptr) {
        return std::nullopt;
    }
    static std::mutex descriptorMutex;
    static std::unordered_map<const Function*, std::optional<AffineRecursiveSummaryDescriptor>> descriptorCache;
    std::optional<AffineRecursiveSummaryDescriptor> descriptor;
    {
        std::lock_guard<std::mutex> lock(descriptorMutex);
        auto found = descriptorCache.find(callee);
        if (found == descriptorCache.end()) {
            found = descriptorCache.emplace(callee, ExtractAffineRecursiveSummaryDescriptor(callee)).first;
        }
        descriptor = found->second;
    }
    if (!descriptor.has_value()) {
        return std::nullopt;
    }
    const auto& input = *arguments.front().sintValue;
    if (input.IsBottom() || input.Width() != ToWidth(*descriptor->type) ||
        input.IsUnsigned() != descriptor->type->IsUnsignedInteger()) {
        return std::nullopt;
    }
    const auto width = input.Width();
    const bool isUnsigned = input.IsUnsigned();
    const __int128 typeLower = isUnsigned
        ? __int128{0}
        : static_cast<__int128>(SInt::SMinValue(width).SVal());
    const __int128 typeUpper = isUnsigned
        ? static_cast<__int128>(SInt::UMaxValue(width).UVal())
        : static_cast<__int128>(SInt::SMaxValue(width).SVal());
    if (!IsWithin(descriptor->bound, typeLower, typeUpper) ||
        !IsWithin(descriptor->baseResult, typeLower, typeUpper)) {
        return std::nullopt;
    }
    __int128 terminalLower = descriptor->bound;
    __int128 terminalUpper = descriptor->bound;
    switch (descriptor->baseRelation) {
        case RelationalOperation::LE:
            terminalLower = descriptor->bound + descriptor->parameterStep + 1;
            break;
        case RelationalOperation::LT:
            terminalLower = descriptor->bound + descriptor->parameterStep;
            terminalUpper = descriptor->bound - 1;
            break;
        case RelationalOperation::GE:
            terminalUpper = descriptor->bound + descriptor->parameterStep - 1;
            break;
        case RelationalOperation::GT:
            terminalLower = descriptor->bound + 1;
            terminalUpper = descriptor->bound + descriptor->parameterStep;
            break;
        case RelationalOperation::EQ:
        case RelationalOperation::NE:
            return std::nullopt;
    }
    if (!IsWithin(terminalLower, typeLower, typeUpper) ||
        !IsWithin(terminalUpper, typeLower, typeUpper)) {
        return std::nullopt;
    }
    const auto& numeric = input.NumericBound();
    if (numeric.IsEmptySet() ||
        (isUnsigned ? numeric.IsWrappedSet() : numeric.IsSignWrappedSet())) {
        return std::nullopt;
    }
    const auto inputLower = ToMathematicalInteger(numeric.MinValue(isUnsigned), isUnsigned);
    const auto inputUpper = ToMathematicalInteger(numeric.MaxValue(isUnsigned), isUnsigned);
    auto lowerResult = EvaluateAffineRecursiveResult(
        inputLower, descriptor.value(), typeLower, typeUpper);
    auto upperResult = EvaluateAffineRecursiveResult(
        inputUpper, descriptor.value(), typeLower, typeUpper);
    if (!lowerResult.has_value() || !upperResult.has_value()) {
        return std::nullopt;
    }
    auto resultLower = std::min(lowerResult.value(), upperResult.value());
    auto resultUpper = std::max(lowerResult.value(), upperResult.value());

    std::optional<std::vector<SInt>> exactValues;
    if (arguments.front().exactSIntValues.has_value()) {
        std::vector<SInt> results;
        results.reserve(arguments.front().exactSIntValues->size());
        for (const auto& value : *arguments.front().exactSIntValues) {
            auto exactResult = EvaluateAffineRecursiveResult(
                ToMathematicalInteger(value, isUnsigned), descriptor.value(), typeLower, typeUpper);
            if (!exactResult.has_value()) {
                return std::nullopt;
            }
            results.emplace_back(MakeSIntFromMathematicalInteger(width, exactResult.value()));
        }
        exactValues = NormalizeExactIntSet(std::move(results));
    }
    auto lower = MakeSIntFromMathematicalInteger(width, resultLower);
    auto upper = MakeSIntFromMathematicalInteger(width, resultUpper);
    auto domain = SIntDomain::Intersects(
        SIntDomain::FromNumeric(RelationalOperation::GE, lower, isUnsigned),
        SIntDomain::FromNumeric(RelationalOperation::LE, upper, isUnsigned));
    std::optional<SIntCongruence> congruence;
    auto absoluteResultStep = descriptor->resultStep < 0
        ? static_cast<unsigned __int128>(-descriptor->resultStep)
        : static_cast<unsigned __int128>(descriptor->resultStep);
    if (absoluteResultStep > 1 && absoluteResultStep <= std::numeric_limits<uint64_t>::max()) {
        auto stride = static_cast<uint64_t>(absoluteResultStep);
        congruence = SIntCongruence{stride, CanonicalResidue(descriptor->baseResult, stride)};
    }
    return ContextAbstractValue{domain, std::move(exactValues), std::move(congruence)};
}

std::optional<std::vector<GlobalVar*>> RangeAnalysis::CollectContextMutableGlobals(
    const Function* callee, const ContextArguments& arguments) const
{
    ScopedContextAnalysisStage stage("context-effect-closure");
    if (IsRangeAnalysisPerfCollectionEnabled()) {
        ++rangeAnalysisPerfStats.contextEffectClosureCalls;
    }
    ScopedRangeAnalysisPerfTimer timer(&rangeAnalysisPerfStats.contextEffectClosureNanos);
    std::stringstream cacheKeyStream;
    std::unordered_map<size_t, size_t> cacheObjectIdentities;
    size_t nextCacheObjectIdentity = 1;
    cacheKeyStream << static_cast<const void*>(callee);
    const auto& calleeParams = callee == nullptr
        ? std::vector<Parameter*>{}
        : callee->GetParams();
    for (size_t i = 0; i < arguments.size(); ++i) {
        auto parameterType = i < calleeParams.size() ? calleeParams[i]->GetType() : nullptr;
        cacheKeyStream << ':' << arguments[i].ToKeyString(
            parameterType, cacheObjectIdentities, nextCacheObjectIdentity);
    }
    auto cacheKey = cacheKeyStream.str();
    {
        std::lock_guard<std::mutex> lock(contextEffectClosureCacheMtx);
        auto found = contextEffectClosureCache.find(cacheKey);
        if (found != contextEffectClosureCache.end()) {
            if (IsRangeAnalysisPerfCollectionEnabled()) {
                ++rangeAnalysisPerfStats.contextEffectClosureCacheHits;
            }
            return found->second;
        }
    }
    const auto computeClosure = [&]() -> std::optional<std::vector<GlobalVar*>> {
    constexpr size_t MAX_EFFECT_CONTEXTS = 128;
    constexpr size_t MAX_EFFECT_DISPATCH_TARGETS = 16;
    constexpr size_t MAX_EFFECT_RESOLUTION_NODES = 512;
    constexpr unsigned MAX_EFFECT_RESOLUTION_DEPTH = 16;

    using ClassSet = std::optional<std::vector<ClassType*>>;
    struct EffectContext {
        const Function* function{nullptr};
        ContextArguments parameterValues;
        std::vector<std::vector<ClassType*>> parameterClasses;
    };

    if (callee == nullptr || callee->GetBody() == nullptr ||
        callee->GetParams().size() != arguments.size()) {
        return std::nullopt;
    }

    const auto normalizeClasses = [](std::vector<ClassType*> classes) {
        classes.erase(std::remove(classes.begin(), classes.end(), nullptr), classes.end());
        std::sort(classes.begin(), classes.end());
        classes.erase(std::unique(classes.begin(), classes.end()), classes.end());
        return classes;
    };

    EffectContext root;
    root.function = callee;
    root.parameterValues = arguments;
    root.parameterClasses.resize(arguments.size());
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i].classValue != nullptr) {
            root.parameterClasses[i].emplace_back(arguments[i].classValue);
        }
    }

    std::vector<EffectContext> worklist;
    worklist.emplace_back(std::move(root));
    std::unordered_set<std::string> visitedContexts;
    std::unordered_set<GlobalVar*> seenGlobals;
    std::vector<GlobalVar*> globals;
    size_t resolvedNodes = 0;

    const auto buildEffectContextKey = [](const EffectContext& context) {
        std::stringstream key;
        std::unordered_map<size_t, size_t> canonicalObjectIdentities;
        size_t nextCanonicalObjectIdentity = 1;
        key << static_cast<const void*>(context.function);
        const auto& params = context.function == nullptr
            ? std::vector<Parameter*>{}
            : context.function->GetParams();
        for (size_t i = 0; i < context.parameterValues.size(); ++i) {
            key << ':';
            auto parameterType = i < params.size() ? params[i]->GetType() : nullptr;
            key << context.parameterValues[i].ToKeyString(
                parameterType, canonicalObjectIdentities, nextCanonicalObjectIdentity);
        }
        return key.str();
    };

    const auto addGlobal = [&](GlobalVar* global) {
        if (global == nullptr || GetTrackedMutableGlobalBaseType(global) == nullptr ||
            !seenGlobals.emplace(global).second) {
            return true;
        }
        if (globals.size() >= MAX_CONTEXT_GLOBALS) {
            return false;
        }
        globals.emplace_back(global);
        return true;
    };

    const auto resolveEffectContextValue =
        [](const EffectContext& context, Value* value) -> const ContextAbstractValue* {
        constexpr unsigned MAX_CONTEXT_VALUE_DEPTH = 16;
        std::function<const ContextAbstractValue*(Value*, unsigned)> resolve =
            [&](Value* current, unsigned depth) -> const ContextAbstractValue* {
            if (current == nullptr || context.function == nullptr ||
                depth >= MAX_CONTEXT_VALUE_DEPTH) {
                return nullptr;
            }
            if (current->IsParameter()) {
                const auto& params = context.function->GetParams();
                auto found = std::find(params.begin(), params.end(), current);
                if (found == params.end()) {
                    return nullptr;
                }
                auto index = static_cast<size_t>(std::distance(params.begin(), found));
                return index < context.parameterValues.size()
                    ? &context.parameterValues[index]
                    : nullptr;
            }
            if (!current->IsLocalVar()) {
                return nullptr;
            }
            auto expression = StaticCast<LocalVar*>(current)->GetExpr();
            if (expression == nullptr) {
                return nullptr;
            }
            if (expression->GetExprKind() == ExprKind::TYPECAST) {
                return resolve(
                    StaticCast<TypeCast*>(expression)->GetSourceValue(), depth + 1);
            }
            if (expression->GetExprKind() == ExprKind::LOAD) {
                return resolve(StaticCast<Load*>(expression)->GetLocation(), depth + 1);
            }
            const ContextAbstractValue* aggregate = nullptr;
            std::vector<uint64_t> path;
            if (expression->GetExprKind() == ExprKind::GET_ELEMENT_REF) {
                auto element = StaticCast<GetElementRef*>(expression);
                aggregate = resolve(element->GetLocation(), depth + 1);
                path = element->GetPath();
            } else if (expression->GetExprKind() == ExprKind::FIELD) {
                auto field = StaticCast<Field*>(expression);
                aggregate = resolve(field->GetBase(), depth + 1);
                path = field->GetPath();
            } else {
                return nullptr;
            }
            for (auto index : path) {
                if (aggregate == nullptr ||
                    aggregate->kind != ContextAbstractValue::Kind::OBJECT ||
                    index >= aggregate->objectFields.size()) {
                    return nullptr;
                }
                aggregate = &aggregate->objectFields[index];
            }
            return aggregate;
        };
        return resolve(value, 0);
    };

    const auto resolveClassesForValue =
        [&](const EffectContext& context, Value* value) -> ClassSet {
        std::unordered_map<Value*, ClassSet> memo;
        std::unordered_set<Value*> active;
        std::function<ClassSet(Value*, unsigned)> resolve =
            [&](Value* current, unsigned depth) -> ClassSet {
            if (current == nullptr || depth >= MAX_EFFECT_RESOLUTION_DEPTH ||
                resolvedNodes >= MAX_EFFECT_RESOLUTION_NODES) {
                return std::nullopt;
            }
            if (auto found = memo.find(current); found != memo.end()) {
                return found->second;
            }
            if (!active.emplace(current).second) {
                return std::nullopt;
            }
            ++resolvedNodes;
            const auto finish = [&](ClassSet result) {
                active.erase(current);
                memo.emplace(current, result);
                return result;
            };

            if (auto contextValue = resolveEffectContextValue(context, current);
                contextValue != nullptr &&
                (contextValue->kind == ContextAbstractValue::Kind::CLASS ||
                    contextValue->kind == ContextAbstractValue::Kind::OBJECT) &&
                contextValue->classValue != nullptr) {
                return finish(std::vector<ClassType*>{contextValue->classValue});
            }
            if (current->IsParameter() && context.function != nullptr) {
                const auto& params = context.function->GetParams();
                auto found = std::find(params.begin(), params.end(), current);
                if (found != params.end()) {
                    auto index = static_cast<size_t>(std::distance(params.begin(), found));
                    if (index < context.parameterClasses.size() &&
                        !context.parameterClasses[index].empty()) {
                        return finish(context.parameterClasses[index]);
                    }
                }
            }

            std::unordered_set<Value*> exactVisited;
            if (auto exactClass = ResolveExactAllocatedClass(current, exactVisited);
                exactClass != nullptr) {
                return finish(std::vector<ClassType*>{exactClass});
            }
            if (!current->IsLocalVar()) {
                return finish(std::nullopt);
            }
            auto expression = StaticCast<LocalVar*>(current)->GetExpr();
            if (expression == nullptr) {
                return finish(std::nullopt);
            }
            if (expression->GetExprKind() == ExprKind::TYPECAST) {
                return finish(resolve(
                    StaticCast<TypeCast*>(expression)->GetSourceValue(), depth + 1));
            }
            if (expression->GetExprKind() == ExprKind::LOAD) {
                return finish(resolve(
                    StaticCast<Load*>(expression)->GetLocation(), depth + 1));
            }
            if (expression->GetExprKind() == ExprKind::APPLY) {
                auto target =
                    DynamicCast<Function*>(StaticCast<Apply*>(expression)->GetCallee());
                if (target == nullptr || target->GetBody() == nullptr ||
                    !target->HasReturnValue()) {
                    return finish(std::nullopt);
                }
                return finish(resolve(target->GetReturnValue(), depth + 1));
            }
            if (expression->GetExprKind() == ExprKind::APPLY_WITH_EXCEPTION) {
                auto target = DynamicCast<Function*>(
                    StaticCast<ApplyWithException*>(expression)->GetCallee());
                if (target == nullptr || target->GetBody() == nullptr ||
                    !target->HasReturnValue()) {
                    return finish(std::nullopt);
                }
                return finish(resolve(target->GetReturnValue(), depth + 1));
            }

            auto type = current->GetType();
            if (type == nullptr || !type->IsRef() || current->IsGlobal() ||
                current->TestAttr(Attribute::STATIC)) {
                return finish(std::nullopt);
            }
            std::vector<ClassType*> classes;
            bool sawStore = false;
            for (auto user : current->GetUsers()) {
                if (user->GetExprKind() == ExprKind::DEBUGEXPR ||
                    (user->GetExprKind() == ExprKind::LOAD &&
                        StaticCast<const Load*>(user)->GetLocation() == current)) {
                    continue;
                }
                if (user->GetExprKind() != ExprKind::STORE ||
                    StaticCast<const Store*>(user)->GetLocation() != current) {
                    return finish(std::nullopt);
                }
                sawStore = true;
                auto storedClasses =
                    resolve(StaticCast<const Store*>(user)->GetValue(), depth + 1);
                if (!storedClasses.has_value()) {
                    return finish(std::nullopt);
                }
                classes.insert(classes.end(), storedClasses->begin(), storedClasses->end());
                classes = normalizeClasses(std::move(classes));
                if (classes.size() > MAX_EFFECT_DISPATCH_TARGETS) {
                    return finish(std::nullopt);
                }
            }
            return finish(sawStore && !classes.empty()
                ? ClassSet{std::move(classes)}
                : std::nullopt);
        };
        return resolve(value, 0);
    };

    const auto makeChildContext = [&](const Function* target,
                                      const std::vector<Value*>& callArgs,
                                      const EffectContext& parent,
                                      ClassType* receiverClass) -> std::optional<EffectContext> {
        if (target == nullptr || target->GetBody() == nullptr ||
            target->GetParams().size() != callArgs.size()) {
            return std::nullopt;
        }
        EffectContext child;
        child.function = target;
        child.parameterValues.resize(callArgs.size());
        child.parameterClasses.resize(callArgs.size());
        for (size_t i = 0; i < callArgs.size(); ++i) {
            if (auto contextValue = resolveEffectContextValue(parent, callArgs[i]);
                contextValue != nullptr) {
                child.parameterValues[i] = *contextValue;
            }
            auto classes = resolveClassesForValue(parent, callArgs[i]);
            if (classes.has_value()) {
                child.parameterClasses[i] = normalizeClasses(std::move(*classes));
                if (child.parameterValues[i].IsTop() &&
                    child.parameterClasses[i].size() == 1) {
                    child.parameterValues[i] =
                        ContextAbstractValue{child.parameterClasses[i].front()};
                }
            }
        }
        if (receiverClass != nullptr && !child.parameterClasses.empty()) {
            child.parameterClasses.front() = {receiverClass};
            if (child.parameterValues.front().kind ==
                ContextAbstractValue::Kind::OBJECT) {
                child.parameterValues.front().classValue = receiverClass;
            } else {
                child.parameterValues.front() = ContextAbstractValue{receiverClass};
            }
        }
        return child;
    };

    const auto traceEffectReject = [](const char* reason, const Function* function,
                                      const Expression* expression,
                                      const Function* target = nullptr) {
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") == nullptr) {
            return;
        }
        std::cerr << "[RangeAnalysisContextEffectReject] reason=" << reason
                  << " function="
                  << (function == nullptr ? "<null>" : function->GetIdentifier())
                  << " expression-kind="
                  << (expression == nullptr
                          ? -1
                          : static_cast<int>(expression->GetExprKind()))
                  << " line="
                  << (expression == nullptr
                          ? 0
                          : expression->GetDebugLocation().GetBeginPos().line)
                  << " target="
                  << (target == nullptr ? "<null>" : target->GetIdentifier())
                  << '\n';
    };

    while (!worklist.empty()) {
        if (IsContextAnalysisBudgetExhausted() ||
            visitedContexts.size() >= MAX_EFFECT_CONTEXTS ||
            resolvedNodes >= MAX_EFFECT_RESOLUTION_NODES) {
            traceEffectReject("budget", nullptr, nullptr);
            return std::nullopt;
        }
        auto context = std::move(worklist.back());
        worklist.pop_back();
        if (context.function == nullptr || context.function->GetBody() == nullptr) {
            traceEffectReject("missing-context-body", context.function, nullptr);
            return std::nullopt;
        }
        if (!visitedContexts.emplace(buildEffectContextKey(context)).second) {
            continue;
        }

        for (auto block : context.function->GetBody()->GetBlocks()) {
            for (auto expression : block->GetExpressions()) {
                switch (expression->GetExprKind()) {
                    case ExprKind::LOAD:
                        if (!addGlobal(DynamicCast<GlobalVar*>(
                                StaticCast<Load*>(expression)->GetLocation()))) {
                            return std::nullopt;
                        }
                        break;
                    case ExprKind::STORE:
                        if (!addGlobal(DynamicCast<GlobalVar*>(
                                StaticCast<Store*>(expression)->GetLocation()))) {
                            return std::nullopt;
                        }
                        break;
                    case ExprKind::APPLY:
                    case ExprKind::APPLY_WITH_EXCEPTION: {
                        Value* calleeValue = nullptr;
                        std::vector<Value*> callArgs;
                        if (expression->GetExprKind() == ExprKind::APPLY) {
                            auto apply = StaticCast<Apply*>(expression);
                            calleeValue = apply->GetCallee();
                            callArgs = apply->GetArgs();
                        } else {
                            auto apply = StaticCast<ApplyWithException*>(expression);
                            calleeValue = apply->GetCallee();
                            callArgs = apply->GetArgs();
                        }
                        if (auto lambda = IsApplyToLambda(expression);
                            lambda != nullptr && lambda->GetBody() != nullptr) {
                            auto lambdaGlobals =
                                CollectTrackedMutableGlobals(lambda, builder.GetCurPackage());
                            if (!lambdaGlobals.complete) {
                                traceEffectReject(
                                    "incomplete-lambda-effects", context.function,
                                    expression);
                                return std::nullopt;
                            }
                            for (auto global : lambdaGlobals) {
                                if (!addGlobal(global)) {
                                    return std::nullopt;
                                }
                            }
                            break;
                        }
                        auto target = DynamicCast<Function*>(calleeValue);
                        if (target == nullptr || target->GetBody() == nullptr) {
                            if (target != nullptr &&
                                target->TestAttr(Attribute::NO_SIDE_EFFECT)) {
                                break;
                            }
                            traceEffectReject(
                                "unresolved-direct-target", context.function,
                                expression, target);
                            return std::nullopt;
                        }
                        auto child =
                            makeChildContext(target, callArgs, context, nullptr);
                        if (!child.has_value()) {
                            traceEffectReject(
                                "invalid-direct-context", context.function,
                                expression, target);
                            return std::nullopt;
                        }
                        worklist.emplace_back(std::move(*child));
                        break;
                    }
                    case ExprKind::INVOKE:
                    case ExprKind::INVOKE_WITH_EXCEPTION: {
                        Value* receiver = nullptr;
                        std::vector<Value*> callArgs;
                        const Invoke* invoke = nullptr;
                        const InvokeWithException* invokeWithException = nullptr;
                        if (expression->GetExprKind() == ExprKind::INVOKE) {
                            invoke = StaticCast<Invoke*>(expression);
                            receiver = invoke->GetObject();
                            callArgs = invoke->GetArgs();
                        } else {
                            invokeWithException =
                                StaticCast<InvokeWithException*>(expression);
                            receiver = invokeWithException->GetObject();
                            callArgs = invokeWithException->GetArgs();
                        }
                        auto classes = resolveClassesForValue(context, receiver);
                        if (!classes.has_value() || classes->empty() ||
                            classes->size() > MAX_EFFECT_DISPATCH_TARGETS) {
                            traceEffectReject(
                                "unresolved-receiver-classes", context.function,
                                expression);
                            return std::nullopt;
                        }
                        for (auto exactClass : *classes) {
                            auto target = invoke != nullptr
                                ? ResolveExactInvokeTarget(invoke, exactClass, builder)
                                : ResolveExactInvokeTarget(
                                      invokeWithException, exactClass, builder);
                            auto child = makeChildContext(
                                target, callArgs, context, exactClass);
                            if (!child.has_value()) {
                                traceEffectReject(
                                    "invalid-dispatch-context", context.function,
                                    expression, target);
                                return std::nullopt;
                            }
                            worklist.emplace_back(std::move(*child));
                        }
                        break;
                    }
                    case ExprKind::INVOKESTATIC: {
                        auto invoke = StaticCast<InvokeStatic*>(expression);
                        auto attributes = invoke->GetVirtualMethodAttr(builder);
                        auto noSideEffect =
                            attributes.TestAttr(Attribute::NO_SIDE_EFFECT);
                        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                            std::cerr << "[RangeAnalysisContextEffectStatic] function="
                                      << context.function->GetIdentifier()
                                      << " method=" << invoke->GetMethodName()
                                      << " no-side-effect=" << noSideEffect << '\n';
                        }
                        if (noSideEffect) {
                            break;
                        }
                        traceEffectReject(
                            "impure-static-dispatch", context.function, expression);
                        return std::nullopt;
                    }
                    case ExprKind::INVOKESTATIC_WITH_EXCEPTION: {
                        auto invoke =
                            StaticCast<InvokeStaticWithException*>(expression);
                        auto attributes = invoke->GetVirtualMethodAttr(builder);
                        auto noSideEffect =
                            attributes.TestAttr(Attribute::NO_SIDE_EFFECT);
                        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                            std::cerr << "[RangeAnalysisContextEffectStatic] function="
                                      << context.function->GetIdentifier()
                                      << " method=" << invoke->GetMethodName()
                                      << " no-side-effect=" << noSideEffect << '\n';
                        }
                        if (noSideEffect) {
                            break;
                        }
                        traceEffectReject(
                            "impure-static-dispatch", context.function, expression);
                        return std::nullopt;
                    }
                    default:
                        break;
                }
            }
        }
    }

    std::sort(globals.begin(), globals.end());
    return globals;
    };
    auto result = computeClosure();
    {
        std::lock_guard<std::mutex> lock(contextEffectClosureCacheMtx);
        if (contextEffectClosureCache.size() < MaxTotalReachableContextSummaries()) {
            contextEffectClosureCache.emplace(std::move(cacheKey), result);
        }
    }
    return result;
}

struct RangeAnalysis::ContextualSummary {
    ContextSummaryState state{ContextSummaryState::UNSEEN};
    size_t precision{0};
    std::optional<ContextAbstractValue> returnValue;
    std::vector<std::optional<ContextAbstractValue>> refArgValues;
    ContextGlobalValues globalValues;
    std::unique_ptr<Results<RangeDomain>> result;
    const Function* callee{nullptr};
    ContextArguments inputArguments;
    ContextGlobalValues inputGlobalValues;
};

struct RangeAnalysis::LambdaContextualSummary {
    bool ready{false};
    const Lambda* lambda{nullptr};
    std::unordered_map<Block*, RangeDomain> entryStates;
    std::optional<ContextAbstractValue> returnValue;
    std::vector<std::optional<ContextAbstractValue>> refArgValues;
    std::vector<std::pair<Value*, ContextAbstractValue>> capturedValues;
    ContextGlobalValues globalValues;
};

std::mutex& RangeAnalysis::GetContextSummaryMutex()
{
    static std::mutex summaryMtx;
    return summaryMtx;
}

std::unordered_map<std::string, RangeAnalysis::ContextualSummary>& RangeAnalysis::GetContextSummaryCache()
{
    static std::unordered_map<std::string, ContextualSummary> summaryCache;
    return summaryCache;
}

std::vector<std::string>& RangeAnalysis::GetContextSummaryOrder()
{
    static std::vector<std::string> contextOrder;
    return contextOrder;
}

std::unordered_map<const Function*, size_t>& RangeAnalysis::GetContextCounts()
{
    static std::unordered_map<const Function*, size_t> contextCounts;
    return contextCounts;
}

std::unordered_map<const Function*, size_t>& RangeAnalysis::GetBoundedLoopContextCounts()
{
    static std::unordered_map<const Function*, size_t> contextCounts;
    return contextCounts;
}

std::mutex& RangeAnalysis::GetBoundedLoopExitCacheMutex()
{
    static std::mutex cacheMtx;
    return cacheMtx;
}

std::unordered_map<const RangeAnalysis*, RangeAnalysis::BoundedLoopExitCache>&
RangeAnalysis::GetBoundedLoopExitCaches()
{
    static std::unordered_map<const RangeAnalysis*, BoundedLoopExitCache> caches;
    return caches;
}

std::unique_ptr<ValueRange> RangeAnalysis::GetBoundedLoopObservedRange(const Expression* expression)
{
    std::lock_guard<std::mutex> lock(boundedLoopObservationsMtx);
    if (!boundedLoopObservationsComplete) {
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr && expression != nullptr) {
            std::cerr << "[RangeAnalysisBoundedLookup] expression=" << expression
                      << " complete=0 found=0\n";
        }
        return nullptr;
    }
    auto found = boundedLoopObservations.find(expression);
    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr && expression != nullptr) {
        const auto& location = expression->GetDebugLocation();
        std::cerr << "[RangeAnalysisBoundedLookup] expression=" << expression
                  << " kind=" << static_cast<int>(expression->GetExprKind())
                  << " block="
                  << (expression->GetParentBlock() == nullptr
                          ? "<null>"
                          : expression->GetParentBlock()->GetIdentifier())
                  << " line=" << location.GetBeginPos().line
                  << " complete=1 found=" << (found != boundedLoopObservations.end())
                  << " range=" << (found == boundedLoopObservations.end()
                          ? "none"
                          : found->second->ToString())
                  << '\n';
    }
    return found == boundedLoopObservations.end() ? nullptr : found->second->Clone();
}

std::unique_ptr<ValueRange> RangeAnalysis::GetBoundedLoopPrefixObservedRange(
    const Expression* expression)
{
    std::lock_guard<std::mutex> lock(boundedLoopObservationsMtx);
    auto found = boundedLoopPrefixObservations.find(expression);
    return found == boundedLoopPrefixObservations.end()
        ? nullptr : found->second->Clone();
}

std::unique_ptr<ValueRange> RangeAnalysis::JoinSupplementalLoopEvidence(
    const ValueRange& complete, const ValueRange& evidence)
{
    if (complete.GetRangeKind() != evidence.GetRangeKind()) {
        return complete.Clone();
    }
    const auto ordinaryJoin = [&]() {
        auto result = complete.Clone();
        if (auto joined = result->Join(evidence); joined.has_value()) {
            result = std::move(joined.value());
        }
        return result;
    };
    if (complete.GetRangeKind() != ValueRange::RangeKind::SINT) {
        return ordinaryJoin();
    }

    const auto& completeRange = StaticCast<const SIntRange&>(complete);
    const auto& evidenceRange = StaticCast<const SIntRange&>(evidence);
    if (!evidenceRange.GetExactValues().has_value()) {
        return ordinaryJoin();
    }
    std::vector<SInt> missingValues;
    for (const auto& value : *evidenceRange.GetExactValues()) {
        if (!SIntRangeContainsExactValue(completeRange, value)) {
            missingValues.emplace_back(value);
        }
    }
    if (missingValues.empty()) {
        return complete.Clone();
    }

    auto joined = ordinaryJoin();
    auto* joinedRange = StaticCast<const SIntRange*>(joined.get());
    if (completeRange.GetExactValues().has_value() ||
        completeRange.GetExcludedValues().has_value() ||
        joinedRange->GetExactValues().has_value()) {
        return joined;
    }
    auto fragments = GetSIntIntervalFragmentsForUnion(completeRange);
    if (!fragments.has_value()) {
        return joined;
    }
    for (const auto& value : missingValues) {
        fragments->emplace_back(SIntIntervalFragment{value, value, 1, 0});
    }
    auto normalizedFragments = NormalizeSIntIntervalFragments(
        std::move(*fragments), joinedRange->GetVal());
    if (!normalizedFragments.has_value()) {
        return joined;
    }
    return std::make_unique<SIntRange>(joinedRange->GetVal(),
        joinedRange->GetExactValues(), joinedRange->GetCongruence(),
        joinedRange->GetKnownBits(), joinedRange->GetExcludedValues(),
        std::move(normalizedFragments));
}

std::unique_ptr<ValueRange> RangeAnalysis::GetLocalBoundedLoopObservedRange(
    const Expression* expression) const
{
    if (expression == nullptr ||
        incompleteLocalBoundedLoopObservations.find(expression) !=
            incompleteLocalBoundedLoopObservations.end()) {
        return nullptr;
    }
    auto found = localBoundedLoopObservations.find(expression);
    return found == localBoundedLoopObservations.end() ? nullptr : found->second->Clone();
}

std::unique_ptr<ValueRange> RangeAnalysis::GetLocalBoundedLoopPrefixObservedRange(
    const Expression* expression) const
{
    std::lock_guard<std::mutex> lock(boundedLoopObservationsMtx);
    auto owner = localBoundedLoopPrefixObservations.find(this);
    if (owner == localBoundedLoopPrefixObservations.end()) {
        return nullptr;
    }
    auto found = owner->second.find(expression);
    return found == owner->second.end() ? nullptr : found->second->Clone();
}

void RangeAnalysis::ClearBoundedLoopObservedRanges()
{
    std::lock_guard<std::mutex> lock(boundedLoopObservationsMtx);
    boundedLoopObservations.clear();
    boundedLoopPrefixObservations.clear();
    localBoundedLoopPrefixObservations.clear();
    completedBoundedLoopPrefixAttempts.clear();
    boundedLoopObservationsComplete = true;
}

bool RangeAnalysis::VisitReachableContextSensitiveResults(
    const Function* rootFunction, Results<RangeDomain>&, const ContextResultVisitor& visitor)
{
    if (IsRangeAnalysisPerfCollectionEnabled()) {
        ++rangeAnalysisPerfStats.reachableVisitCalls;
    }
    ScopedRangeAnalysisPerfTimer timer(&rangeAnalysisPerfStats.reachableVisitNanos);
    std::vector<std::string> worklist;
    std::unordered_set<std::string> discovered;
    bool complete = true;
    {
        std::lock_guard<std::mutex> lock(boundedLoopCallContextsMtx);
        complete = analyzedCallContextRecordingComplete;
    }
    const auto enqueue = [&](const std::string& context) {
        if (discovered.find(context) != discovered.end()) {
            return;
        }
        if (discovered.size() >= MaxTotalReachableContextSummaries()) {
            complete = false;
            return;
        }
        discovered.emplace(context);
        worklist.emplace_back(context);
    };
    const auto collectCalls = [&](const std::string& parentContext) {
        auto recorded = GetRecordedChildContexts(parentContext);
        complete = complete && recorded.complete;
        for (const auto& context : recorded.keys) {
            enqueue(context);
        }
    };

    collectCalls(BuildRootAnalysisContextKey(rootFunction));
    while (!worklist.empty()) {
        auto key = std::move(worklist.back());
        worklist.pop_back();
        const Function* callee = nullptr;
        Results<RangeDomain>* result = nullptr;
        {
            std::lock_guard<std::mutex> lock(GetContextSummaryMutex());
            auto found = GetContextSummaryCache().find(key);
            if (found != GetContextSummaryCache().end() &&
                found->second.state == ContextSummaryState::READY &&
                found->second.result != nullptr) {
                callee = found->second.callee;
                result = found->second.result.get();
            }
        }
        if (callee == nullptr || result == nullptr) {
            complete = false;
            continue;
        }
        visitor(callee, key, *result);
        collectCalls(key);
    }
    rangeAnalysisPerfStats.maxReachableContexts =
        std::max(rangeAnalysisPerfStats.maxReachableContexts, discovered.size());
    return complete;
}

void RangeAnalysis::VisitContextSensitiveLambdaResults(
    const ContextExpressionVisitor& actionBeforeVisitExpr,
    const ContextExpressionVisitor& actionAfterVisitExpr,
    const ContextTerminatorVisitor& actionOnTerminator)
{
    std::vector<LambdaContextualSummary*> summaries;
    summaries.reserve(lambdaContextSummaries.size());
    for (auto& [_, summary] : lambdaContextSummaries) {
        if (summary->ready && summary->lambda != nullptr && summary->lambda->GetBody() != nullptr) {
            summaries.emplace_back(summary.get());
        }
    }

    for (auto summary : summaries) {
        for (auto block : summary->lambda->GetBody()->GetBlocks()) {
            auto found = summary->entryStates.find(block);
            if (found == summary->entryStates.end() || found->second.IsBottom()) {
                continue;
            }
            auto state = found->second;
            auto expressions = block->GetNonTerminatorExpressions();
            for (size_t index = 0; index < expressions.size(); ++index) {
                auto expression = expressions[index];
                actionBeforeVisitExpr(state, expression, index);
                if (expression->GetExprKind() == ExprKind::LAMBDA) {
                    PreHandleLambdaExpression(state, StaticCast<const Lambda*>(expression));
                } else {
                    PropagateExpressionEffect(state, expression);
                }
                actionAfterVisitExpr(state, expression, index);
            }
            auto terminator = block->GetTerminator();
            if (terminator == nullptr) {
                continue;
            }
            auto target = PropagateTerminatorEffect(state, terminator);
            actionOnTerminator(state, terminator, target);
        }
    }
}

void RangeAnalysis::ClearContextSensitiveResults()
{
    if (IsRangeAnalysisStatsEnabled() && rangeAnalysisStatsSessionActive) {
        size_t summaryCount = 0;
        size_t readySummaryCount = 0;
        size_t computingSummaryCount = 0;
        size_t failedSummaryCount = 0;
        size_t failedKeyCount = 0;
        size_t regularContextCount = 0;
        size_t boundedContextCount = 0;
        size_t maxContextsPerFunction = 0;
        size_t analysisCount = 0;
        size_t cacheHitCount = 0;
        size_t recursiveHitCount = 0;
        size_t budgetRejectCount = 0;
        size_t analysisFailureCount = 0;
        size_t peakDepth = 0;
        {
            std::lock_guard<std::mutex> lock(GetContextSummaryMutex());
            const auto& summaryCache = GetContextSummaryCache();
            summaryCount = summaryCache.size();
            for (const auto& [_, summary] : summaryCache) {
                switch (summary.state) {
                    case ContextSummaryState::READY:
                        ++readySummaryCount;
                        break;
                    case ContextSummaryState::COMPUTING:
                        ++computingSummaryCount;
                        break;
                    case ContextSummaryState::FAILED:
                        ++failedSummaryCount;
                        break;
                    case ContextSummaryState::UNSEEN:
                    default:
                        break;
                }
            }
            failedKeyCount = failedContextKeys.size();
            for (const auto& [_, count] : GetContextCounts()) {
                regularContextCount += count;
                maxContextsPerFunction = std::max(maxContextsPerFunction, count);
            }
            for (const auto& [_, count] : GetBoundedLoopContextCounts()) {
                boundedContextCount += count;
                maxContextsPerFunction = std::max(maxContextsPerFunction, count);
            }
            analysisCount = contextSummaryAnalysisCount;
            cacheHitCount = contextSummaryCacheHitCount;
            recursiveHitCount = contextSummaryRecursiveHitCount;
            budgetRejectCount = contextSummaryBudgetRejectCount;
            analysisFailureCount = contextSummaryFailedCount;
            peakDepth = contextSummaryPeakDepth;
        }

        size_t boundedObservationCount = 0;
        bool boundedObservationsAreComplete = true;
        {
            std::lock_guard<std::mutex> lock(boundedLoopObservationsMtx);
            boundedObservationCount = boundedLoopObservations.size();
            boundedObservationsAreComplete = boundedLoopObservationsComplete;
        }
        size_t recordedCallContextCount = 0;
        bool recordedCallContextsAreComplete = true;
        {
            std::lock_guard<std::mutex> lock(boundedLoopCallContextsMtx);
            recordedCallContextCount = analyzedCallContextCount;
            recordedCallContextsAreComplete = analyzedCallContextRecordingComplete;
        }
        size_t objectIdentityCount = 0;
        {
            std::lock_guard<std::mutex> lock(contextObjectIdentitiesMtx);
            objectIdentityCount = contextObjectIdentities.size();
        }
        std::string firstBudgetStage;
        std::string firstBudgetFunction;
        uint64_t firstBudgetLine = 0;
        {
            std::lock_guard<std::mutex> lock(contextBudgetTraceMtx);
            firstBudgetStage = contextBudgetFirstStage;
            firstBudgetFunction = contextBudgetFirstFunction;
            firstBudgetLine = contextBudgetFirstLine;
        }
        std::string budgetReason{"none"};
        switch (contextBudgetExhaustionReason.load(std::memory_order_relaxed)) {
            case ContextBudgetExhaustionReason::WALL_TIME:
                budgetReason = "wall-time";
                break;
            case ContextBudgetExhaustionReason::WORK_UNITS:
                budgetReason = "work-units";
                break;
            case ContextBudgetExhaustionReason::NONE:
            default:
                break;
        }
        uint64_t finiteClassFailureCount = 0;
        for (const auto& [_, count] : finiteClassFailureReasons) {
            finiteClassFailureCount += count;
        }
        uint64_t contextTopCount = 0;
        for (const auto& [_, count] : contextTopSources) {
            contextTopCount += count;
        }
        const auto internalElapsedNanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - rangeAnalysisStatsSessionStart);
        const auto internalElapsedMilliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(internalElapsedNanoseconds);
        std::ostringstream json;
        json << "{\"schema\":1,\"internal_elapsed_ns\":"
             << internalElapsedNanoseconds.count()
             << ",\"internal_elapsed_ms\":" << internalElapsedMilliseconds.count()
             << ",\"config\":{\"loop_widening_start_times\":"
             << rangeAnalysisConfig.loopWideningStartTimes
             << ",\"max_inqueue_times\":" << rangeAnalysisConfig.maxInqueueTimes
             << ",\"max_context_per_function\":"
             << rangeAnalysisConfig.maxContextPerFunction
             << ",\"max_total_context_summaries\":"
             << rangeAnalysisConfig.maxTotalContextSummaries
             << ",\"max_context_analysis_depth\":"
             << rangeAnalysisConfig.maxContextAnalysisDepth
             << ",\"max_exact_int_set_size\":"
             << rangeAnalysisConfig.maxExactIntSetSize
             << ",\"max_bounded_loop_observations\":"
             << rangeAnalysisConfig.maxBoundedLoopObservations
             << ",\"max_recorded_call_contexts\":"
             << rangeAnalysisConfig.maxRecordedCallContexts
             << "},\"context\":{\"summaries\":" << summaryCount
             << ",\"ready\":" << readySummaryCount
             << ",\"computing\":" << computingSummaryCount
             << ",\"failed\":" << failedSummaryCount
             << ",\"failed_keys\":" << failedKeyCount
             << ",\"regular_contexts\":" << regularContextCount
             << ",\"bounded_contexts\":" << boundedContextCount
             << ",\"max_per_function\":" << maxContextsPerFunction
             << ",\"analyses\":" << analysisCount
             << ",\"cache_hits\":" << cacheHitCount
             << ",\"recursive_hits\":" << recursiveHitCount
             << ",\"budget_rejects\":" << budgetRejectCount
             << ",\"analysis_failures\":" << analysisFailureCount
             << ",\"peak_depth\":" << peakDepth
             << ",\"work_units\":"
             << contextAnalysisWorkUnits.load(std::memory_order_relaxed)
             << ",\"budget_exhausted\":"
             << (contextAnalysisBudgetExhausted.load(std::memory_order_relaxed)
                     ? "true"
                     : "false")
             << ",\"budget_reason\":";
        WriteJsonString(json, budgetReason);
        json << ",\"first_budget_stage\":";
        WriteJsonString(json, firstBudgetStage.empty() ? "none" : firstBudgetStage);
        json << ",\"first_budget_elapsed_ms\":"
             << contextBudgetFirstElapsedMilliseconds.load(std::memory_order_relaxed)
             << ",\"first_budget_function\":";
        WriteJsonString(json, firstBudgetFunction.empty() ? "none" : firstBudgetFunction);
        json << ",\"first_budget_line\":" << firstBudgetLine
             << "},\"limits\":{\"loop_widening_applications\":"
             << rangeAnalysisTuningStats.loopWideningApplications
             << ",\"max_inqueue_count\":" << rangeAnalysisTuningStats.maxInqueueCount
             << ",\"inqueue_limit_hits\":" << rangeAnalysisTuningStats.inqueueLimitHits
             << ",\"exact_set_limit_rejects\":"
             << rangeAnalysisTuningStats.exactSetLimitRejects
             << ",\"bounded_loop_observations\":" << boundedObservationCount
             << ",\"bounded_loop_observations_complete\":"
             << (boundedObservationsAreComplete ? "true" : "false")
             << ",\"recorded_call_contexts\":" << recordedCallContextCount
             << ",\"recorded_call_contexts_complete\":"
             << (recordedCallContextsAreComplete ? "true" : "false")
             << ",\"context_object_identities\":" << objectIdentityCount
             << "},\"perf\":{\"finite_class_resolve_calls\":"
             << rangeAnalysisPerfStats.finiteClassResolveCalls
             << ",\"finite_class_resolve_nodes\":"
             << rangeAnalysisPerfStats.finiteClassResolveNodes
             << ",\"finite_class_memo_hits\":"
             << rangeAnalysisPerfStats.finiteClassMemoHits
             << ",\"finite_class_budget_rejects\":"
             << rangeAnalysisPerfStats.finiteClassBudgetRejects
             << ",\"finite_class_failure_count\":" << finiteClassFailureCount
             << ",\"finite_class_resolve_ns\":"
             << rangeAnalysisPerfStats.finiteClassResolveNanos
             << ",\"finite_invoke_calls\":"
             << rangeAnalysisPerfStats.finiteInvokeCalls
             << ",\"finite_invoke_ns\":" << rangeAnalysisPerfStats.finiteInvokeNanos
             << ",\"dispatch_merge_calls\":"
             << rangeAnalysisPerfStats.finiteDispatchMergeCalls
             << ",\"dispatch_merge_ns\":"
             << rangeAnalysisPerfStats.finiteDispatchMergeNanos
             << ",\"dispatch_state_copies\":"
             << rangeAnalysisPerfStats.finiteDispatchStateCopies
             << ",\"dispatch_state_joins\":"
             << rangeAnalysisPerfStats.finiteDispatchStateJoins
             << ",\"dispatch_target_resolve_calls\":"
             << rangeAnalysisPerfStats.dispatchTargetResolveCalls
             << ",\"dispatch_target_resolve_ns\":"
             << rangeAnalysisPerfStats.dispatchTargetResolveNanos
             << ",\"context_effect_closure_calls\":"
             << rangeAnalysisPerfStats.contextEffectClosureCalls
             << ",\"context_effect_closure_cache_hits\":"
             << rangeAnalysisPerfStats.contextEffectClosureCacheHits
             << ",\"context_effect_closure_ns\":"
             << rangeAnalysisPerfStats.contextEffectClosureNanos
             << ",\"bounded_invoke_replays\":"
             << rangeAnalysisPerfStats.boundedInvokeReplays
             << ",\"context_key_calls\":" << rangeAnalysisPerfStats.contextKeyCalls
             << ",\"context_key_ns\":" << rangeAnalysisPerfStats.contextKeyNanos
             << ",\"context_analysis_calls\":"
             << rangeAnalysisPerfStats.contextAnalysisCalls
             << ",\"context_analysis_ns\":"
             << rangeAnalysisPerfStats.contextAnalysisNanos
             << ",\"context_ready_cache_hits\":"
             << rangeAnalysisPerfStats.contextReadyCacheHits
             << ",\"context_failed_cache_hits\":"
             << rangeAnalysisPerfStats.contextFailedCacheHits
             << ",\"context_computing_cache_hits\":"
             << rangeAnalysisPerfStats.contextComputingCacheHits
             << ",\"context_budget_rejects\":"
             << rangeAnalysisPerfStats.contextBudgetRejects
             << ",\"context_summary_instantiations\":"
             << rangeAnalysisPerfStats.contextSummaryInstantiations
             << ",\"context_summary_instantiation_ns\":"
             << rangeAnalysisPerfStats.contextSummaryInstantiationNanos
             << ",\"context_new_analyses\":"
             << rangeAnalysisPerfStats.contextNewAnalyses
             << ",\"context_fixpoint_ns\":"
             << rangeAnalysisPerfStats.contextFixpointNanos
             << ",\"context_summary_build_ns\":"
             << rangeAnalysisPerfStats.contextSummaryBuildNanos
             << ",\"reachable_visit_calls\":"
             << rangeAnalysisPerfStats.reachableVisitCalls
             << ",\"reachable_visit_ns\":"
             << rangeAnalysisPerfStats.reachableVisitNanos
             << ",\"max_class_depth\":" << rangeAnalysisPerfStats.maxFiniteClassDepth
             << ",\"max_classes\":" << rangeAnalysisPerfStats.maxFiniteClassCount
             << ",\"max_targets\":" << rangeAnalysisPerfStats.maxFiniteTargetCount
             << ",\"max_reachable_contexts\":"
             << rangeAnalysisPerfStats.maxReachableContexts
             << ",\"context_top_count\":" << contextTopCount
             << ",\"first_context_top_source\":";
        WriteJsonString(json, firstContextTopSource.empty() ? "none" : firstContextTopSource);
        json << "}}";

        std::ofstream output(GetRangeAnalysisStatsFile(), std::ios::out | std::ios::app);
        if (output) {
            output << json.str() << '\n';
        }
    }

    {
        std::lock_guard<std::mutex> lock(GetContextSummaryMutex());
        auto& summaryCache = GetContextSummaryCache();
        if (IsRangeAnalysisPerfTraceEnabled() &&
            (!summaryCache.empty() || !failedContextKeys.empty() ||
                contextAnalysisBudgetExhausted.load(std::memory_order_relaxed))) {
            const auto countState = [&summaryCache](ContextSummaryState state) {
                return static_cast<size_t>(std::count_if(
                    summaryCache.begin(), summaryCache.end(),
                    [state](const auto& entry) { return entry.second.state == state; }));
            };
            const auto budgetReason = []() {
                switch (contextBudgetExhaustionReason.load(std::memory_order_relaxed)) {
                    case ContextBudgetExhaustionReason::WALL_TIME:
                        return "wall-time";
                    case ContextBudgetExhaustionReason::WORK_UNITS:
                        return "work-units";
                    case ContextBudgetExhaustionReason::NONE:
                    default:
                        return "none";
                }
            };
            std::string firstBudgetStage;
            std::string firstBudgetFunction;
            uint64_t firstBudgetLine = 0;
            {
                std::lock_guard<std::mutex> budgetLock(contextBudgetTraceMtx);
                firstBudgetStage = contextBudgetFirstStage;
                firstBudgetFunction = contextBudgetFirstFunction;
                firstBudgetLine = contextBudgetFirstLine;
            }
            std::cerr << "[RangeAnalysisContextStats] summaries=" << summaryCache.size()
                      << " ready=" << countState(ContextSummaryState::READY)
                      << " computing=" << countState(ContextSummaryState::COMPUTING)
                      << " failed=" << countState(ContextSummaryState::FAILED)
                      << " failed-keys=" << failedContextKeys.size()
                      << " analyses=" << contextSummaryAnalysisCount
                      << " cache-hits=" << contextSummaryCacheHitCount
                      << " recursive-hits=" << contextSummaryRecursiveHitCount
                      << " budget-rejects=" << contextSummaryBudgetRejectCount
                      << " analysis-failures=" << contextSummaryFailedCount
                      << " peak-depth=" << contextSummaryPeakDepth
                      << " work-units="
                      << contextAnalysisWorkUnits.load(std::memory_order_relaxed)
                      << " work-exhausted="
                      << contextAnalysisBudgetExhausted.load(std::memory_order_relaxed)
                      << " budget-ms=" << GetContextAnalysisWallTime().count()
                      << " budget-reason=" << budgetReason()
                      << " first-budget-stage="
                      << (firstBudgetStage.empty() ? "none" : firstBudgetStage)
                      << " first-budget-function="
                      << (firstBudgetFunction.empty() ? "none" : firstBudgetFunction)
                      << " first-budget-line=" << firstBudgetLine
                      << " first-budget-elapsed-ms="
                      << contextBudgetFirstElapsedMilliseconds.load(std::memory_order_relaxed)
                      << '\n';
            std::vector<std::pair<const Function*, size_t>> functionContexts(
                GetContextCounts().begin(), GetContextCounts().end());
            std::sort(functionContexts.begin(), functionContexts.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });
            std::cerr << "[RangeAnalysisContextsPerFunction]";
            for (const auto& [function, count] : functionContexts) {
                std::cerr << ' '
                          << (function == nullptr ? "<null>" : function->GetIdentifier())
                          << '=' << count;
            }
            std::cerr << '\n';
        }
        summaryCache.clear();
        GetContextSummaryOrder().clear();
        GetContextCounts().clear();
        GetBoundedLoopContextCounts().clear();
        failedContextKeys.clear();
        contextSummaryAnalysisCount = 0;
        contextSummaryCacheHitCount = 0;
        contextSummaryRecursiveHitCount = 0;
        contextSummaryBudgetRejectCount = 0;
        contextSummaryFailedCount = 0;
        contextSummaryPeakDepth = 0;
    }
    contextAnalysisWorkUnits.store(0, std::memory_order_relaxed);
    contextAnalysisBudgetExhausted.store(false, std::memory_order_relaxed);
    contextBudgetExhaustionReason.store(
        ContextBudgetExhaustionReason::NONE, std::memory_order_relaxed);
    contextBudgetFirstElapsedMilliseconds.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(contextBudgetTraceMtx);
        contextBudgetFirstStage.clear();
        contextBudgetFirstFunction.clear();
        contextBudgetFirstLine = 0;
    }
    contextAnalysisBudgetStart = std::chrono::steady_clock::now();
    contextSummaryAnalysisDepth = 0;
    overrideTraceInvokeLogCount = 0;
    PrintAndResetRangeAnalysisPerfStats();
    rangeAnalysisTuningStats = RangeAnalysisTuningStats{};
    rangeAnalysisStatsSessionActive = IsRangeAnalysisStatsEnabled();
    rangeAnalysisStatsSessionStart = std::chrono::steady_clock::now();
    ClearBoundedLoopCallContexts();
    {
        std::lock_guard<std::mutex> lock(contextEffectClosureCacheMtx);
        contextEffectClosureCache.clear();
    }
    {
        std::lock_guard<std::mutex> lock(GetBoundedLoopExitCacheMutex());
        GetBoundedLoopExitCaches().clear();
    }
    ClearTrackedMutableGlobalCache();
    ClearContextObjectIdentities();
    ClearLoopForestCache();
}

ClassType* RangeAnalysis::GetRecordedContextObjectClass(const Value* object) const
{
    if (object == nullptr) {
        return nullptr;
    }
    auto found = contextObjectClasses.find(object);
    return found == contextObjectClasses.end() ? nullptr : found->second;
}

void RangeAnalysis::RecordContextObjectClass(
    const Value* object, ClassType* exactClass) const
{
    if (object != nullptr && exactClass != nullptr) {
        contextObjectClasses[object] = exactClass;
    }
}

RangeAnalysis::ContextAbstractValue RangeAnalysis::CaptureContextValue(
    const RangeDomain& state, Value* value, bool preserveIntervals) const
{
    if (value == nullptr) {
        return ContextAbstractValue{};
    }
    if (value->IsLocalVar()) {
        auto expression = StaticCast<LocalVar*>(value)->GetExpr();
        if (expression != nullptr && expression->GetExprKind() == ExprKind::LAMBDA) {
            auto lambda = StaticCast<const Lambda*>(expression);
            if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                std::cerr << "[RangeAnalysisLambdaContext] captures="
                          << lambda->GetCapturedVariables().size();
                for (auto captured : lambda->GetCapturedVariables()) {
                    std::cerr << " type="
                              << (captured == nullptr || captured->GetType() == nullptr
                                      ? "<null>"
                                      : captured->GetType()->ToString())
                              << " readonly="
                              << (captured != nullptr && captured->TestAttr(Attribute::READONLY));
                }
                std::cerr << '\n';
            }
            constexpr size_t MAX_CONTEXT_LAMBDA_CAPTURES = 16;
            auto capturedVariables = lambda->GetCapturedVariables();
            if (capturedVariables.size() > MAX_CONTEXT_LAMBDA_CAPTURES) {
                return ContextAbstractValue{};
            }
            std::vector<ContextAbstractValue> capturedValues;
            capturedValues.reserve(capturedVariables.size());
            for (auto captured : capturedVariables) {
                auto capturedType = captured == nullptr ? nullptr : captured->GetType();
                if (capturedType == nullptr || capturedType->IsRef() ||
                    (!capturedType->IsBoolean() && !capturedType->IsInteger())) {
                    return ContextAbstractValue{};
                }
                auto capturedValue = CaptureContextValue(
                    state, captured, /* preserveIntervals = */ true);
                if (capturedValue.IsTop()) {
                    return ContextAbstractValue{};
                }
                capturedValues.emplace_back(std::move(capturedValue));
            }
            return ContextAbstractValue{lambda, std::move(capturedValues)};
        }
        if (expression != nullptr && expression->GetExprKind() == ExprKind::CONSTANT) {
            auto literal = StaticCast<Constant*>(expression)->GetValue();
            if (literal != nullptr && !literal->IsNullLiteral()) {
                auto literalDomain = HandleNonNullLiteralValue<RangeValueDomain>(literal);
                auto literalRange = literalDomain.CheckAbsVal();
                if (literalRange != nullptr && literalRange->GetRangeKind() == ValueRange::RangeKind::BOOL) {
                    return ContextAbstractValue{StaticCast<const BoolRange*>(literalRange)->GetVal()};
                }
                if (literalRange != nullptr && literalRange->GetRangeKind() == ValueRange::RangeKind::SINT) {
                    const auto* sintRange = StaticCast<const SIntRange*>(literalRange);
                    return ContextAbstractValue{
                        sintRange->GetVal(), sintRange->GetExactValues(), sintRange->GetCongruence(),
                        sintRange->GetKnownBits(), sintRange->GetExcludedValues(),
                        sintRange->GetIntervalFragments()};
                }
            }
        }
    }
    std::unordered_set<Value*> visited;
    auto exactClass = ResolveExactAllocatedClass(value, visited);
    auto type = value->GetType();
    if (type->IsRef()) {
        type = StaticCast<RefType*>(type)->GetRootBaseType();
        auto object = state.CheckAbstractObjectRefBy(value);
        if (type != nullptr && (type->IsClass() || type->IsStruct() || type->IsTuple())) {
            if (exactClass != nullptr) {
                RecordContextObjectClass(object, exactClass);
            }
            auto aggregate = CaptureContextValue(state, object, type, preserveIntervals);
            if (aggregate.kind == ContextAbstractValue::Kind::OBJECT) {
                aggregate.classValue =
                    exactClass != nullptr ? exactClass : GetRecordedContextObjectClass(object);
            }
            return aggregate;
        }
        auto scalar = CaptureContextValue(state, object, type, preserveIntervals);
        return scalar.IsTop() && exactClass != nullptr ? ContextAbstractValue{exactClass} : scalar;
    }
    if (exactClass != nullptr) {
        return ContextAbstractValue{exactClass};
    }
    return CaptureContextValue(state, value, type, preserveIntervals);
}

RangeAnalysis::ContextAbstractValue RangeAnalysis::CaptureContextValue(
    const RangeDomain& state, Value* value, Type* type, bool preserveIntervals) const
{
    if (value == nullptr || type == nullptr) {
        return ContextAbstractValue{};
    }
    if (type->IsClass() || type->IsStruct() || type->IsTuple() || type->IsRawArray()) {
        constexpr size_t MAX_CONTEXT_OBJECT_DEPTH = 4;
        constexpr size_t MAX_CONTEXT_OBJECT_NODES = 128;
        constexpr size_t MAX_CONTEXT_OBJECT_FIELDS = 64;
        size_t remainingNodes = MAX_CONTEXT_OBJECT_NODES;
        std::unordered_set<Value*> visitedObjects;
        std::function<ContextAbstractValue(Value*, size_t)> captureObject =
            [&](Value* object, size_t depth) -> ContextAbstractValue {
            if (object == nullptr || depth >= MAX_CONTEXT_OBJECT_DEPTH || remainingNodes == 0 ||
                !visitedObjects.emplace(object).second) {
                return ContextAbstractValue{};
            }
            auto objectIdentity = GetContextObjectIdentity(object);
            if (objectIdentity == 0) {
                return ContextAbstractValue{};
            }
            std::vector<ContextAbstractValue> fields;
            auto children = const_cast<RangeDomain&>(state).GetChildren(object);
            auto trackedFields = std::min(children.size(), MAX_CONTEXT_OBJECT_FIELDS);
            fields.reserve(trackedFields);
            for (size_t i = 0; i < trackedFields; ++i) {
                if (remainingNodes == 0) {
                    fields.emplace_back();
                    continue;
                }
                --remainingNodes;
                auto child = children[i];
                auto domain = state.CheckAbstractValueWithTopBottom(child);
                if (domain == nullptr || domain->IsTop()) {
                    fields.emplace_back();
                    continue;
                }
                if (domain->GetKind() == RangeValueDomain::ValueKind::REF) {
                    fields.emplace_back(captureObject(state.CheckAbstractObjectRefBy(child), depth + 1));
                    continue;
                }
                auto absVal = domain->CheckAbsVal();
                if (absVal == nullptr) {
                    auto nestedChildren = const_cast<RangeDomain&>(state).GetChildren(child);
                    fields.emplace_back(nestedChildren.empty()
                            ? ContextAbstractValue{}
                            : captureObject(child, depth + 1));
                    continue;
                }
                if (absVal->GetRangeKind() == ValueRange::RangeKind::BOOL) {
                    auto boolDomain = StaticCast<const BoolRange*>(absVal)->GetVal();
                    fields.emplace_back(boolDomain.IsNonTrivial() &&
                            (preserveIntervals || boolDomain.IsSingleValue())
                        ? ContextAbstractValue{boolDomain}
                        : ContextAbstractValue{});
                    continue;
                }
                if (absVal->GetRangeKind() == ValueRange::RangeKind::SINT) {
                    const auto* sintRange = StaticCast<const SIntRange*>(absVal);
                    const auto& sintDomain = sintRange->GetVal();
                    fields.emplace_back((sintDomain.IsNonTrivial() || sintRange->GetCongruence().has_value() ||
                            sintRange->GetKnownBits().has_value() || sintRange->GetExcludedValues().has_value() ||
                            sintRange->GetIntervalFragments().has_value()) &&
                            (preserveIntervals || sintDomain.IsSingleValue() ||
                                sintRange->GetExactValues().has_value() || sintRange->GetCongruence().has_value() ||
                                sintRange->GetKnownBits().has_value() || sintRange->GetExcludedValues().has_value() ||
                                sintRange->GetIntervalFragments().has_value())
                        ? ContextAbstractValue{
                              sintDomain, sintRange->GetExactValues(), sintRange->GetCongruence(),
                              sintRange->GetKnownBits(), sintRange->GetExcludedValues(),
                              sintRange->GetIntervalFragments()}
                        : ContextAbstractValue{});
                    continue;
                }
                fields.emplace_back();
            }
            return ContextAbstractValue{
                GetRecordedContextObjectClass(object), std::move(fields), objectIdentity};
        };
        return captureObject(value, 0);
    }
    auto domain = state.CheckAbstractValueWithTopBottom(value);
    if (domain == nullptr || domain->IsTop()) {
        return ContextAbstractValue{};
    }
    auto absVal = domain->CheckAbsVal();
    if (absVal == nullptr) {
        return ContextAbstractValue{};
    }
    if (type->IsBoolean()) {
        if (absVal->GetRangeKind() != ValueRange::RangeKind::BOOL) {
            return ContextAbstractValue{};
        }
        auto boolDomain = StaticCast<const BoolRange*>(absVal)->GetVal();
        if (!boolDomain.IsNonTrivial() || (!preserveIntervals && !boolDomain.IsSingleValue())) {
            return ContextAbstractValue{};
        }
        return ContextAbstractValue{std::move(boolDomain)};
    }
    if (type->IsInteger()) {
        if (absVal->GetRangeKind() != ValueRange::RangeKind::SINT) {
            return ContextAbstractValue{};
        }
        const auto* sintRange = StaticCast<const SIntRange*>(absVal);
        const auto& sintDomain = sintRange->GetVal();
        const auto& exactValues = sintRange->GetExactValues();
        if ((!sintDomain.IsNonTrivial() && !sintRange->GetCongruence().has_value() &&
                !sintRange->GetKnownBits().has_value() && !sintRange->GetExcludedValues().has_value() &&
                !sintRange->GetIntervalFragments().has_value()) ||
            (!preserveIntervals && !sintDomain.IsSingleValue() && !exactValues.has_value() &&
                !sintRange->GetCongruence().has_value() && !sintRange->GetKnownBits().has_value() &&
                !sintRange->GetExcludedValues().has_value() &&
                !sintRange->GetIntervalFragments().has_value())) {
            return ContextAbstractValue{};
        }
        return ContextAbstractValue{
            sintDomain, exactValues, sintRange->GetCongruence(), sintRange->GetKnownBits(),
            sintRange->GetExcludedValues(), sintRange->GetIntervalFragments()};
    }
    return ContextAbstractValue{};
}

RangeAnalysis::ContextAbstractValue RangeAnalysis::JoinContextValues(
    const ContextAbstractValue& lhs, const ContextAbstractValue& rhs)
{
    if (lhs.IsTop() || rhs.IsTop() || lhs.kind != rhs.kind) {
        return ContextAbstractValue{};
    }
    if (lhs.kind == ContextAbstractValue::Kind::BOOL && lhs.boolValue.has_value() && rhs.boolValue.has_value()) {
        return ContextAbstractValue{BoolDomain::Union(*lhs.boolValue, *rhs.boolValue)};
    }
    if (lhs.kind == ContextAbstractValue::Kind::SINT && lhs.sintValue && rhs.sintValue &&
        lhs.sintValue->Width() == rhs.sintValue->Width() &&
        lhs.sintValue->IsUnsigned() == rhs.sintValue->IsUnsigned()) {
        auto lhsKnownBits = lhs.sintKnownBits;
        auto rhsKnownBits = rhs.sintKnownBits;
        if (lhsKnownBits.has_value() || rhsKnownBits.has_value()) {
            lhsKnownBits = GetEffectiveKnownBits(
                *lhs.sintValue, lhs.exactSIntValues, lhs.sintCongruence, lhsKnownBits);
            rhsKnownBits = GetEffectiveKnownBits(
                *rhs.sintValue, rhs.exactSIntValues, rhs.sintCongruence, rhsKnownBits);
        }
        auto joinedDomain = SIntDomain::Unions(*lhs.sintValue, *rhs.sintValue);
        SIntRange lhsRange{*lhs.sintValue, lhs.exactSIntValues, lhs.sintCongruence,
            lhs.sintKnownBits, lhs.excludedSIntValues, lhs.sintIntervalFragments};
        SIntRange rhsRange{*rhs.sintValue, rhs.exactSIntValues, rhs.sintCongruence,
            rhs.sintKnownBits, rhs.excludedSIntValues, rhs.sintIntervalFragments};
        return ContextAbstractValue{joinedDomain,
            MergeExactIntSets(lhs.exactSIntValues, rhs.exactSIntValues),
            MergeSIntCongruencesForUnion(*lhs.sintValue, lhs.exactSIntValues, lhs.sintCongruence,
                *rhs.sintValue, rhs.exactSIntValues, rhs.sintCongruence),
            MergeSIntKnownBitsForUnion(
                *lhs.sintValue, lhsKnownBits, *rhs.sintValue, rhsKnownBits),
            MergeExcludedSIntValuesForUnion(*lhs.sintValue, lhs.excludedSIntValues,
                *rhs.sintValue, rhs.excludedSIntValues, joinedDomain),
            MergeSIntIntervalFragmentsForUnion(lhsRange, rhsRange, joinedDomain)};
    }
    if (lhs.kind == ContextAbstractValue::Kind::CLASS) {
        return lhs.classValue == rhs.classValue ? ContextAbstractValue{lhs.classValue} : ContextAbstractValue{};
    }
    if (lhs.kind == ContextAbstractValue::Kind::LAMBDA) {
        if (lhs.lambdaValue != rhs.lambdaValue ||
            lhs.lambdaCapturedValues.size() != rhs.lambdaCapturedValues.size()) {
            return ContextAbstractValue{};
        }
        std::vector<ContextAbstractValue> captures;
        captures.reserve(lhs.lambdaCapturedValues.size());
        for (size_t i = 0; i < lhs.lambdaCapturedValues.size(); ++i) {
            captures.emplace_back(
                JoinContextValues(lhs.lambdaCapturedValues[i], rhs.lambdaCapturedValues[i]));
        }
        return ContextAbstractValue{lhs.lambdaValue, std::move(captures)};
    }
    if (lhs.kind == ContextAbstractValue::Kind::OBJECT &&
        lhs.objectIdentity != 0 && lhs.objectIdentity == rhs.objectIdentity &&
        lhs.objectFields.size() == rhs.objectFields.size()) {
        std::vector<ContextAbstractValue> fields;
        fields.reserve(lhs.objectFields.size());
        for (size_t i = 0; i < lhs.objectFields.size(); ++i) {
            fields.emplace_back(JoinContextValues(lhs.objectFields[i], rhs.objectFields[i]));
        }
        auto exactClass = lhs.classValue == rhs.classValue ? lhs.classValue : nullptr;
        return ContextAbstractValue{exactClass, std::move(fields), lhs.objectIdentity};
    }
    return ContextAbstractValue{};
}

ClassType* RangeAnalysis::ResolveExactClassForValue(Value* value) const
{
    // Lowering can create long, acyclic Load/TypeCast forwarding chains. The
    // visited set still terminates cycles; keep a generous stack-depth guard.
    constexpr unsigned MAX_CLASS_DEF_DEPTH = 64;
    std::unordered_set<Value*> visited;
    std::function<ClassType*(Value*, unsigned)> resolve = [&](Value* current, unsigned depth) -> ClassType* {
        if (current == nullptr || depth >= MAX_CLASS_DEF_DEPTH || !visited.emplace(current).second) {
            return nullptr;
        }
        if (current->IsParameter()) {
            auto parameter = StaticCast<Parameter*>(current);
            if (!isContextAnalysis || parameter->GetOwnerFunc() != func) {
                return nullptr;
            }
            const auto& params = func->GetParams();
            auto found = std::find(params.begin(), params.end(), parameter);
            if (found == params.end()) {
                return nullptr;
            }
            auto index = static_cast<size_t>(std::distance(params.begin(), found));
            if (index >= contextArguments.size()) {
                return nullptr;
            }
            const auto& contextValue = contextArguments[index];
            return contextValue.kind == ContextAbstractValue::Kind::CLASS ||
                    contextValue.kind == ContextAbstractValue::Kind::OBJECT
                ? contextValue.classValue
                : nullptr;
        }
        if (!current->IsLocalVar()) {
            return nullptr;
        }
        auto expression = StaticCast<LocalVar*>(current)->GetExpr();
        if (expression == nullptr) {
            return nullptr;
        }
        switch (expression->GetExprKind()) {
            case ExprKind::ALLOCATE: {
                auto allocatedType = StaticCast<Allocate*>(expression)->GetType();
                return allocatedType != nullptr && allocatedType->IsClass()
                    ? StaticCast<ClassType*>(allocatedType)
                    : nullptr;
            }
            case ExprKind::ALLOCATE_WITH_EXCEPTION: {
                auto allocatedType = StaticCast<AllocateWithException*>(expression)->GetType();
                return allocatedType != nullptr && allocatedType->IsClass()
                    ? StaticCast<ClassType*>(allocatedType)
                    : nullptr;
            }
            case ExprKind::TYPECAST:
                return resolve(StaticCast<TypeCast*>(expression)->GetSourceValue(), depth + 1);
            case ExprKind::LOAD:
                return resolve(StaticCast<Load*>(expression)->GetLocation(), depth + 1);
            default:
                return nullptr;
        }
    };
    return resolve(value, 0);
}

const Lambda* RangeAnalysis::ResolveContextLambdaForValue(Value* value) const
{
    constexpr unsigned MAX_LAMBDA_DEF_DEPTH = 8;
    std::unordered_set<Value*> visited;
    std::function<const Lambda*(Value*, unsigned)> resolve =
        [&](Value* current, unsigned depth) -> const Lambda* {
        if (current == nullptr || depth >= MAX_LAMBDA_DEF_DEPTH ||
            !visited.emplace(current).second) {
            return nullptr;
        }
        if (current->IsParameter()) {
            auto parameter = StaticCast<Parameter*>(current);
            if (!isContextAnalysis || parameter->GetOwnerFunc() != func) {
                return nullptr;
            }
            const auto& params = func->GetParams();
            auto found = std::find(params.begin(), params.end(), parameter);
            if (found == params.end()) {
                return nullptr;
            }
            auto index = static_cast<size_t>(std::distance(params.begin(), found));
            if (index >= contextArguments.size()) {
                return nullptr;
            }
            const auto& contextValue = contextArguments[index];
            return contextValue.kind == ContextAbstractValue::Kind::LAMBDA
                ? contextValue.lambdaValue
                : nullptr;
        }
        if (!current->IsLocalVar()) {
            return nullptr;
        }
        auto expression = StaticCast<LocalVar*>(current)->GetExpr();
        if (expression == nullptr) {
            return nullptr;
        }
        if (expression->GetExprKind() == ExprKind::LAMBDA) {
            return StaticCast<const Lambda*>(expression);
        }
        if (expression->GetExprKind() == ExprKind::TYPECAST) {
            return resolve(StaticCast<TypeCast*>(expression)->GetSourceValue(), depth + 1);
        }
        return nullptr;
    };
    return resolve(value, 0);
}

std::optional<std::vector<ClassType*>> RangeAnalysis::ResolveFiniteClassSetForValue(Value* value) const
{
    ScopedContextAnalysisStage stage("finite-class-resolution");
    if (IsRangeAnalysisPerfCollectionEnabled()) {
        ++rangeAnalysisPerfStats.finiteClassResolveCalls;
    }
    ScopedRangeAnalysisPerfTimer timer(&rangeAnalysisPerfStats.finiteClassResolveNanos);
    if (IsContextAnalysisBudgetExhausted()) {
        RecordFiniteClassFailure("context-budget");
        return std::nullopt;
    }
    // A long but acyclic chain of local Ref/Load/Store forwarding is common
    // after lowering source-level receiver variables. Total work remains
    // bounded by MAX_CLASS_RESOLUTION_NODES and memoized by CHIR Value.
    constexpr unsigned MAX_CLASS_DEF_DEPTH = 64;
    constexpr size_t MAX_CLASS_TARGETS = 16;
    constexpr size_t MAX_CLASS_RESOLUTION_NODES = 512;
    using ClassSet = std::optional<std::vector<ClassType*>>;
    std::function<const ContextAbstractValue*(Value*, unsigned)> resolveContextValue =
        [&](Value* current, unsigned depth) -> const ContextAbstractValue* {
        if (!isContextAnalysis || current == nullptr || depth >= MAX_CLASS_DEF_DEPTH) {
            return nullptr;
        }
        if (current->IsParameter()) {
            auto parameter = StaticCast<Parameter*>(current);
            if (parameter->GetOwnerFunc() != func) {
                return nullptr;
            }
            const auto& params = func->GetParams();
            auto found = std::find(params.begin(), params.end(), parameter);
            if (found == params.end()) {
                return nullptr;
            }
            auto index = static_cast<size_t>(std::distance(params.begin(), found));
            return index < contextArguments.size() ? &contextArguments[index] : nullptr;
        }
        if (!current->IsLocalVar()) {
            return nullptr;
        }
        auto expression = StaticCast<LocalVar*>(current)->GetExpr();
        if (expression == nullptr) {
            return nullptr;
        }
        if (expression->GetExprKind() == ExprKind::TYPECAST) {
            return resolveContextValue(
                StaticCast<TypeCast*>(expression)->GetSourceValue(), depth + 1);
        }
        if (expression->GetExprKind() == ExprKind::LOAD) {
            return resolveContextValue(
                StaticCast<Load*>(expression)->GetLocation(), depth + 1);
        }
        const ContextAbstractValue* aggregate = nullptr;
        std::vector<uint64_t> path;
        if (expression->GetExprKind() == ExprKind::GET_ELEMENT_REF) {
            auto element = StaticCast<GetElementRef*>(expression);
            aggregate = resolveContextValue(element->GetLocation(), depth + 1);
            path = element->GetPath();
        } else if (expression->GetExprKind() == ExprKind::FIELD) {
            auto field = StaticCast<Field*>(expression);
            aggregate = resolveContextValue(field->GetBase(), depth + 1);
            path = field->GetPath();
        } else {
            return nullptr;
        }
        for (auto index : path) {
            if (aggregate == nullptr ||
                aggregate->kind != ContextAbstractValue::Kind::OBJECT ||
                index >= aggregate->objectFields.size()) {
                return nullptr;
            }
            aggregate = &aggregate->objectFields[index];
        }
        return aggregate;
    };
    std::unordered_set<Value*> active;
    std::unordered_map<Value*, ClassSet> memo;
    size_t resolvedNodes = 0;
    bool budgetExceeded = false;
    std::function<ClassSet(Value*, unsigned)> resolve =
        [&](Value* current, unsigned depth) -> ClassSet {
        if (IsContextAnalysisBudgetExhausted()) {
            budgetExceeded = true;
            return std::nullopt;
        }
        if (IsRangeAnalysisPerfCollectionEnabled()) {
            rangeAnalysisPerfStats.maxFiniteClassDepth =
                std::max(rangeAnalysisPerfStats.maxFiniteClassDepth, static_cast<size_t>(depth));
        }
        if (current == nullptr || depth >= MAX_CLASS_DEF_DEPTH) {
            return std::nullopt;
        }
        if (auto found = memo.find(current); found != memo.end()) {
            if (IsRangeAnalysisPerfCollectionEnabled()) {
                ++rangeAnalysisPerfStats.finiteClassMemoHits;
            }
            return found->second;
        }
        if (resolvedNodes >= MAX_CLASS_RESOLUTION_NODES) {
            if (!budgetExceeded && IsRangeAnalysisPerfCollectionEnabled()) {
                ++rangeAnalysisPerfStats.finiteClassBudgetRejects;
            }
            budgetExceeded = true;
            return std::nullopt;
        }
        ++resolvedNodes;
        if (IsRangeAnalysisPerfCollectionEnabled()) {
            ++rangeAnalysisPerfStats.finiteClassResolveNodes;
        }
        if (!active.emplace(current).second) {
            return std::nullopt;
        }
        const auto finish = [&active, &memo, &budgetExceeded, current](ClassSet result) {
            active.erase(current);
            if (!budgetExceeded) {
                memo.emplace(current, result);
            }
            return result;
        };
        if (auto contextValue = resolveContextValue(current, 0);
            contextValue != nullptr &&
            (contextValue->kind == ContextAbstractValue::Kind::CLASS ||
                contextValue->kind == ContextAbstractValue::Kind::OBJECT) &&
            contextValue->classValue != nullptr) {
            return finish(std::vector<ClassType*>{contextValue->classValue});
        }
        if (auto exactClass = ResolveExactClassForValue(current); exactClass != nullptr) {
            return finish(std::vector<ClassType*>{exactClass});
        }
        if (!current->IsLocalVar()) {
            return finish(std::nullopt);
        }
        auto expression = StaticCast<LocalVar*>(current)->GetExpr();
        if (expression == nullptr) {
            return finish(std::nullopt);
        }
        if (expression->GetExprKind() == ExprKind::TYPECAST) {
            auto result = resolve(StaticCast<TypeCast*>(expression)->GetSourceValue(), depth + 1);
            return finish(std::move(result));
        }
        if (expression->GetExprKind() == ExprKind::LOAD) {
            auto result = resolve(StaticCast<Load*>(expression)->GetLocation(), depth + 1);
            return finish(std::move(result));
        }
        if (expression->GetExprKind() == ExprKind::APPLY) {
            auto callee = DynamicCast<Function*>(StaticCast<Apply*>(expression)->GetCallee());
            if (callee == nullptr || callee->GetBody() == nullptr || !callee->HasReturnValue()) {
                return finish(std::nullopt);
            }
            auto result = resolve(callee->GetReturnValue(), depth + 1);
            return finish(std::move(result));
        }
        if (expression->GetExprKind() == ExprKind::APPLY_WITH_EXCEPTION) {
            auto callee =
                DynamicCast<Function*>(StaticCast<ApplyWithException*>(expression)->GetCallee());
            if (callee == nullptr || callee->GetBody() == nullptr || !callee->HasReturnValue()) {
                return finish(std::nullopt);
            }
            auto result = resolve(callee->GetReturnValue(), depth + 1);
            return finish(std::move(result));
        }

        auto type = current->GetType();
        if (type == nullptr || !type->IsRef() || current->IsGlobal() ||
            current->TestAttr(Attribute::STATIC)) {
            return finish(std::nullopt);
        }
        std::vector<ClassType*> classes;
        bool sawStore = false;
        for (auto user : current->GetUsers()) {
            if (user->GetExprKind() == ExprKind::DEBUGEXPR) {
                continue;
            }
            if (user->GetExprKind() == ExprKind::LOAD &&
                StaticCast<const Load*>(user)->GetLocation() == current) {
                continue;
            }
            if (user->GetExprKind() != ExprKind::STORE ||
                StaticCast<const Store*>(user)->GetLocation() != current) {
                return finish(std::nullopt);
            }
            sawStore = true;
            auto storedClasses = resolve(StaticCast<const Store*>(user)->GetValue(), depth + 1);
            if (!storedClasses.has_value()) {
                return finish(std::nullopt);
            }
            for (auto storedClass : *storedClasses) {
                if (std::find(classes.begin(), classes.end(), storedClass) != classes.end()) {
                    continue;
                }
                if (classes.size() >= MAX_CLASS_TARGETS) {
                    return finish(std::nullopt);
                }
                classes.emplace_back(storedClass);
            }
        }
        return finish(sawStore && !classes.empty()
            ? ClassSet{std::move(classes)}
            : std::nullopt);
    };
    auto result = resolve(value, 0);
    if (result.has_value()) {
        rangeAnalysisPerfStats.maxFiniteClassCount =
            std::max(rangeAnalysisPerfStats.maxFiniteClassCount, result->size());
    } else {
        RecordFiniteClassFailure(
            budgetExceeded ? "resolution-budget" : "open-or-unsupported-definition");
    }
    return result;
}

bool RangeAnalysis::HandleFiniteInvokeTargets(
    RangeDomain& state, const Expression* callExpression, const Invoke* invoke)
{
    ScopedOverrideTraceLocation traceLocation(func, callExpression);
    ScopedContextAnalysisStage stage("finite-invoke-target-resolution");
    if (IsRangeAnalysisPerfCollectionEnabled()) {
        ++rangeAnalysisPerfStats.finiteInvokeCalls;
    }
    ScopedRangeAnalysisPerfTimer timer(&rangeAnalysisPerfStats.finiteInvokeNanos);
    if (IsContextAnalysisBudgetExhausted()) {
        RecordContextTopSource("invoke-budget-before-resolution");
        return false;
    }
    auto classes = ResolveFiniteClassSetForValue(invoke == nullptr ? nullptr : invoke->GetObject());
    if (!classes.has_value() || classes->empty()) {
        RecordContextTopSource("invoke-open-receiver-set");
        return false;
    }
    std::vector<Function*> targets;
    targets.reserve(classes->size());
    for (auto exactClass : *classes) {
        Function* target = nullptr;
        {
            if (IsRangeAnalysisPerfCollectionEnabled()) {
                ++rangeAnalysisPerfStats.dispatchTargetResolveCalls;
            }
            ScopedRangeAnalysisPerfTimer targetTimer(
                &rangeAnalysisPerfStats.dispatchTargetResolveNanos);
            target = ResolveExactInvokeTarget(invoke, exactClass, builder);
        }
        if (target == nullptr) {
            RecordContextTopSource("invoke-vtable-target");
            return false;
        }
        if (std::find(targets.begin(), targets.end(), target) == targets.end()) {
            targets.emplace_back(target);
        }
    }
    if (targets.empty()) {
        return false;
    }
    rangeAnalysisPerfStats.maxFiniteTargetCount =
        std::max(rangeAnalysisPerfStats.maxFiniteTargetCount, targets.size());
    if (IsOverrideTraceEnabled() && overrideTraceInvokeLogCount < 256) {
        ++overrideTraceInvokeLogCount;
        const auto& location = callExpression->GetDebugLocation();
        std::cerr << "[RangeAnalysisOverrideInvoke] caller="
                  << (func == nullptr ? "<none>" : func->GetIdentifier())
                  << " line=" << location.GetBeginPos().line
                  << " receiver-complete=1 classes=" << classes->size()
                  << " slot=" << invoke->GetVirtualMethodOffset(&builder)
                  << " targets=" << targets.size();
        for (auto target : targets) {
            std::cerr << ' ' << target->GetIdentifier();
        }
        std::cerr << '\n';
    }

    return MergeFiniteDispatchTargets(
        state, callExpression, invoke->GetArgs(), invoke->GetResult(), targets, classes->size());
}

bool RangeAnalysis::HandleFiniteInvokeTargets(
    RangeDomain& state, const Expression* callExpression, const InvokeWithException* invoke)
{
    ScopedOverrideTraceLocation traceLocation(func, callExpression);
    ScopedContextAnalysisStage stage("finite-exception-invoke-target-resolution");
    if (IsRangeAnalysisPerfCollectionEnabled()) {
        ++rangeAnalysisPerfStats.finiteInvokeCalls;
    }
    ScopedRangeAnalysisPerfTimer timer(&rangeAnalysisPerfStats.finiteInvokeNanos);
    if (IsContextAnalysisBudgetExhausted()) {
        RecordContextTopSource("invoke-budget-before-resolution");
        return false;
    }
    auto classes = ResolveFiniteClassSetForValue(invoke == nullptr ? nullptr : invoke->GetObject());
    if (!classes.has_value() || classes->empty()) {
        RecordContextTopSource("invoke-open-receiver-set");
        return false;
    }
    std::vector<Function*> targets;
    targets.reserve(classes->size());
    for (auto exactClass : *classes) {
        Function* target = nullptr;
        {
            if (IsRangeAnalysisPerfCollectionEnabled()) {
                ++rangeAnalysisPerfStats.dispatchTargetResolveCalls;
            }
            ScopedRangeAnalysisPerfTimer targetTimer(
                &rangeAnalysisPerfStats.dispatchTargetResolveNanos);
            target = ResolveExactInvokeTarget(invoke, exactClass, builder);
        }
        if (target == nullptr) {
            RecordContextTopSource("invoke-vtable-target");
            return false;
        }
        if (std::find(targets.begin(), targets.end(), target) == targets.end()) {
            targets.emplace_back(target);
        }
    }
    if (targets.empty()) {
        return false;
    }
    rangeAnalysisPerfStats.maxFiniteTargetCount =
        std::max(rangeAnalysisPerfStats.maxFiniteTargetCount, targets.size());
    if (IsOverrideTraceEnabled() && overrideTraceInvokeLogCount < 256) {
        ++overrideTraceInvokeLogCount;
        const auto& location = callExpression->GetDebugLocation();
        std::cerr << "[RangeAnalysisOverrideInvoke] caller="
                  << (func == nullptr ? "<none>" : func->GetIdentifier())
                  << " line=" << location.GetBeginPos().line
                  << " receiver-complete=1 classes=" << classes->size()
                  << " slot=" << invoke->GetVirtualMethodOffset(&builder)
                  << " targets=" << targets.size() << " exceptional=1";
        for (auto target : targets) {
            std::cerr << ' ' << target->GetIdentifier();
        }
        std::cerr << '\n';
    }

    return MergeFiniteDispatchTargets(
        state, callExpression, invoke->GetArgs(), invoke->GetResult(), targets, classes->size());
}

bool RangeAnalysis::MergeFiniteDispatchTargets(RangeDomain& state, const Expression* callExpression,
    const std::vector<Value*>& args, Value* result, const std::vector<Function*>& targets, size_t classCount)
{
    ScopedContextAnalysisStage stage("finite-dispatch-merge");
    if (IsRangeAnalysisPerfCollectionEnabled()) {
        ++rangeAnalysisPerfStats.finiteDispatchMergeCalls;
    }
    ScopedRangeAnalysisPerfTimer timer(&rangeAnalysisPerfStats.finiteDispatchMergeNanos);
    if (IsContextAnalysisBudgetExhausted()) {
        RecordContextTopSource("dispatch-budget-before-merge");
        return false;
    }
    if (targets.size() == 1) {
        auto target = targets.front();
        HandleContextSensitiveCall(state, callExpression, target, args, result);
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            auto resultRange = result == nullptr ? nullptr : state.CheckAbstractValue(result);
            std::cerr << "[RangeAnalysisInvokeTargetSummary] target=" << target->GetIdentifier()
                      << " value=" << (resultRange == nullptr ? "Top" : resultRange->ToString()) << '\n';
            const auto& location = callExpression->GetDebugLocation();
            std::cerr << "[RangeAnalysisInvokeTargets] result=finite classes=" << classCount
                      << " targets=1"
                      << " value=" << (resultRange == nullptr ? "Top" : resultRange->ToString())
                      << " line=" << location.GetBeginPos().line << '\n';
        }
        return true;
    }
    if (IsRangeAnalysisPerfCollectionEnabled()) {
        ++rangeAnalysisPerfStats.finiteDispatchStateCopies;
    }
    auto inputState = state;
    bool hasMergedState = false;
    for (auto target : targets) {
        if (IsContextAnalysisBudgetExhausted()) {
            RecordContextTopSource("dispatch-budget-during-merge");
            return false;
        }
        if (IsRangeAnalysisPerfCollectionEnabled()) {
            ++rangeAnalysisPerfStats.finiteDispatchStateCopies;
        }
        auto targetState = inputState;
        HandleContextSensitiveCall(targetState, callExpression, target, args, result);
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            auto resultRange = result == nullptr ? nullptr : targetState.CheckAbstractValue(result);
            std::cerr << "[RangeAnalysisInvokeTargetSummary] target=" << target->GetIdentifier()
                      << " value=" << (resultRange == nullptr ? "Top" : resultRange->ToString()) << '\n';
        }
        if (!hasMergedState) {
            state = std::move(targetState);
            hasMergedState = true;
        } else {
            if (IsRangeAnalysisPerfCollectionEnabled()) {
                ++rangeAnalysisPerfStats.finiteDispatchStateJoins;
            }
            state.Join(targetState);
        }
    }
    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
        auto resultRange = result == nullptr ? nullptr : state.CheckAbstractValue(result);
        const auto& location = callExpression->GetDebugLocation();
        std::cerr << "[RangeAnalysisInvokeTargets] result=finite classes=" << classCount
                  << " targets=" << targets.size()
                  << " value=" << (resultRange == nullptr ? "Top" : resultRange->ToString())
                  << " line=" << location.GetBeginPos().line << '\n';
    }
    return true;
}

void RangeAnalysis::ApplyContextValue(RangeDomain& state, Value* dest, const ContextAbstractValue& value) const
{
    if (dest == nullptr || value.IsTop()) {
        return;
    }
    ApplyContextValue(state, dest, dest->GetType(), value);
}

void RangeAnalysis::ApplyContextValue(RangeDomain& state, Value* dest, Type* type, const ContextAbstractValue& value) const
{
    if (dest == nullptr || value.IsTop()) {
        return;
    }
    if (type != nullptr && type->IsRef()) {
        auto baseType = StaticCast<RefType*>(type)->GetRootBaseType();
        auto object = state.CheckAbstractObjectRefBy(dest);
        if (baseType == nullptr) {
            return;
        }
        if (object == nullptr) {
            const Expression* definition = dest->GetType() != nullptr && dest->IsLocalVar()
                ? StaticCast<LocalVar*>(dest)->GetExpr()
                : nullptr;
            object = state.GetReferencedObjAndSetToTop(dest, definition);
        }
        if (value.kind == ContextAbstractValue::Kind::OBJECT) {
            auto boundIdentity = FindBoundContextObjectIdentity(object);
            if (boundIdentity.has_value() &&
                boundIdentity.value() != value.objectIdentity) {
                SetContextObjectIdentity(object, value.objectIdentity);
                state.Update(object, /* isTop = */ true);
                state.ForgetChildren(object);
                return;
            }
            if (!SetContextObjectIdentity(object, value.objectIdentity)) {
                state.Update(object, /* isTop = */ true);
                state.ForgetChildren(object);
                return;
            }
            RecordContextObjectClass(object, value.classValue);
        }
        ApplyContextValue(state, object, baseType, value);
        return;
    }
    if ((type == nullptr || type->IsBoolean()) && value.kind == ContextAbstractValue::Kind::BOOL &&
        value.boolValue.has_value()) {
        state.Update(dest, std::make_unique<BoolRange>(*value.boolValue));
        return;
    }
    if ((type == nullptr || type->IsInteger()) && value.kind == ContextAbstractValue::Kind::SINT && value.sintValue) {
        state.Update(dest, std::make_unique<SIntRange>(
            *value.sintValue, value.exactSIntValues, value.sintCongruence, value.sintKnownBits,
            value.excludedSIntValues, value.sintIntervalFragments));
        return;
    }
    if (type != nullptr && type->IsRawArray() && value.kind == ContextAbstractValue::Kind::OBJECT) {
        const bool traceContextArray = std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr;
        if (traceContextArray) {
            std::cerr << "[RangeAnalysisContextRawArray] phase=begin elements="
                      << value.objectFields.size() << " identity=" << value.objectIdentity << '\n';
        }
        if (!SetContextObjectIdentity(dest, value.objectIdentity)) {
            state.Update(dest, /* isTop = */ true);
            state.ForgetChildren(dest);
            return;
        }
        constexpr size_t MAX_CONTEXT_RAW_ARRAY_ELEMENTS = 64;
        if (value.objectFields.size() > MAX_CONTEXT_RAW_ARRAY_ELEMENTS) {
            state.Update(dest, /* isTop = */ true);
            state.ForgetChildren(dest);
            return;
        }
        auto elementType = StaticCast<RawArrayType*>(type)->GetElementType();
        auto children = state.GetChildren(dest);
        if (traceContextArray) {
            std::cerr << "[RangeAnalysisContextRawArray] phase=children-before count="
                      << children.size() << " element-type="
                      << (elementType == nullptr ? "<null>" : elementType->ToString()) << '\n';
        }
        if (children.size() != value.objectFields.size()) {
            state.ForgetChildren(dest);
            const auto setChildTop = [&state, elementType](AbstractObject* child, size_t) {
                state.SetToTopOrTopRef(child, elementType != nullptr && elementType->IsRef());
            };
            state.EnsureChildren(dest, value.objectFields.size(), setChildTop);
            children = state.GetChildren(dest);
        }
        if (traceContextArray) {
            std::cerr << "[RangeAnalysisContextRawArray] phase=children-ready count="
                      << children.size() << '\n';
        }
        if (children.size() != value.objectFields.size()) {
            state.Update(dest, /* isTop = */ true);
            state.ForgetChildren(dest);
            return;
        }
        for (size_t i = 0; i < children.size(); ++i) {
            if (traceContextArray) {
                std::cerr << "[RangeAnalysisContextRawArray] phase=element index=" << i << '\n';
            }
            ApplyContextValue(state, children[i], elementType, value.objectFields[i]);
        }
        if (traceContextArray) {
            std::cerr << "[RangeAnalysisContextRawArray] phase=end\n";
        }
        return;
    }
    if (type != nullptr && value.kind == ContextAbstractValue::Kind::OBJECT &&
        (type->IsClass() || type->IsStruct() || type->IsTuple())) {
        const bool traceContextObject = std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr;
        if (traceContextObject) {
            std::cerr << "[RangeAnalysisContextObject] phase=begin type=" << type->ToString()
                      << " fields=" << value.objectFields.size()
                      << " identity=" << value.objectIdentity << '\n';
        }
        const bool isDirectValueAggregate = dest->GetType() != nullptr &&
            !dest->GetType()->IsRef() && (type->IsStruct() || type->IsTuple());
        if (!isDirectValueAggregate && !SetContextObjectIdentity(dest, value.objectIdentity)) {
            state.Update(dest, /* isTop = */ true);
            state.ForgetChildren(dest);
            return;
        }
        if (!isDirectValueAggregate) {
            RecordContextObjectClass(dest, value.classValue);
        }
        std::vector<Type*> memberTypes;
        if (type->IsClass() || type->IsStruct()) {
            memberTypes = StaticCast<CustomType*>(type)->GetInstantiatedMemberTys(builder);
        } else {
            memberTypes = StaticCast<TupleType*>(type)->GetElementTypes();
        }
        auto oldChildren = state.GetChildren(dest);
        std::vector<AbstractObject*> referencedObjects(oldChildren.size(), nullptr);
        for (size_t i = 0; i < oldChildren.size() && i < memberTypes.size(); ++i) {
            if (memberTypes[i] != nullptr && memberTypes[i]->IsRef()) {
                referencedObjects[i] = state.CheckAbstractObjectRefBy(oldChildren[i]);
            }
        }
        ResetObjectChildrenToTop(state, dest, type);
        auto children = state.GetChildren(dest);
        auto count = std::min(children.size(), value.objectFields.size());
        for (size_t i = 0; i < count; ++i) {
            auto memberType = i < memberTypes.size() ? memberTypes[i] : nullptr;
            if (traceContextObject) {
                std::cerr << "[RangeAnalysisContextObject] phase=field index=" << i
                          << " member-type="
                          << (memberType == nullptr ? "<null>" : memberType->ToString())
                          << " value-kind="
                          << static_cast<unsigned>(value.objectFields[i].kind)
                          << " value-identity=" << value.objectFields[i].objectIdentity << '\n';
            }
            if (memberType != nullptr && memberType->IsRef() && i < referencedObjects.size() &&
                referencedObjects[i] != nullptr &&
                value.objectFields[i].kind == ContextAbstractValue::Kind::OBJECT &&
                GetContextObjectIdentity(referencedObjects[i]) == value.objectFields[i].objectIdentity) {
                state.SetRefToObject(children[i], referencedObjects[i]);
            }
            ApplyContextValue(state, children[i], memberType, value.objectFields[i]);
        }
        if (traceContextObject) {
            std::cerr << "[RangeAnalysisContextObject] phase=end type=" << type->ToString()
                      << " children=" << children.size() << '\n';
        }
    }
}

bool TryHandleStructArrayLiteralConstructor(RangeDomain& state, const Apply* apply);
bool TryHandleArrayLiteralIndexApply(RangeDomain& state, const Apply* apply);
bool TryHandleStructArrayLiteralMutation(RangeDomain& state, const Apply* apply);
bool IsStructArrayValue(Value* value);
void PropagateArrayLiteralInfoOnLoad(const Load* load);
void PropagateArrayLiteralInfoOnStore(const Store* store);
std::optional<StructArrayLiteralInfo> LookupStructArrayLiteralInfo(Value* arrayValue);
std::optional<StructArrayLiteralInfo> LookupStructArrayLiteralInfo(
    const RangeDomain& state, Value* arrayValue);
void InvalidateRawArrayLiteral(RangeDomain& state, Value* rawArray);
void InvalidateStructArrayLiteral(Value* arrayValue);
void ForgetReferenceArgument(RangeDomain& state, Value* arg);

void RangeAnalysis::HavocCallEffects(
    RangeDomain& state, const std::vector<Value*>& args, Value* result, const Lambda* lambda)
{
    if (result != nullptr) {
        state.SetToTopOrTopRef(result, result->GetType() != nullptr && result->GetType()->IsRef());
    }
    for (auto arg : args) {
        ForgetReferenceArgument(state, arg);
    }
    if (lambda != nullptr) {
        ValueAnalysis<RangeValueDomain>::HandleVarStateCapturedByLambda(state, lambda);
    }

    auto package = builder.GetCurPackage();
    if (package == nullptr) {
        state.ClearState();
        return;
    }
    for (auto global : package->GetGlobalVars()) {
        if (global == nullptr || global->TestAttr(Attribute::READONLY) || global->GetType() == nullptr ||
            !global->GetType()->IsRef()) {
            continue;
        }
        auto baseType = GetRefRootBaseType(global->GetType());
        if (baseType != nullptr && (baseType->IsBoolean() || baseType->IsInteger())) {
            auto object = EnsureMutableGlobalValueInitialized(state, global);
            if (object == nullptr) {
                state.ClearState();
            } else {
                state.Update(object, /* isTop = */ true);
            }
            continue;
        }
        ForgetReferenceArgument(state, global);
    }
}

void RangeAnalysis::HandleApplyExpr(RangeDomain& state, const Apply* apply, Value* refObj)
{
    (void)refObj;
    if (TryHandlePureSpawnFutureResult(state, apply)) {
        return;
    }
    if (TryHandleStructArrayLiteralConstructor(state, apply) ||
        TryHandleStructArrayLiteralMutation(state, apply) ||
        TryHandleArrayLiteralIndexApply(state, apply)) {
        RecordFullyModeledCallContext(analysisContextKey, apply);
        if (boundedLoopCallContextRecorder != nullptr && boundedLoopEvaluationOwner == this) {
            (void)(*boundedLoopCallContextRecorder)[apply];
        }
        return;
    }
    HandleContextSensitiveCall(state, apply, apply->GetCallee(), apply->GetArgs(), apply->GetResult());
}

bool RangeAnalysis::IsPureSpawnLambda(const Lambda* lambda) const
{
    if (lambda == nullptr || lambda->GetBody() == nullptr || !lambda->GetParams().empty()) {
        return false;
    }
    const auto captures = lambda->GetCapturedVariables();
    if (!std::all_of(captures.begin(), captures.end(), [](Value* captured) {
            if (captured == nullptr || captured->GetType() == nullptr ||
                !captured->TestAttr(Attribute::READONLY)) {
                return false;
            }
            auto type = captured->GetType();
            if (type->IsBoolean() || type->IsInteger()) {
                return true;
            }
            auto baseType = GetRefRootBaseType(type);
            return baseType != nullptr && (baseType->IsBoolean() || baseType->IsInteger());
        })) {
        return false;
    }
    auto globals = CollectTrackedMutableGlobals(lambda, builder.GetCurPackage());
    if (!globals.complete || !globals.values.empty()) {
        return false;
    }
    for (auto block : lambda->GetBody()->GetBlocks()) {
        for (auto expression : block->GetExpressions()) {
            if (expression->GetExprKind() == ExprKind::APPLY ||
                expression->GetExprKind() == ExprKind::APPLY_WITH_EXCEPTION) {
                Value* callee = expression->GetExprKind() == ExprKind::APPLY
                    ? StaticCast<const Apply*>(expression)->GetCallee()
                    : StaticCast<const ApplyWithException*>(expression)->GetCallee();
                auto function = DynamicCast<Function*>(callee);
                if (function != nullptr &&
                    (function->GetBody() != nullptr ||
                        function->TestAttr(Attribute::NO_SIDE_EFFECT))) {
                    continue;
                }
                return false;
            }
            switch (expression->GetExprMajorKind()) {
                case ExprMajorKind::UNARY_EXPR:
                case ExprMajorKind::BINARY_EXPR:
                    continue;
                case ExprMajorKind::MEMORY_EXPR: {
                    switch (expression->GetExprKind()) {
                        case ExprKind::ALLOCATE:
                            continue;
                        case ExprKind::LOAD: {
                            auto location = StaticCast<const Load*>(expression)->GetLocation();
                            if (location != nullptr && !location->IsGlobal() &&
                                !location->TestAttr(Attribute::STATIC)) {
                                continue;
                            }
                            return false;
                        }
                        case ExprKind::STORE: {
                            auto location = StaticCast<const Store*>(expression)->GetLocation();
                            if (location != nullptr && !location->IsGlobal() &&
                                !location->TestAttr(Attribute::STATIC)) {
                                continue;
                            }
                            return false;
                        }
                        default:
                            return false;
                    }
                }
                case ExprMajorKind::STRUCTURED_CTRL_FLOW_EXPR:
                    return false;
                case ExprMajorKind::OTHERS:
                    switch (expression->GetExprKind()) {
                        case ExprKind::CONSTANT:
                        case ExprKind::DEBUGEXPR:
                        case ExprKind::TUPLE:
                        case ExprKind::TYPECAST:
                        case ExprKind::INSTANCEOF:
                            continue;
                        default:
                            return false;
                    }
                case ExprMajorKind::TERMINATOR:
                    switch (expression->GetExprKind()) {
                        case ExprKind::GOTO:
                        case ExprKind::BRANCH:
                        case ExprKind::MULTIBRANCH:
                        case ExprKind::EXIT:
                            continue;
                        default:
                            return false;
                    }
                default:
                    return false;
            }
        }
    }
    return true;
}

bool RangeAnalysis::IsUniqueSpawnValueProjection(
    Value* future, Value* calleeValue, Type* resultType, Type* parentType) const
{
    auto parentClass = parentType == nullptr ? nullptr : DynamicCast<ClassType*>(parentType->StripAllRefs());
    auto futureClass = future == nullptr ? nullptr : DynamicCast<ClassType*>(future->GetType()->StripAllRefs());
    auto callee = DynamicCast<Function*>(calleeValue);
    if (parentClass == nullptr || futureClass == nullptr || callee == nullptr || resultType == nullptr ||
        parentClass->GetClassDef() != futureClass->GetClassDef()) {
        return false;
    }
    auto genericParams = parentClass->GetClassDef()->GetGenericTypeParams();
    auto genericArgs = parentClass->GetGenericArgs();
    if (genericParams.size() != genericArgs.size()) {
        return false;
    }
    std::vector<Function*> valueProjectionMethods;
    for (auto method : parentClass->GetClassDef()->GetMethods()) {
        if (method == nullptr || method->TestAttr(Attribute::STATIC) || method->GetParams().size() != 1) {
            continue;
        }
        for (size_t i = 0; i < genericParams.size(); ++i) {
            if (method->GetReturnType() == genericParams[i] && genericArgs[i] == resultType) {
                valueProjectionMethods.emplace_back(method);
                break;
            }
        }
    }
    return valueProjectionMethods.size() == 1 && valueProjectionMethods.front() == callee;
}

bool RangeAnalysis::IsFullyModeledPureSpawnFutureInitializer(const Apply* initializer) const
{
    if (initializer == nullptr || initializer->GetArgs().size() < 2 || initializer->GetParentBlock() == nullptr) {
        return false;
    }
    auto future = initializer->GetArgs().front();
    const Lambda* lambda = nullptr;
    for (size_t i = 1; i < initializer->GetArgs().size(); ++i) {
        auto candidate = ResolveContextLambdaForValue(initializer->GetArgs()[i]);
        if (candidate == nullptr) {
            continue;
        }
        if (lambda != nullptr) {
            return false;
        }
        lambda = candidate;
    }
    if (future == nullptr || !IsPureSpawnLambda(lambda)) {
        return false;
    }

    auto block = initializer->GetParentBlock();
    const auto& expressions = block->GetExpressions();
    auto initializerPosition = std::find(expressions.begin(), expressions.end(), initializer);
    if (initializerPosition == expressions.end()) {
        return false;
    }
    const Spawn* spawn = nullptr;
    for (auto user : future->GetUsers()) {
        if (user->GetExprKind() != ExprKind::SPAWN) {
            continue;
        }
        auto candidate = StaticCast<const Spawn*>(user);
        if (candidate->IsExecuteClosure() || candidate->GetFuture() != future ||
            candidate->GetParentBlock() != block) {
            continue;
        }
        auto position = std::find(expressions.begin(), expressions.end(), candidate);
        if (position == expressions.end() || position <= initializerPosition || spawn != nullptr) {
            return false;
        }
        spawn = candidate;
    }
    if (spawn == nullptr) {
        return false;
    }
    auto spawnPosition = std::find(expressions.begin(), expressions.end(), spawn);

    for (auto user : future->GetUsers()) {
        if (user->GetExprKind() != ExprKind::APPLY || user->GetParentBlock() != block) {
            continue;
        }
        auto apply = StaticCast<const Apply*>(user);
        auto position = std::find(expressions.begin(), expressions.end(), apply);
        if (position == expressions.end()) {
            return false;
        }
        if (position < spawnPosition) {
            auto args = apply->GetArgs();
            if (!args.empty() && args.front() == future && apply != initializer) {
                return false;
            }
            continue;
        }
        if (apply != initializer && ResolvePureSpawnLambdaForApply(apply) == lambda) {
            return true;
        }
    }
    return false;
}

bool RangeAnalysis::IsFullyModeledPureSpawnFutureInitializer(const ApplyWithException* initializer) const
{
    if (initializer == nullptr || initializer->GetArgs().size() < 2 || initializer->GetSuccessBlock() == nullptr) {
        return false;
    }
    auto future = initializer->GetArgs().front();
    const Lambda* lambda = nullptr;
    for (size_t i = 1; i < initializer->GetArgs().size(); ++i) {
        auto candidate = ResolveContextLambdaForValue(initializer->GetArgs()[i]);
        if (candidate == nullptr) {
            continue;
        }
        if (lambda != nullptr) {
            return false;
        }
        lambda = candidate;
    }
    if (future == nullptr || !IsPureSpawnLambda(lambda)) {
        return false;
    }

    const SpawnWithException* spawn = nullptr;
    for (auto user : future->GetUsers()) {
        if (user->GetExprKind() != ExprKind::SPAWN_WITH_EXCEPTION) {
            continue;
        }
        auto candidate = StaticCast<const SpawnWithException*>(user);
        if (candidate->IsExecuteClosure() || candidate->GetFuture() != future ||
            candidate->GetParentBlock() != initializer->GetSuccessBlock() || spawn != nullptr) {
            continue;
        }
        spawn = candidate;
    }
    if (spawn == nullptr || spawn->GetSuccessBlock() == nullptr) {
        return false;
    }

    for (auto user : future->GetUsers()) {
        if (user->GetExprKind() != ExprKind::APPLY_WITH_EXCEPTION) {
            continue;
        }
        auto apply = StaticCast<const ApplyWithException*>(user);
        if (apply->GetSuccessBlock() == spawn->GetParentBlock()) {
            auto args = apply->GetArgs();
            if (!args.empty() && args.front() == future && apply != initializer) {
                return false;
            }
            continue;
        }
        if (apply != initializer && apply->GetParentBlock() == spawn->GetSuccessBlock() &&
            ResolvePureSpawnLambdaForApply(apply) == lambda) {
            return true;
        }
    }
    return false;
}

const Lambda* RangeAnalysis::ResolvePureSpawnLambdaForApply(const Apply* apply) const
{
    if (apply == nullptr || apply->GetResult() == nullptr || apply->GetArgs().size() != 1 ||
        (!apply->GetResult()->GetType()->IsInteger() && !apply->GetResult()->GetType()->IsBoolean())) {
        return nullptr;
    }
    auto future = apply->GetArgs().front();
    auto block = apply->GetParentBlock();
    if (future == nullptr || block == nullptr) {
        return nullptr;
    }
    const auto& expressions = block->GetExpressions();
    auto applyPosition = std::find(expressions.begin(), expressions.end(), apply);
    if (applyPosition == expressions.end()) {
        return nullptr;
    }

    const Spawn* spawn = nullptr;
    for (auto user : future->GetUsers()) {
        if (user->GetExprKind() != ExprKind::SPAWN) {
            continue;
        }
        auto candidate = StaticCast<const Spawn*>(user);
        if (candidate->IsExecuteClosure() || candidate->GetFuture() != future ||
            candidate->GetParentBlock() != block) {
            continue;
        }
        auto position = std::find(expressions.begin(), expressions.end(), candidate);
        if (position == expressions.end() || position >= applyPosition || spawn != nullptr) {
            return nullptr;
        }
        spawn = candidate;
    }
    if (spawn == nullptr) {
        return nullptr;
    }
    auto spawnPosition = std::find(expressions.begin(), expressions.end(), spawn);

    const Lambda* lambda = nullptr;
    for (auto user : future->GetUsers()) {
        if (user == apply || user->GetExprKind() != ExprKind::APPLY || user->GetParentBlock() != block) {
            continue;
        }
        auto initializer = StaticCast<const Apply*>(user);
        auto args = initializer->GetArgs();
        if (args.empty() || args.front() != future) {
            continue;
        }
        auto position = std::find(expressions.begin(), expressions.end(), initializer);
        if (position == expressions.end() || position >= spawnPosition) {
            continue;
        }
        const Lambda* candidateLambda = nullptr;
        for (size_t i = 1; i < args.size(); ++i) {
            auto candidate = ResolveContextLambdaForValue(args[i]);
            if (candidate == nullptr) {
                continue;
            }
            if (candidateLambda != nullptr) {
                return nullptr;
            }
            candidateLambda = candidate;
        }
        if (candidateLambda == nullptr || (lambda != nullptr && lambda != candidateLambda)) {
            return nullptr;
        }
        lambda = candidateLambda;
    }
    if (!IsPureSpawnLambda(lambda) || lambda->GetReturnType() != apply->GetResult()->GetType()) {
        return nullptr;
    }

    return IsUniqueSpawnValueProjection(future, apply->GetCallee(), apply->GetResult()->GetType(),
        apply->GetInstParentCustomTyOfCallee(builder))
        ? lambda
        : nullptr;
}

const Lambda* RangeAnalysis::ResolvePureSpawnLambdaForApply(const ApplyWithException* apply) const
{
    if (apply == nullptr || apply->GetResult() == nullptr || apply->GetArgs().size() != 1 ||
        (!apply->GetResult()->GetType()->IsInteger() && !apply->GetResult()->GetType()->IsBoolean())) {
        return nullptr;
    }
    auto future = apply->GetArgs().front();
    if (future == nullptr || apply->GetParentBlock() == nullptr) {
        return nullptr;
    }

    const SpawnWithException* spawn = nullptr;
    for (auto user : future->GetUsers()) {
        if (user->GetExprKind() != ExprKind::SPAWN_WITH_EXCEPTION) {
            continue;
        }
        auto candidate = StaticCast<const SpawnWithException*>(user);
        if (candidate->IsExecuteClosure() || candidate->GetFuture() != future ||
            candidate->GetSuccessBlock() != apply->GetParentBlock() || spawn != nullptr) {
            return nullptr;
        }
        spawn = candidate;
    }
    if (spawn == nullptr || spawn->GetParentBlock() == nullptr) {
        return nullptr;
    }

    const Lambda* lambda = nullptr;
    for (auto user : future->GetUsers()) {
        if (user == apply || user->GetExprKind() != ExprKind::APPLY_WITH_EXCEPTION) {
            continue;
        }
        auto initializer = StaticCast<const ApplyWithException*>(user);
        if (initializer->GetSuccessBlock() != spawn->GetParentBlock()) {
            continue;
        }
        auto args = initializer->GetArgs();
        if (args.empty() || args.front() != future) {
            continue;
        }
        const Lambda* candidateLambda = nullptr;
        for (size_t i = 1; i < args.size(); ++i) {
            auto candidate = ResolveContextLambdaForValue(args[i]);
            if (candidate == nullptr) {
                continue;
            }
            if (candidateLambda != nullptr) {
                return nullptr;
            }
            candidateLambda = candidate;
        }
        if (candidateLambda == nullptr || (lambda != nullptr && lambda != candidateLambda)) {
            return nullptr;
        }
        lambda = candidateLambda;
    }
    if (!IsPureSpawnLambda(lambda) || lambda->GetReturnType() != apply->GetResult()->GetType()) {
        return nullptr;
    }
    return IsUniqueSpawnValueProjection(future, apply->GetCallee(), apply->GetResult()->GetType(),
        apply->GetInstParentCustomTyOfCallee(builder))
        ? lambda
        : nullptr;
}

bool RangeAnalysis::ApplyPureSpawnLambdaResult(
    RangeDomain& state, const Lambda* lambda, Value* resultValue, const Expression* callExpression)
{
    if (lambda == nullptr || resultValue == nullptr || callExpression == nullptr) {
        return false;
    }
    auto candidateState = state;
    if (!HandleLambdaContextSensitiveCall(candidateState, lambda, {}, resultValue)) {
        return false;
    }
    auto result = candidateState.CheckAbstractValue(resultValue);
    if (result == nullptr) {
        return false;
    }
    state = std::move(candidateState);
    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
        const auto& location = callExpression->GetDebugLocation();
        std::cerr << "[RangeAnalysisSpawnFuture] result=" << result->ToString()
                  << " line=" << location.GetBeginPos().line << '\n';
    }
    return true;
}

bool RangeAnalysis::TryHandlePureSpawnFutureResult(RangeDomain& state, const Apply* apply)
{
    auto lambda = ResolvePureSpawnLambdaForApply(apply);
    return ApplyPureSpawnLambdaResult(state, lambda, apply == nullptr ? nullptr : apply->GetResult(), apply);
}

bool RangeAnalysis::TryHandlePureSpawnFutureResult(RangeDomain& state, const ApplyWithException* apply)
{
    auto lambda = ResolvePureSpawnLambdaForApply(apply);
    return ApplyPureSpawnLambdaResult(state, lambda, apply == nullptr ? nullptr : apply->GetResult(), apply);
}

void RangeAnalysis::HandleVarStateCapturedByLambda(RangeDomain& state, const Lambda* lambda)
{
    if (CanAnalyzeLambdaContext(lambda)) {
        return;
    }
    ValueAnalysis<RangeValueDomain>::HandleVarStateCapturedByLambda(state, lambda);
}

std::optional<Block*> RangeAnalysis::HandleApplyWithExceptionTerminator(
    RangeDomain& state, const ApplyWithException* apply, Value* refObj)
{
    (void)refObj;
    if (!TryHandlePureSpawnFutureResult(state, apply)) {
        HandleContextSensitiveCall(state, apply, apply->GetCallee(), apply->GetArgs(), apply->GetResult());
    }
    return std::nullopt;
}

std::optional<Block*> RangeAnalysis::HandleInvokeWithExceptionTerminator(
    RangeDomain& state, const InvokeWithException* invoke, Value* refObj)
{
    (void)refObj;
    if (!HandleFiniteInvokeTargets(state, invoke, invoke)) {
        RecordUnmodeledCallContext(analysisContextKey, invoke);
        HavocCallEffects(state, invoke->GetArgs(), invoke->GetResult());
    }
    return std::nullopt;
}

void RangeAnalysis::HandleContextSensitiveCall(RangeDomain& state, const Expression* callExpression,
    Value* calleeValue, const std::vector<Value*>& args, Value* result)
{
    ScopedOverrideTraceLocation traceLocation(func, callExpression);
    ScopedContextAnalysisStage stage("context-sensitive-call");
    auto lambda = IsApplyToLambda(callExpression);
    if (lambda == nullptr) {
        lambda = ResolveContextLambdaForValue(calleeValue);
    }
    if (IsContextAnalysisBudgetExhausted()) {
        RecordContextTopSource("call-budget");
        RecordUnmodeledCallContext(analysisContextKey, callExpression);
        HavocCallEffects(state, args, result, lambda);
        return;
    }
    if (lambda != nullptr) {
        if (HandleLambdaContextSensitiveCall(state, lambda, args, result)) {
            return;
        }
        if (CanAnalyzeLambdaContext(lambda)) {
            ValueAnalysis<RangeValueDomain>::HandleVarStateCapturedByLambda(state, lambda);
        }
    }
    if (calleeValue == nullptr || !calleeValue->IsFuncWithBody()) {      //intrinsic/外部函数等
        RecordContextTopSource("call-target-without-body");
        bool fullyModeled = false;
        if (callExpression != nullptr && callExpression->GetExprKind() == ExprKind::APPLY) {
            fullyModeled = IsFullyModeledPureSpawnFutureInitializer(StaticCast<const Apply*>(callExpression));
        } else if (callExpression != nullptr && callExpression->GetExprKind() == ExprKind::APPLY_WITH_EXCEPTION) {
            fullyModeled =
                IsFullyModeledPureSpawnFutureInitializer(StaticCast<const ApplyWithException*>(callExpression));
        }
        if (fullyModeled) {
            RecordFullyModeledCallContext(analysisContextKey, callExpression);
            if (boundedLoopCallContextRecorder != nullptr && boundedLoopEvaluationOwner == this) {
                (void)(*boundedLoopCallContextRecorder)[callExpression];
            }
        } else {
            RecordUnmodeledCallContext(analysisContextKey, callExpression);
        }
        HavocCallEffects(state, args, result, lambda);
        return;
    }
    auto callee = DynamicCast<Function*>(calleeValue);
    if (callee == nullptr || callee->GetBody() == nullptr || callee->GetParams().size() != args.size()) {
        RecordContextTopSource("call-signature-or-body");
        RecordUnmodeledCallContext(analysisContextKey, callExpression);
        HavocCallEffects(state, args, result, lambda);
        return;
    }

    ContextArguments contextArgs;
    contextArgs.reserve(args.size());
    for (auto arg : args) {
        auto contextValue = CaptureContextValue(state, arg, /* preserveIntervals = */ true);
        if (auto exactClass = ResolveExactClassForValue(arg); exactClass != nullptr) {
            if (contextValue.kind == ContextAbstractValue::Kind::OBJECT) {
                contextValue.classValue = exactClass;
            } else {
                contextValue = ContextAbstractValue{exactClass};
            }
        }
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisCallArgument] callee=" << callee->GetIdentifier()
                      << " type="
                      << (arg == nullptr || arg->GetType() == nullptr
                              ? "<null>"
                              : arg->GetType()->ToString())
                      << " struct-array=" << IsStructArrayValue(arg)
                      << " literal-info=" << LookupStructArrayLiteralInfo(arg).has_value()
                      << " context="
                      << contextValue.ToKeyString(arg == nullptr ? nullptr : arg->GetType())
                      << '\n';
        }
        contextArgs.emplace_back(std::move(contextValue));
    }
    std::unordered_map<AbstractObject*, size_t> firstRefArgument;
    size_t nextAliasGroup = 1;
    for (size_t i = 0; i < args.size(); ++i) {
        auto arg = args[i];
        if (arg == nullptr || arg->GetType() == nullptr || !arg->GetType()->IsRef()) {
            continue;
        }
        auto object = state.CheckAbstractObjectRefBy(arg);
        if (object == nullptr) {
            continue;
        }
        auto [found, inserted] = firstRefArgument.emplace(object, i);
        if (inserted) {
            continue;
        }
        auto& firstValue = contextArgs[found->second];
        if (firstValue.aliasGroup == 0) {
            firstValue.aliasGroup = nextAliasGroup++;
        }
        contextArgs[i].aliasGroup = firstValue.aliasGroup;
    }

    auto conservativeGlobals = CollectTrackedMutableGlobals(callee, builder.GetCurPackage());
    std::optional<std::vector<GlobalVar*>> contextualGlobals;
    const std::vector<GlobalVar*>* trackedGlobals = &conservativeGlobals.values;
    if (!conservativeGlobals.complete) {
        contextualGlobals = CollectContextMutableGlobals(callee, contextArgs);
        if (!contextualGlobals.has_value()) {
            RecordContextTopSource("call-effect-closure");
            RecordUnmodeledCallContext(analysisContextKey, callExpression);
            HavocCallEffects(state, args, result, lambda);
            return;
        }
        trackedGlobals = &contextualGlobals.value();
    }
    ContextGlobalValues contextGlobals;
    for (auto global : *trackedGlobals) {
        auto baseType = GetTrackedMutableGlobalBaseType(global);
        CJC_ASSERT(baseType != nullptr);
        EnsureMutableGlobalValueInitialized(state, global);
        contextGlobals.emplace_back(global, CaptureContextValue(state, global, /* preserveIntervals = */ true));
    }
    auto childContext = BuildContextKey(callee, contextArgs, contextGlobals);
    RecordAnalyzedCallContext(analysisContextKey, callExpression, childContext);
    if (boundedLoopCallContextRecorder != nullptr && boundedLoopEvaluationOwner == this &&
        callExpression != nullptr) {
        (*boundedLoopCallContextRecorder)[callExpression].keys.emplace(childContext);
    }

    std::vector<std::optional<ContextAbstractValue>> refArgValues;
    ContextGlobalValues summarizedGlobals = contextGlobals;
    auto summary =
        AnalyzeCalleeWithContext(callee, contextArgs, refArgValues, summarizedGlobals, &childContext);
    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
        for (size_t i = 0; i < args.size(); ++i) {
            auto arg = args[i];
            if (arg == nullptr || arg->GetType() == nullptr ||
                (!arg->GetType()->IsRef() && !IsStructArrayValue(arg))) {
                continue;
            }
            std::cerr << "[RangeAnalysisCallRefSummary] callee=" << callee->GetIdentifier()
                      << " arg=" << i
                      << " input=" << contextArgs[i].ToKeyString(arg->GetType())
                      << " output=";
            if (i >= refArgValues.size() || !refArgValues[i].has_value()) {
                std::cerr << "Top";
            } else {
                std::cerr << refArgValues[i]->ToKeyString(arg->GetType());
            }
            std::cerr << '\n';
        }
    }
    for (size_t i = 0; i < args.size(); ++i) {
        auto arg = args[i];
        if (arg == nullptr || arg->GetType() == nullptr) {
            continue;
        }
        const bool isStructArray = IsStructArrayValue(arg);
        if (isStructArray) {
            if (i < refArgValues.size() && refArgValues[i].has_value()) {
                ApplyContextValue(state, arg, arg->GetType(), *refArgValues[i]);
            } else {
                auto info = LookupStructArrayLiteralInfo(state, arg);
                if (info.has_value()) {
                    InvalidateRawArrayLiteral(state, info->rawArray);
                }
                InvalidateStructArrayLiteral(arg);
            }
            continue;
        }
        if (!arg->GetType()->IsRef()) {
            continue;
        }
        auto baseType = GetRefRootBaseType(arg->GetType());
        auto object = state.CheckAbstractObjectRefBy(arg);
        if (i < refArgValues.size() && refArgValues[i].has_value() && object != nullptr && baseType != nullptr &&
            (baseType->IsBoolean() || baseType->IsInteger() || baseType->IsClass() || baseType->IsStruct() ||
                baseType->IsTuple())) {
            ApplyContextValue(state, object, baseType, *refArgValues[i]);
        } else {
            ForgetReferenceArgument(state, arg);
        }
    }
    for (const auto& [global, inputValue] : contextGlobals) {
        (void)inputValue;
        auto baseType = GetTrackedMutableGlobalBaseType(global);
        auto summaryIt = std::find_if(summarizedGlobals.begin(), summarizedGlobals.end(),
            [global = global](const auto& entry) { return entry.first == global; });
        auto object = EnsureMutableGlobalValueInitialized(state, global);
        if (baseType == nullptr || summaryIt == summarizedGlobals.end() || summaryIt->second.IsTop()) {
            state.Update(object, /* isTop = */ true);
            continue;
        }
        ApplyContextValue(state, global, summaryIt->second);
    }
    if (summary.has_value()) {
        ApplyContextValue(state, result, summary.value());
    } else if (result != nullptr) {
        RecordContextTopSource("call-summary-unavailable");
        state.SetToTopOrTopRef(result, result->GetType()->IsRef());
    }
}

std::string RangeAnalysis::BuildLambdaContextKey(
    const RangeDomain& state, const Lambda* lambda, const std::vector<Value*>& args)
{
    std::stringstream key;
    key << lambda->GetIdentifier() << '@' << static_cast<const void*>(lambda) << '(';
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            key << ',';
        }
        auto argument = args[i];
        key << CaptureContextValue(state, argument, /* preserveIntervals = */ true)
                   .ToKeyString(argument == nullptr ? nullptr : argument->GetType());
    }
    key << ")captures(";
    auto captures = lambda->GetCapturedVariables();
    for (size_t i = 0; i < captures.size(); ++i) {
        if (i != 0) {
            key << ',';
        }
        auto captured = captures[i];
        key << static_cast<const void*>(captured) << '='
            << CaptureContextValue(state, captured, /* preserveIntervals = */ true)
                   .ToKeyString(captured == nullptr ? nullptr : captured->GetType());
    }
    key << ")globals(";
    auto globals = CollectTrackedMutableGlobals(lambda, builder.GetCurPackage());
    for (size_t i = 0; i < globals.size(); ++i) {
        if (i != 0) {
            key << ',';
        }
        auto global = globals[i];
        auto baseType = GetTrackedMutableGlobalBaseType(global);
        auto object = state.CheckAbstractObjectRefBy(global);
        key << static_cast<const void*>(global) << '='
            << CaptureContextValue(state, object, baseType, /* preserveIntervals = */ true)
                   .ToKeyString(baseType);
    }
    key << ')';
    return key.str();
}

void RangeAnalysis::ApplyLambdaContextSummary(RangeDomain& state, const LambdaContextualSummary& summary,
    const std::vector<Value*>& args, Value* result) const
{
    for (size_t i = 0; i < args.size(); ++i) {
        auto arg = args[i];
        auto baseType = arg == nullptr ? nullptr : GetRefRootBaseType(arg->GetType());
        if (!IsSupportedContextRootType(baseType)) {
            continue;
        }
        auto object = state.CheckAbstractObjectRefBy(arg);
        if (object == nullptr || i >= summary.refArgValues.size() || !summary.refArgValues[i].has_value() ||
            summary.refArgValues[i]->IsTop()) {
            ForgetReferenceArgument(state, arg);
            continue;
        }
        ApplyContextValue(state, object, baseType, summary.refArgValues[i].value());
    }
    for (const auto& [captured, value] : summary.capturedValues) {
        auto baseType = captured == nullptr ? nullptr : GetRefRootBaseType(captured->GetType());
        auto object = captured == nullptr ? nullptr : state.CheckAbstractObjectRefBy(captured);
        if (baseType == nullptr || object == nullptr || value.IsTop()) {
            if (captured != nullptr) {
                state.ForgetValueAndChildren(captured);
            }
            continue;
        }
        ApplyContextValue(state, captured, value);
    }
    for (const auto& [global, value] : summary.globalValues) {
        auto baseType = GetTrackedMutableGlobalBaseType(global);
        auto object = EnsureMutableGlobalValueInitialized(state, global);
        if (baseType == nullptr || value.IsTop()) {
            state.Update(object, /* isTop = */ true);
            continue;
        }
        ApplyContextValue(state, global, value);
    }
    if (result != nullptr) {
        if (summary.returnValue.has_value() && !summary.returnValue->IsTop()) {
            ApplyContextValue(state, result, summary.returnValue.value());
        } else {
            state.SetToTopOrTopRef(result, result->GetType()->IsRef());
        }
    }
}

bool RangeAnalysis::HandleLambdaContextSensitiveCall(
    RangeDomain& state, const Lambda* lambda, const std::vector<Value*>& args, Value* result)
{
    if (lambda == nullptr || lambda->GetBody() == nullptr) {
        return false;
    }
    const auto params = lambda->GetParams();
    const bool canAnalyze = params.size() == args.size() && CanAnalyzeLambdaContext(lambda);
    if (!canAnalyze || !CollectTrackedMutableGlobals(lambda, builder.GetCurPackage()).complete) {
        return false;
    }
    auto key = BuildLambdaContextKey(state, lambda, args);
    if (auto found = lambdaContextSummaries.find(key); found != lambdaContextSummaries.end()) {
        if (!found->second->ready) {
            return false;
        }
        ApplyLambdaContextSummary(state, *found->second, args, result);
        return true;
    }
    if (lambdaContextSummaries.size() >= MAX_LAMBDA_CONTEXTS_PER_ANALYSIS) {
        return false;
    }
    auto pending = std::make_unique<LambdaContextualSummary>();
    auto summary = pending.get();
    lambdaContextSummaries.emplace(key, std::move(pending));
    if (!AnalyzeLambdaWithContext(lambda, args, state, *summary)) {
        lambdaContextSummaries.erase(key);
        return false;
    }
    summary->ready = true;
    ApplyLambdaContextSummary(state, *summary, args, result);
    return true;
}

bool RangeAnalysis::AnalyzeLambdaWithContext(const Lambda* lambda, const std::vector<Value*>& args,
    const RangeDomain& callerState, LambdaContextualSummary& summary)
{
    auto body = lambda->GetBody();
    auto blocks = body == nullptr ? std::vector<Block*>{} : body->GetBlocks();
    if (body == nullptr || blocks.empty() ||
        (GetBlockLimit().has_value() && blocks.size() > GetBlockLimit().value())) {
        return false;
    }

    auto lambdaState = callerState;
    auto params = lambda->GetParams();
    std::vector<AbstractObject*> parameterObjects(params.size(), nullptr);
    for (size_t i = 0; i < params.size(); ++i) {
        auto param = params[i];
        auto argument = args[i];
        if (param == nullptr || argument == nullptr || param->GetType() == nullptr) {
            return false;
        }
        if (param->GetType()->IsRef()) {
            auto object = lambdaState.CheckAbstractObjectRefBy(argument);
            if (object == nullptr) {
                object = lambdaState.GetReferencedObjAndSetToTop(param);
            } else {
                lambdaState.SetRefToObject(param, object);
            }
            parameterObjects[i] = object;
            continue;
        }
        if (lambdaState.CheckAbstractValueWithTopBottom(argument) == nullptr) {
            lambdaState.SetToTopOrTopRef(param, /* isRef = */ false);
        } else {
            lambdaState.Propagate(argument, param);
        }
    }

    std::unordered_map<Block*, RangeDomain> entryStates;
    for (auto block : blocks) {
        entryStates.emplace(block, Bottom());
    }
    auto entry = lambda->GetEntryBlock();
    if (entry == nullptr || entryStates.find(entry) == entryStates.end()) {
        return false;
    }
    entryStates.at(entry) = lambdaState;

    std::deque<Block*> worklist = TopologicalSort(entry);
    std::unordered_set<Block*> queued(worklist.begin(), worklist.end());
    while (!worklist.empty()) {
        auto block = worklist.front();
        worklist.pop_front();
        queued.erase(block);
        auto state = entryStates.at(block);
        if (state.IsBottom()) {
            continue;
        }
        auto expressions = block->GetExpressions();
        if (expressions.empty()) {
            continue;
        }
        auto terminator = StaticCast<Terminator*>(expressions.back());
        expressions.pop_back();
        for (auto expression : expressions) {
            if (expression->GetExprKind() == ExprKind::LAMBDA) {
                PreHandleLambdaExpression(state, StaticCast<const Lambda*>(expression));
            } else {
                PropagateExpressionEffect(state, expression);
            }
        }
        auto target = PropagateTerminatorEffect(state, terminator);
        if (CheckInQueueTimes(block, state)) {
            target = std::nullopt;
        }
        auto successors = target.has_value() ? std::vector<Block*>{target.value()} : block->GetSuccessors();
        for (auto successor : successors) {
            auto found = entryStates.find(successor);
            if (found == entryStates.end()) {
                continue;
            }
            auto edgeState = GetTerminatorStateForSuccessor(*this, state, terminator, successor);
            if (found->second.Join(edgeState) && queued.emplace(successor).second) {
                worklist.emplace_back(successor);
            }
        }
    }

    std::optional<RangeDomain> joinedExitState;
    for (auto block : blocks) {
        auto terminator = block->GetTerminator();
        auto found = entryStates.find(block);
        if (terminator == nullptr || terminator->GetExprKind() != ExprKind::EXIT ||
            found == entryStates.end() || found->second.IsBottom()) {
            continue;
        }
        auto state = found->second;
        for (auto expression : block->GetNonTerminatorExpressions()) {
            if (expression->GetExprKind() == ExprKind::LAMBDA) {
                PreHandleLambdaExpression(state, StaticCast<const Lambda*>(expression));
            } else {
                PropagateExpressionEffect(state, expression);
            }
        }
        PropagateTerminatorEffect(state, terminator);
        if (!joinedExitState.has_value()) {
            joinedExitState = std::move(state);
        } else {
            joinedExitState->Join(state);
        }
    }
    if (!joinedExitState.has_value()) {
        return false;
    }

    summary.returnValue = CaptureContextValue(
        joinedExitState.value(), lambda->GetReturnValue(), /* preserveIntervals = */ true);
    summary.refArgValues.resize(params.size());
    for (size_t i = 0; i < params.size(); ++i) {
        auto baseType = GetRefRootBaseType(params[i]->GetType());
        if (!IsSupportedContextRootType(baseType)) {
            continue;
        }
        summary.refArgValues[i] = CaptureContextValue(
            joinedExitState.value(), parameterObjects[i], baseType, /* preserveIntervals = */ true);
    }
    for (auto captured : lambda->GetCapturedVariables()) {
        auto baseType = captured == nullptr ? nullptr : GetRefRootBaseType(captured->GetType());
        if (!IsSupportedContextRootType(baseType)) {
            continue;
        }
        auto object = joinedExitState->CheckAbstractObjectRefBy(captured);
        summary.capturedValues.emplace_back(captured,
            CaptureContextValue(joinedExitState.value(), object, baseType, /* preserveIntervals = */ true));
    }
    for (auto global : CollectTrackedMutableGlobals(lambda, builder.GetCurPackage())) {
        auto baseType = GetTrackedMutableGlobalBaseType(global);
        auto object = joinedExitState->CheckAbstractObjectRefBy(global);
        summary.globalValues.emplace_back(global,
            CaptureContextValue(joinedExitState.value(), object, baseType, /* preserveIntervals = */ true));
    }
    summary.lambda = lambda;
    summary.entryStates = std::move(entryStates);
    return true;
}

std::string RangeAnalysis::BuildContextKey(
    const Function* callee, const ContextArguments& arguments, const ContextGlobalValues& globalValues)
{
    if (IsRangeAnalysisPerfCollectionEnabled()) {
        ++rangeAnalysisPerfStats.contextKeyCalls;
    }
    ScopedRangeAnalysisPerfTimer timer(&rangeAnalysisPerfStats.contextKeyNanos);
    std::stringstream keyStream;
    std::unordered_map<size_t, size_t> canonicalObjectIdentities;
    size_t nextCanonicalObjectIdentity = 1;
    keyStream << callee->GetSrcCodeIdentifier() << "@" << static_cast<const void*>(callee) << "(";
    auto params = callee->GetParams();
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (i != 0) {
            keyStream << ",";
        }
        auto paramType = i < params.size() ? params[i]->GetType() : nullptr;
        keyStream << arguments[i].ToKeyString(
            paramType, canonicalObjectIdentities, nextCanonicalObjectIdentity);
    }
    keyStream << ")globals(";
    for (size_t i = 0; i < globalValues.size(); ++i) {
        if (i != 0) {
            keyStream << ",";
        }
        auto global = globalValues[i].first;
        keyStream << static_cast<const void*>(global) << "="
                  << globalValues[i].second.ToKeyString(GetTrackedMutableGlobalBaseType(global),
                         canonicalObjectIdentities, nextCanonicalObjectIdentity);
    }
    keyStream << ")";
    return keyStream.str();
}

std::optional<RangeAnalysis::ContextAbstractValue> RangeAnalysis::AnalyzeCalleeWithContext(
    const Function* callee, const ContextArguments& arguments,
    std::vector<std::optional<ContextAbstractValue>>& refArgValues, ContextGlobalValues& globalValues,
    const std::string* precomputedKey)
{
    ScopedOverrideTraceLocation traceLocation(callee, overrideTraceCallsite);
    ScopedContextAnalysisStage stage("callee-context-analysis");
    if (IsRangeAnalysisPerfCollectionEnabled()) {
        ++rangeAnalysisPerfStats.contextAnalysisCalls;
    }
    ScopedRangeAnalysisPerfTimer timer(&rangeAnalysisPerfStats.contextAnalysisNanos);
    if (auto affineRecursiveSummary =
            TryAnalyzeAffineRecursiveCallee(callee, arguments, globalValues);
        affineRecursiveSummary.has_value()) {
        refArgValues.clear();
        return affineRecursiveSummary;
    }
    auto key = precomputedKey == nullptr
        ? BuildContextKey(callee, arguments, globalValues)
        : *precomputedKey;
    // Exact scalar recursion carries no abstract memory and each context has a
    // single concrete argument tuple.  A separate, still-bounded budget lets
    // finite descending recurrences finish without weakening the general
    // context limits used by interval, reference, and side-effecting calls.
    constexpr size_t MAX_EXACT_SCALAR_RECURSION_DEPTH = 512;
    constexpr size_t MAX_EXACT_SCALAR_RECURSION_CONTEXTS_PER_FUNCTION = 512;
    const auto params = callee == nullptr ? std::vector<Parameter*>{} : callee->GetParams();
    auto returnType = callee == nullptr || !callee->HasReturnValue()
        ? nullptr
        : callee->GetReturnValue()->GetType();
    const bool hasScalarReturn = IsSupportedLambdaResultType(returnType);
    const bool isRecursiveCall = callee != nullptr &&
        std::find(contextSummaryFunctionStack.begin(), contextSummaryFunctionStack.end(), callee) !=
            contextSummaryFunctionStack.end();
    const bool isExactScalarRecursiveContext = isRecursiveCall && globalValues.empty() &&
        hasScalarReturn &&
        params.size() == arguments.size() &&
        std::all_of(arguments.begin(), arguments.end(),
            [](const auto& argument) { return argument.IsSingleValue(); }) &&
        std::all_of(params.begin(), params.end(), [](const Parameter* parameter) {
            auto type = parameter == nullptr ? nullptr : parameter->GetType();
            return type != nullptr && !type->IsRef() && (type->IsInteger() || type->IsBoolean());
        });
    const auto setUnknownOutputs = [&refArgValues, &globalValues]() {
        refArgValues.clear();
        for (auto& entry : globalValues) {
            entry.second = ContextAbstractValue{};
        }
    };
    const auto instantiateCachedSummary =
        [&arguments, &refArgValues, &globalValues, &setUnknownOutputs](
            const ContextualSummary& cached) -> std::optional<ContextAbstractValue> {
        if (IsRangeAnalysisPerfCollectionEnabled()) {
            ++rangeAnalysisPerfStats.contextSummaryInstantiations;
        }
        ScopedRangeAnalysisPerfTimer instantiateTimer(
            &rangeAnalysisPerfStats.contextSummaryInstantiationNanos);
        if (cached.inputArguments.size() != arguments.size() ||
            cached.inputGlobalValues.size() != globalValues.size()) {
            setUnknownOutputs();
            return std::nullopt;
        }
        std::unordered_map<size_t, size_t> identities;
        for (size_t i = 0; i < arguments.size(); ++i) {
            if (!cached.inputArguments[i].BindObjectIdentities(arguments[i], identities)) {
                setUnknownOutputs();
                return std::nullopt;
            }
        }
        for (size_t i = 0; i < globalValues.size(); ++i) {
            if (cached.inputGlobalValues[i].first != globalValues[i].first ||
                !cached.inputGlobalValues[i].second.BindObjectIdentities(
                    globalValues[i].second, identities)) {
                setUnknownOutputs();
                return std::nullopt;
            }
        }

        auto returnValue = cached.returnValue;
        refArgValues = cached.refArgValues;
        globalValues = cached.globalValues;
        if (returnValue.has_value()) {
            returnValue->RebindObjectIdentities(identities);
        }
        for (auto& value : refArgValues) {
            if (value.has_value()) {
                value->RebindObjectIdentities(identities);
            }
        }
        for (auto& [global, value] : globalValues) {
            (void)global;
            value.RebindObjectIdentities(identities);
        }
        return returnValue;
    };
    {
        std::lock_guard<std::mutex> lock(GetContextSummaryMutex());
        auto& summaryCache = GetContextSummaryCache();
        if (failedContextKeys.find(key) != failedContextKeys.end()) {
            ++contextSummaryCacheHitCount;
            if (IsRangeAnalysisPerfCollectionEnabled()) {
                ++rangeAnalysisPerfStats.contextFailedCacheHits;
            }
            setUnknownOutputs();
            return std::nullopt;
        }
        if (auto it = summaryCache.find(key); it != summaryCache.end()) {
            ++contextSummaryCacheHitCount;
            if (it->second.state == ContextSummaryState::READY) {
                if (IsRangeAnalysisPerfCollectionEnabled()) {
                    ++rangeAnalysisPerfStats.contextReadyCacheHits;
                }
                return instantiateCachedSummary(it->second);
            }
            if (it->second.state == ContextSummaryState::COMPUTING) {
                ++contextSummaryRecursiveHitCount;
                if (IsRangeAnalysisPerfCollectionEnabled()) {
                    ++rangeAnalysisPerfStats.contextComputingCacheHits;
                }
            }
            setUnknownOutputs();
            return std::nullopt;
        }
        const auto rejectContext = [&key]() {
            ++contextSummaryBudgetRejectCount;
            if (IsRangeAnalysisPerfCollectionEnabled()) {
                ++rangeAnalysisPerfStats.contextBudgetRejects;
            }
            RecordContextTopSource("summary-budget-reject");
            if (failedContextKeys.size() < MaxTotalReachableContextSummaries()) {
                failedContextKeys.emplace(key);
            }
        };
        size_t regularSummaryCount = 0;
        for (const auto& [_, count] : GetContextCounts()) {
            regularSummaryCount += count;
        }
        size_t boundedSummaryCount = 0;
        for (const auto& [_, count] : GetBoundedLoopContextCounts()) {
            boundedSummaryCount += count;
        }
        const bool summaryLimitReached = boundedLoopEvaluationOwner != nullptr
            ? boundedSummaryCount >= MAX_TOTAL_BOUNDED_LOOP_CONTEXT_SUMMARIES
            : regularSummaryCount >= rangeAnalysisConfig.maxTotalContextSummaries;
        const auto contextDepthLimit = isExactScalarRecursiveContext
            ? MAX_EXACT_SCALAR_RECURSION_DEPTH
            : rangeAnalysisConfig.maxContextAnalysisDepth;
        if (IsOverrideTraceEnabled() &&
            (contextSummaryAnalysisDepth >= contextDepthLimit || summaryLimitReached ||
                GetContextSummaryOrder().size() >= MaxTotalReachableContextSummaries() ||
                IsContextAnalysisBudgetExhausted())) {
            std::cerr << "[RangeAnalysisRecursiveBudget] callee="
                      << (callee == nullptr ? "<null>" : callee->GetIdentifier())
                      << " recursive=" << isRecursiveCall
                      << " exact-scalar=" << isExactScalarRecursiveContext
                      << " globals=" << globalValues.size()
                      << " params=" << params.size()
                      << " arguments=" << arguments.size()
                      << " scalar-return=" << hasScalarReturn
                      << " all-single="
                      << std::all_of(arguments.begin(), arguments.end(),
                             [](const auto& argument) { return argument.IsSingleValue(); })
                      << " depth=" << contextSummaryAnalysisDepth
                      << " depth-limit=" << contextDepthLimit
                      << " regular-summaries=" << regularSummaryCount
                      << " summary-limit=" << summaryLimitReached
                      << '\n';
        }
        if (contextSummaryAnalysisDepth >= contextDepthLimit ||
            summaryLimitReached ||
            GetContextSummaryOrder().size() >= MaxTotalReachableContextSummaries() ||
            IsContextAnalysisBudgetExhausted()) {
            rejectContext();
            setUnknownOutputs();
            return std::nullopt;
        }
        if (boundedLoopEvaluationOwner != nullptr) {
            auto& boundedCount = GetBoundedLoopContextCounts()[callee];
            if (boundedCount >= MAX_BOUNDED_LOOP_CONTEXT_PER_FUNCTION) {
                rejectContext();
                setUnknownOutputs();
                return std::nullopt;
            }
            ++boundedCount;
        } else {
            auto& count = GetContextCounts()[callee];
            auto contextLimit = isExactScalarRecursiveContext
                ? MAX_EXACT_SCALAR_RECURSION_CONTEXTS_PER_FUNCTION
                : globalValues.empty() ? rangeAnalysisConfig.maxContextPerFunction
                                       : MAX_GLOBAL_CONTEXT_PER_FUNCTION;
            if (count >= contextLimit) {
                rejectContext();
                setUnknownOutputs();
                return std::nullopt;
            }
            ++count;
        }
        ContextualSummary pending;
        pending.callee = callee;
        pending.inputArguments = arguments;
        pending.inputGlobalValues = globalValues;
        pending.state = ContextSummaryState::COMPUTING;
        pending.precision = static_cast<size_t>(std::count_if(
            arguments.begin(), arguments.end(), [](const auto& argument) { return !argument.IsTop(); }));
        pending.precision += static_cast<size_t>(std::count_if(globalValues.begin(), globalValues.end(),
            [](const auto& entry) { return !entry.second.IsTop(); }));
        summaryCache.emplace(key, std::move(pending));
        GetContextSummaryOrder().emplace_back(key);
        ++contextSummaryAnalysisCount;
        if (IsRangeAnalysisPerfCollectionEnabled()) {
            ++rangeAnalysisPerfStats.contextNewAnalyses;
        }
        if (contextSummaryAnalysisDepth + 1 > contextSummaryPeakDepth) {
            contextSummaryPeakDepth = contextSummaryAnalysisDepth + 1;
        }
    }

    struct ContextDepthGuard {
        explicit ContextDepthGuard(const Function* function)
        {
            ++contextSummaryAnalysisDepth;
            contextSummaryFunctionStack.emplace_back(function);
        }

        ~ContextDepthGuard()
        {
            contextSummaryFunctionStack.pop_back();
            --contextSummaryAnalysisDepth;
        }
    } depthGuard{callee};

    std::optional<ContextAbstractValue> returnValue;
    std::vector<std::optional<ContextAbstractValue>> summarizedRefArgs;
    ContextGlobalValues summarizedGlobals;
    auto contextAnalysis = std::unique_ptr<RangeAnalysis>(new RangeAnalysis(
        callee, builder, isDebug, diag, ContextArguments(arguments),
        ContextGlobalValues(globalValues)));
    auto contextAnalysisPtr = contextAnalysis.get();
    auto engine = Engine<RangeDomain>(callee, std::move(contextAnalysis));
    std::unique_ptr<Results<RangeDomain>> results;
    {
        ScopedRangeAnalysisPerfTimer fixpointTimer(
            &rangeAnalysisPerfStats.contextFixpointNanos);
        results = engine.IterateToFixpoint();
    }
    if (results != nullptr) {
        ScopedRangeAnalysisPerfTimer summaryTimer(
            &rangeAnalysisPerfStats.contextSummaryBuildNanos);
        contextAnalysisPtr->SummarizeContextOutputs(callee, globalValues, *results, returnValue,
            summarizedRefArgs, summarizedGlobals);
    }
    // A context budget cut turns the current abstract state into Top and
    // propagates that sound over-approximation to successors.  The resulting
    // summary remains valid; rejecting it here would poison every caller that
    // is still unwinding after an unrelated global budget exhaustion.
    const bool analysisFailed = results == nullptr;
    if (analysisFailed) {
        RecordContextTopSource(
            results == nullptr ? "summary-fixpoint-failed" : "summary-global-budget-poison");
    }
    {
        std::lock_guard<std::mutex> lock(GetContextSummaryMutex());
        auto& cached = GetContextSummaryCache()[key];
        if (analysisFailed) {
            cached.state = ContextSummaryState::FAILED;
            ++contextSummaryFailedCount;
        } else {
            cached.state = ContextSummaryState::READY;
            cached.returnValue = returnValue;
            cached.refArgValues = summarizedRefArgs;
            cached.globalValues = summarizedGlobals;
            cached.result = std::move(results);
        }
    }
    if (analysisFailed) {
        setUnknownOutputs();
        return std::nullopt;
    }
    refArgValues = std::move(summarizedRefArgs);
    globalValues = std::move(summarizedGlobals);
    return returnValue;
}

void RangeAnalysis::SummarizeContextOutputs(const Function* callee,
    const ContextGlobalValues& globals, Results<RangeDomain>& results,
    std::optional<ContextAbstractValue>& returnValue,
    std::vector<std::optional<ContextAbstractValue>>& refArgValues,
    ContextGlobalValues& globalValues)
{
    const bool tracksReturn = callee->HasReturnValue();
    auto ret = tracksReturn ? callee->GetReturnValue() : nullptr;
    bool returnBecameTop = false;

    auto params = callee->GetParams();
    refArgValues.assign(params.size(), std::nullopt);
    std::vector<bool> trackedRef(params.size(), false);
    std::vector<bool> refBecameTop(params.size(), false);

    for (size_t i = 0; i < params.size(); ++i) {
        auto type = params[i]->GetType();
        auto baseType = GetRefRootBaseType(type);
        trackedRef[i] =
            IsStructArrayValue(params[i]) || IsSupportedContextRootType(baseType);
    }

    globalValues.clear();
    globalValues.reserve(globals.size());
    for (const auto& [global, value] : globals) {
        (void)value;
        globalValues.emplace_back(global, ContextAbstractValue{});
    }
    std::vector<bool> sawGlobalValue(globals.size(), false);
    std::vector<bool> globalBecameTop(globals.size(), false);
    bool sawExit = false;

    results.VisitWith(
        [](const RangeDomain&, Expression*, size_t) {},
        [](const RangeDomain&, Expression*, size_t) {},
        [&](const RangeDomain& state, Terminator* terminator, std::optional<Block*>) {
            if (terminator->GetExprKind() != ExprKind::EXIT) {
                return;
            }
            auto parentBlock = terminator->GetParentBlock();
            auto parentGroup = parentBlock == nullptr ? nullptr : parentBlock->GetParentBlockGroup();
            if (parentGroup == nullptr || parentGroup->GetFuncOrLambdaBody() != callee->GetBody()) {
                return;
            }
            sawExit = true;

            if (tracksReturn && !returnBecameTop) {
                auto value =
                    CaptureContextValue(state, ret, /* preserveIntervals = */ true);
                if (value.kind == ContextAbstractValue::Kind::OBJECT &&
                    value.classValue == nullptr) {
                    auto returnClasses = ResolveFiniteClassSetForValue(ret);
                    if (returnClasses.has_value() && returnClasses->size() == 1) {
                        value.classValue = returnClasses->front();
                    }
                }
                if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                    std::cerr << "[RangeAnalysisReturnSummary] type="
                              << (ret->GetType() == nullptr
                                      ? "<null>"
                                      : ret->GetType()->ToString())
                              << " kind=" << static_cast<unsigned>(value.kind)
                              << " fields=" << value.objectFields.size()
                              << " identity=" << value.objectIdentity
                              << " class="
                              << (value.classValue == nullptr
                                      ? "<null>"
                                      : value.classValue->ToString())
                              << '\n';
                    if (ret->GetType() != nullptr && ret->GetType()->IsEnum()) {
                        auto definingExpression = ret->IsLocalVar()
                            ? StaticCast<LocalVar*>(ret)->GetExpr()
                            : nullptr;
                        std::cerr << "[RangeAnalysisEnumReturn] value=" << ret
                                  << " definition="
                                  << (definingExpression == nullptr
                                          ? -1
                                          : static_cast<int>(definingExpression->GetExprKind()))
                                  << " users=" << ret->GetUsers().size()
                                  << " tag=";
                        auto children = const_cast<RangeDomain&>(state).GetChildren(ret);
                        auto tagRange = children.empty()
                            ? nullptr
                            : state.CheckAbstractValue(children.front());
                        std::cerr << (tagRange == nullptr ? "unknown" : tagRange->ToString());
                        std::cerr << '\n';
                        for (auto user : ret->GetUsers()) {
                            std::cerr << "[RangeAnalysisEnumReturnUser] kind="
                                      << static_cast<int>(user->GetExprKind())
                                      << " line="
                                      << user->GetDebugLocation().GetBeginPos().line << '\n';
                        }
                    }
                }
                if (value.IsTop()) {
                    returnBecameTop = true;
                    returnValue.reset();
                } else if (!returnValue.has_value()) {
                    returnValue = std::move(value);
                } else {
                    returnValue =
                        JoinContextValues(returnValue.value(), value);
                    if (!returnValue.has_value() || returnValue->IsTop()) {
                        returnBecameTop = true;
                        returnValue.reset();
                    }
                }
            }

            for (size_t i = 0; i < params.size(); ++i) {
                if (!trackedRef[i] || refBecameTop[i]) {
                    continue;
                }
                auto value = CaptureContextValue(state, params[i], /* preserveIntervals = */ true);
                if (value.kind == ContextAbstractValue::Kind::OBJECT && i < contextArguments.size()) {
                    value.classValue = contextArguments[i].classValue;
                }
                if (value.IsTop()) {
                    refBecameTop[i] = true;
                    refArgValues[i].reset();
                    continue;
                }
                if (!refArgValues[i].has_value()) {
                    refArgValues[i] = std::move(value);
                    continue;
                }
                refArgValues[i] =
                    JoinContextValues(refArgValues[i].value(), value);
                if (!refArgValues[i].has_value() || refArgValues[i]->IsTop()) {
                    refBecameTop[i] = true;
                    refArgValues[i].reset();
                }
            }

            for (size_t i = 0; i < globals.size(); ++i) {
                if (globalBecameTop[i]) {
                    continue;
                }
                auto global = globals[i].first;
                auto value = CaptureContextValue(state, global, /* preserveIntervals = */ true);
                if (value.IsTop()) {
                    globalBecameTop[i] = true;
                    globalValues[i].second = ContextAbstractValue{};
                    continue;
                }
                if (!sawGlobalValue[i]) {
                    sawGlobalValue[i] = true;
                    globalValues[i].second = std::move(value);
                    continue;
                }
                globalValues[i].second =
                    JoinContextValues(globalValues[i].second, value);
                globalBecameTop[i] = globalValues[i].second.IsTop();
            }
        });

    if (!sawExit) {
        returnValue.reset();
        refArgValues.clear();
        return;
    }

    if (!tracksReturn || returnBecameTop) {
        returnValue.reset();
    }
    for (size_t i = 0; i < refArgValues.size(); ++i) {
        if (!trackedRef[i] || refBecameTop[i]) {
            refArgValues[i].reset();
        }
    }
    for (size_t i = 0; i < globalValues.size(); ++i) {
        if (!sawGlobalValue[i]) {
            globalValues[i].second = ContextAbstractValue{};
        }
    }
}

ValueRange::ValueRange(RangeKind kind) : kind(kind)
{
}

ValueRange::~ValueRange()
{
}

ValueRange::RangeKind ValueRange::GetRangeKind() const
{
    return kind;
}

BoolRange::BoolRange(BoolDomain domain) : ValueRange(RangeKind::BOOL), domain(std::move(domain))
{
}

std::optional<std::unique_ptr<ValueRange>> BoolRange::Join(const ValueRange& rhs) const
{
    CJC_ASSERT(rhs.GetRangeKind() == RangeKind::BOOL);
    const auto& rhsRange = StaticCast<const BoolRange&>(rhs);
    auto joinedDomain = BoolDomain::Union(domain, rhsRange.domain);
    if (domain.IsSame(joinedDomain)) {
        return std::nullopt;
    }
    return std::make_unique<BoolRange>(BoolRange{std::move(joinedDomain)});
}

std::string BoolRange::ToString() const
{
    std::stringstream ss;
    ss << domain;
    return ss.str();
}

std::unique_ptr<ValueRange> BoolRange::Clone() const
{
    return std::make_unique<BoolRange>(domain);
}

const BoolDomain& BoolRange::GetVal() const
{
    return domain;
}

SIntRange::SIntRange(SIntDomain domain)
    : SIntRange(std::move(domain), std::nullopt, std::nullopt, std::nullopt, std::nullopt)
{
}

SIntRange::SIntRange(SIntDomain domain, std::optional<std::vector<SInt>> exactValues)
    : SIntRange(std::move(domain), std::move(exactValues), std::nullopt)
{
}

SIntRange::SIntRange(SIntDomain domain, std::optional<std::vector<SInt>> exactValues,
    std::optional<SIntCongruence> congruence)
    : SIntRange(std::move(domain), std::move(exactValues), std::move(congruence), std::nullopt)
{
}

SIntRange::SIntRange(SIntDomain domain, std::optional<std::vector<SInt>> exactValues,
    std::optional<SIntCongruence> congruence, std::optional<SIntKnownBits> knownBits)
    : SIntRange(std::move(domain), std::move(exactValues), std::move(congruence),
          std::move(knownBits), std::nullopt)
{
}

SIntRange::SIntRange(SIntDomain domain, std::optional<std::vector<SInt>> exactValues,
    std::optional<SIntCongruence> congruence, std::optional<SIntKnownBits> knownBits,
    std::optional<std::vector<SInt>> excludedValues,
    std::optional<std::vector<SIntIntervalFragment>> intervalFragments)
    : ValueRange(RangeKind::SINT), domain(std::move(domain)),
      exactValues(exactValues.has_value() ? NormalizeExactIntSet(std::move(*exactValues)) : std::nullopt),
      congruence(NormalizeCongruence(std::move(congruence))),
      knownBits(NormalizeKnownBits(std::move(knownBits), this->domain.Width())),
      excludedValues(nullptr), intervalFragments(nullptr)
{
    if (!this->exactValues.has_value() && this->domain.IsSingleValue()) {
        this->exactValues = std::vector<SInt>{this->domain.NumericBound().GetSingleElement()};
    }
    if (!this->congruence.has_value()) {
        this->congruence = InferCongruenceFromExactValues(this->exactValues, this->domain.IsUnsigned());
    }
    if (this->knownBits.has_value()) {
        this->domain = SIntDomain::Intersects(this->domain,
            DomainFromKnownBits(this->knownBits, this->domain.Width(), this->domain.IsUnsigned()));
    }
    auto normalizedExcludedValues = excludedValues.has_value()
        ? NormalizeExcludedSIntValues(std::move(*excludedValues), this->domain)
        : std::nullopt;
    if (normalizedExcludedValues.has_value()) {
        this->exactValues = FilterExactValuesByExclusions(
            std::move(this->exactValues), normalizedExcludedValues);
    }
    if (!this->exactValues.has_value() && this->domain.IsSingleValue()) {
        const auto singleton = this->domain.NumericBound().GetSingleElement();
        if (!IsExplicitlyExcluded(normalizedExcludedValues, singleton)) {
            this->exactValues = std::vector<SInt>{singleton};
        }
    }
    if (!this->congruence.has_value()) {
        this->congruence = InferCongruenceFromKnownBits(this->knownBits, this->domain.Width());
    }
    if (normalizedExcludedValues.has_value()) {
        this->excludedValues =
            std::make_unique<std::optional<std::vector<SInt>>>(std::move(normalizedExcludedValues));
    }
    auto normalizedFragments = intervalFragments.has_value()
        ? NormalizeSIntIntervalFragments(std::move(*intervalFragments), this->domain)
        : std::nullopt;
    normalizedFragments = RefineSIntIntervalFragmentsByCongruence(
        std::move(normalizedFragments), this->congruence, this->domain);
    if (normalizedFragments.has_value()) {
        this->intervalFragments =
            std::make_unique<std::optional<std::vector<SIntIntervalFragment>>>(
                std::move(normalizedFragments));
    }
}

std::optional<std::unique_ptr<ValueRange>> SIntRange::Join(const ValueRange& rhs) const
{
    CJC_ASSERT(rhs.GetRangeKind() == RangeKind::SINT);
    const auto& rhsRange = StaticCast<const SIntRange&>(rhs);
    auto mergedExactValues = MergeExactIntSets(exactValues, rhsRange.exactValues);
    auto mergedCongruence = MergeSIntCongruencesForUnion(domain, exactValues, congruence,
        rhsRange.domain, rhsRange.exactValues, rhsRange.congruence);
    auto lhsKnownBits = knownBits;
    auto rhsKnownBits = rhsRange.knownBits;
    if (knownBits.has_value() || rhsRange.knownBits.has_value()) {
        lhsKnownBits = GetEffectiveKnownBits(domain, exactValues, congruence, knownBits);
        rhsKnownBits = GetEffectiveKnownBits(
            rhsRange.domain, rhsRange.exactValues, rhsRange.congruence, rhsRange.knownBits);
    }
    auto mergedKnownBits = MergeSIntKnownBitsForUnion(
        domain, lhsKnownBits, rhsRange.domain, rhsKnownBits);
    auto joinedDomain = SIntDomain::Unions(domain, rhsRange.domain);
    auto mergedExcludedValues = MergeExcludedSIntValuesForUnion(domain, GetExcludedValues(),
        rhsRange.domain, rhsRange.GetExcludedValues(), joinedDomain);
    // Exact sets and bounded individual exclusions already describe the
    // non-convex union without loss. Avoid carrying a second representation
    // (and its vector allocations) through ordinary small joins.
    auto mergedIntervalFragments =
        mergedExactValues.has_value() || mergedExcludedValues.has_value()
        ? std::optional<std::vector<SIntIntervalFragment>>{}
        : MergeSIntIntervalFragmentsForUnion(*this, rhsRange, joinedDomain);
    if (domain.IsSame(joinedDomain) && exactValues == mergedExactValues && congruence == mergedCongruence &&
        knownBits == mergedKnownBits && GetExcludedValues() == mergedExcludedValues &&
        GetIntervalFragments() == mergedIntervalFragments &&
        !(domain.NumericBound().IsFullSet() && !domain.SymbolicBounds().Empty())) {
        return std::nullopt;
    }
    return std::make_unique<SIntRange>(
        SIntRange{std::move(joinedDomain), std::move(mergedExactValues), std::move(mergedCongruence),
            std::move(mergedKnownBits), std::move(mergedExcludedValues),
            std::move(mergedIntervalFragments)});
}

std::string SIntRange::ToString() const
{
    std::stringstream ss;
    ss << domain;
    const auto& exclusions = GetExcludedValues();
    if (exclusions.has_value()) {
        ss << "\\{";
        for (size_t i = 0; i < exclusions->size(); ++i) {
            if (i != 0) {
                ss << ",";
            }
            if (domain.IsUnsigned()) {
                ss << (*exclusions)[i].UVal();
            } else {
                ss << (*exclusions)[i].SVal();
            }
        }
        ss << "}";
    }
    const auto& fragments = GetIntervalFragments();
    if (fragments.has_value()) {
        ss << " fragments{";
        for (size_t i = 0; i < fragments->size(); ++i) {
            if (i != 0) {
                ss << ",";
            }
            if (domain.IsUnsigned()) {
                ss << "[" << (*fragments)[i].lower.UVal() << ","
                   << (*fragments)[i].upper.UVal() << ":"
                   << (*fragments)[i].stride << "]";
            } else {
                ss << "[" << (*fragments)[i].lower.SVal() << ","
                   << (*fragments)[i].upper.SVal() << ":"
                   << (*fragments)[i].stride << "]";
            }
        }
        ss << "}";
    }
    return ss.str();
}

std::unique_ptr<ValueRange> SIntRange::Clone() const
{
    return std::make_unique<SIntRange>(
        domain, exactValues, congruence, knownBits, GetExcludedValues(), GetIntervalFragments());
}

const SIntDomain& SIntRange::GetVal() const
{
    return domain;
}

const std::optional<std::vector<SInt>>& SIntRange::GetExactValues() const
{
    return exactValues;
}

const std::optional<SIntCongruence>& SIntRange::GetCongruence() const
{
    return congruence;
}

const std::optional<SIntKnownBits>& SIntRange::GetKnownBits() const
{
    return knownBits;
}

const std::optional<std::vector<SInt>>& SIntRange::GetExcludedValues() const
{
    static const std::optional<std::vector<SInt>> noExcludedValues;
    return excludedValues == nullptr ? noExcludedValues : *excludedValues;
}

const std::optional<std::vector<SIntIntervalFragment>>& SIntRange::GetIntervalFragments() const
{
    static const std::optional<std::vector<SIntIntervalFragment>> noIntervalFragments;
    return intervalFragments == nullptr ? noIntervalFragments : *intervalFragments;
}

template <> bool IsTrackedGV<RangeValueDomain>(const GlobalVar& gv)
{
    auto baseType = StaticCast<RefType*>(gv.GetType())->GetBaseType();
    auto baseTyKind = baseType->GetTypeKind();
    if ((baseTyKind >= Type::TYPE_INT8 && baseTyKind <= Type::TYPE_UINT_NATIVE) ||
        baseTyKind == Type::TYPE_ENUM || baseTyKind == Type::TYPE_BOOLEAN) {
        return true;
    }

    // Keep small immutable value aggregates in the package-wide abstract
    // state. Their scalar fields are otherwise discarded before functions
    // which load the global are analysed. Reference-bearing aggregates are
    // excluded here because the global-state copier does not model their
    // alias graph recursively.
    constexpr size_t MAX_TRACKED_GLOBAL_AGGREGATE_FIELDS = 16;
    if (baseTyKind != Type::TYPE_STRUCT) {
        return false;
    }
    auto structType = StaticCast<StructType*>(baseType);
    auto structDef = structType->GetStructDef();
    if (structDef == nullptr || structDef->IsCStruct() || structType->IsStructArray() ||
        structDef->GetDirectInstanceVarNum() == 0 ||
        structDef->GetDirectInstanceVarNum() > MAX_TRACKED_GLOBAL_AGGREGATE_FIELDS) {
        return false;
    }
    auto members = structDef->GetDirectInstanceVars();
    return std::all_of(members.begin(), members.end(),
        [](const MemberVarInfo& member) { return member.type != nullptr && !member.type->IsRef(); });
}

template <> RangeValueDomain HandleNonNullLiteralValue<RangeValueDomain>(const LiteralValue* literal)
{
    if (literal->IsBoolLiteral()) {
        return RangeValueDomain(
            std::make_unique<BoolRange>(BoolDomain::FromBool(StaticCast<BoolLiteral*>(literal)->GetVal())));
    } else if (literal->IsIntLiteral()) {
        auto domain = SIntDomain::From(*literal);
        return RangeValueDomain(std::make_unique<SIntRange>(domain,
            std::vector<SInt>{domain.NumericBound().GetSingleElement()}));
    } else {
        return RangeValueDomain(true);
    }
}

// 初始化 RangeAnalysis，并保存诊断器以便表达式 transfer 报告算术错误。
RangeAnalysis::RangeAnalysis(const Function* func, CHIRBuilder& builder, bool isDebug, DiagnosticEngine& diag)
    : ValueAnalysis(func, builder, isDebug), diag(&diag)
{
    analysisContextKey = BuildRootAnalysisContextKey(func);
}

RangeAnalysis::RangeAnalysis(
    const Function* func, CHIRBuilder& builder, bool isDebug, DiagnosticEngine* diag, ContextArguments contextArguments,
    ContextGlobalValues contextGlobalArguments)
    : ValueAnalysis(func, builder, isDebug), diag(diag), contextArguments(std::move(contextArguments)),
      contextGlobalArguments(std::move(contextGlobalArguments)), isContextAnalysis(true)
{
    analysisContextKey = BuildContextKey(func, this->contextArguments, this->contextGlobalArguments);
}

// 析构时清理当前分析实例对应的循环快照。
RangeAnalysis::~RangeAnalysis()
{
    {
        std::lock_guard<std::mutex> lock(loopRangeSnapshotsMtx);
        loopRangeSnapshots.erase(this);
    }
    {
        std::lock_guard<std::mutex> lock(GetBoundedLoopExitCacheMutex());
        GetBoundedLoopExitCaches().erase(this);
    }
    {
        std::lock_guard<std::mutex> lock(boundedLoopObservationsMtx);
        localBoundedLoopPrefixObservations.erase(this);
        completedBoundedLoopPrefixAttempts.erase(this);
    }
}

void RangeAnalysis::SetQueryRefinementContext(std::unordered_set<const Block*> blocks,
    std::unordered_set<const Value*> values, std::unordered_set<const Value*> roots)
{
    queryRefinementBlocks = std::move(blocks);
    queryRefinementValues = std::move(values);
    queryRefinementRoots = std::move(roots);
}

void RangeAnalysis::ClearQueryRefinementBlocks()
{
    queryRefinementBlocks.clear();
    queryRefinementValues.clear();
    queryRefinementRoots.clear();
}

void RangeAnalysis::RecordTerminatorEdgeState(
    const Terminator* terminator, const Block* successor, const RangeDomain& state)
{
    if (terminator == nullptr || successor == nullptr) {
        return;
    }
    auto& successorStates = terminatorEdgeStates[terminator];
    auto found = successorStates.find(successor);
    if (found == successorStates.end()) {
        successorStates.emplace(successor, state);
    } else {
        found->second = state;
    }
}

const RangeDomain* RangeAnalysis::GetRecordedTerminatorEdgeState(
    const Terminator* terminator, const Block* successor) const
{
    auto terminatorIt = terminatorEdgeStates.find(terminator);
    if (terminatorIt == terminatorEdgeStates.end()) {
        return nullptr;
    }
    auto successorIt = terminatorIt->second.find(successor);
    return successorIt == terminatorIt->second.end() ? nullptr : &successorIt->second;
}

void RangeAnalysis::SeedMutableGlobalInitializers(RangeDomain& state)
{
    auto package = builder.GetCurPackage();
    if (isContextAnalysis || func == nullptr || func->IsGVInit() || package == nullptr) {
        return;
    }

    size_t analyzedInitializers = 0;
    for (auto global : package->GetGlobalVars()) {
        auto baseType = GetTrackedMutableGlobalBaseType(global);
        if (baseType == nullptr) {
            continue;
        }
        auto targetObject = EnsureMutableGlobalValueInitialized(state, global);
        auto init = global->GetInitFunc();
        if (init == nullptr || init->GetBody() == nullptr) {
            continue;
        }
        if (analyzedInitializers >= MAX_CONTEXT_GLOBALS) {
            state.Update(targetObject, /* isTop = */ true);
            continue;
        }
        ++analyzedInitializers;

        auto trackedGlobals = CollectTrackedMutableGlobals(init, package);
        if (!trackedGlobals.complete) {
            state.Update(targetObject, /* isTop = */ true);
            continue;
        }
        if (std::find(trackedGlobals.begin(), trackedGlobals.end(), global) == trackedGlobals.end()) {
            if (trackedGlobals.values.size() >= MAX_CONTEXT_GLOBALS) {
                state.Update(targetObject, /* isTop = */ true);
                continue;
            }
            trackedGlobals.values.emplace_back(global);
            std::sort(trackedGlobals.values.begin(), trackedGlobals.values.end());
        }

        ContextGlobalValues inputGlobals;
        inputGlobals.reserve(trackedGlobals.size());
        for (auto trackedGlobal : trackedGlobals) {
            EnsureMutableGlobalValueInitialized(state, trackedGlobal);
            inputGlobals.emplace_back(
                trackedGlobal, CaptureContextValue(state, trackedGlobal, /* preserveIntervals = */ true));
        }

        std::vector<std::optional<ContextAbstractValue>> refArgValues;
        auto summarizedGlobals = inputGlobals;
        (void)AnalyzeCalleeWithContext(init, {}, refArgValues, summarizedGlobals);
        for (const auto& [summarizedGlobal, value] : summarizedGlobals) {
            auto summarizedType = GetTrackedMutableGlobalBaseType(summarizedGlobal);
            auto object = EnsureMutableGlobalValueInitialized(state, summarizedGlobal);
            if (summarizedType == nullptr || value.IsTop()) {
                state.Update(object, /* isTop = */ true);
                continue;
            }
            ApplyContextValue(state, summarizedGlobal, value);
        }
    }
}

void RangeAnalysis::InitializeFuncEntryState(RangeDomain& state)
{
    ValueAnalysis<RangeValueDomain>::InitializeFuncEntryState(state);
    for (auto global : CollectTrackedMutableGlobals(func, builder.GetCurPackage())) {
        EnsureMutableGlobalValueInitialized(state, global);
    }
    SeedMutableGlobalInitializers(state);
    auto params = func->GetParams();
    auto limit = std::min(params.size(), contextArguments.size());
    std::unordered_map<size_t, AbstractObject*> aliasedParameterObjects;
    std::unordered_map<size_t, AbstractObject*> contextualAliasObjects;
    constexpr size_t MAX_CONTEXT_ALIAS_BIND_DEPTH = 8;
    std::function<void(Value*, Type*, const ContextAbstractValue&, size_t)> bindContextAliases =
        [&](Value* destination, Type* type, const ContextAbstractValue& value, size_t depth) {
            if (destination == nullptr || type == nullptr || value.kind != ContextAbstractValue::Kind::OBJECT ||
                depth >= MAX_CONTEXT_ALIAS_BIND_DEPTH) {
                return;
            }
            if (type->IsRef()) {
                auto object = state.CheckAbstractObjectRefBy(destination);
                if (value.objectIdentity != 0) {
                    auto found = contextualAliasObjects.find(value.objectIdentity);
                    if (found != contextualAliasObjects.end()) {
                        state.SetRefToObject(destination, found->second);
                        object = found->second;
                    } else {
                        if (object == nullptr) {
                            object = state.GetReferencedObjAndSetToTop(destination);
                        }
                        if (object != nullptr) {
                            contextualAliasObjects.emplace(value.objectIdentity, object);
                        }
                    }
                }
                bindContextAliases(
                    object, StaticCast<RefType*>(type)->GetRootBaseType(), value, depth + 1);
                return;
            }

            std::vector<Type*> memberTypes;
            if (type->IsClass() || type->IsStruct()) {
                memberTypes = StaticCast<CustomType*>(type)->GetInstantiatedMemberTys(builder);
            } else if (type->IsTuple()) {
                memberTypes = StaticCast<TupleType*>(type)->GetElementTypes();
            }
            auto children = state.GetChildren(destination);
            if (type->IsRawArray()) {
                memberTypes.assign(children.size(), StaticCast<RawArrayType*>(type)->GetElementType());
            }
            auto count = std::min({children.size(), memberTypes.size(), value.objectFields.size()});
            for (size_t childIndex = 0; childIndex < count; ++childIndex) {
                bindContextAliases(children[childIndex], memberTypes[childIndex],
                    value.objectFields[childIndex], depth + 1);
            }
        };
    for (size_t i = 0; i < limit; ++i) {
        auto param = params[i];
        auto type = param->GetType();
        if (contextArguments[i].kind == ContextAbstractValue::Kind::LAMBDA &&
            contextArguments[i].lambdaValue != nullptr) {
            auto capturedVariables = contextArguments[i].lambdaValue->GetCapturedVariables();
            if (capturedVariables.size() == contextArguments[i].lambdaCapturedValues.size()) {
                for (size_t captureIndex = 0; captureIndex < capturedVariables.size(); ++captureIndex) {
                    ApplyContextValue(state, capturedVariables[captureIndex],
                        contextArguments[i].lambdaCapturedValues[captureIndex]);
                }
            }
            continue;
        }
        bindContextAliases(param, type, contextArguments[i], 0);
        if (type != nullptr && type->IsRef()) {
            auto baseType = GetRefRootBaseType(type);
            auto object = state.CheckAbstractObjectRefBy(param);
            auto aliasGroup = contextArguments[i].aliasGroup;
            if (aliasGroup != 0) {
                auto found = aliasedParameterObjects.find(aliasGroup);
                if (found == aliasedParameterObjects.end()) {
                    if (object != nullptr) {
                        aliasedParameterObjects.emplace(aliasGroup, object);
                    }
                } else {
                    state.SetRefToObject(param, found->second);
                    object = found->second;
                }
            }
            if (object != nullptr && baseType != nullptr && (baseType->IsBoolean() || baseType->IsInteger() ||
                baseType->IsClass() || baseType->IsStruct() || baseType->IsTuple())) {
                ApplyContextValue(state, object, baseType, contextArguments[i]);
            }
            bindContextAliases(param, type, contextArguments[i], 0);
            continue;
        }
        ApplyContextValue(state, param, contextArguments[i]);
        bindContextAliases(param, type, contextArguments[i], 0);
    }
    for (const auto& [global, value] : contextGlobalArguments) {
        auto baseType = GetTrackedMutableGlobalBaseType(global);
        auto object = EnsureMutableGlobalValueInitialized(state, global);
        if (baseType == nullptr || value.IsTop()) {
            state.Update(object, /* isTop = */ true);
            continue;
        }
        ApplyContextValue(state, global, value);
    }
}

bool CanAnalyse(const Ptr<Type>& type)
{
    if (type->IsInteger() || type->IsBoolean()) {
        return true;
    }
    return false;
}

const SIntDomain& GetDefaultIntCache(const Ptr<Type>& ty)
{
    constexpr int integerSize{4};
    static SIntDomain signedRange[integerSize]{
        {ConstantRange::Full(IntWidth::I8), false},
        {ConstantRange::Full(IntWidth::I16), false},
        {ConstantRange::Full(IntWidth::I32), false},
        {ConstantRange::Full(IntWidth::I64), false},
    };
    static SIntDomain unsignedRange[integerSize]{
        {ConstantRange::Full(IntWidth::I8), true},
        {ConstantRange::Full(IntWidth::I16), true},
        {ConstantRange::Full(IntWidth::I32), true},
        {ConstantRange::Full(IntWidth::I64), true},
    };
    auto width{Ctz(static_cast<unsigned>(ToWidth(*ty)) / static_cast<unsigned>(CHAR_BIT))};
    return ty->IsUnsignedInteger() ? unsignedRange[width] : signedRange[width];
}

// 按指定符号语义比较两个 SInt 是否严格小于。
bool StrictlyLess(const SInt& lhs, const SInt& rhs, bool isUnsigned)
{
    return isUnsigned ? lhs.Ult(rhs) : lhs.Slt(rhs);
}

// 按指定符号语义比较两个 SInt 是否严格大于。
bool StrictlyGreater(const SInt& lhs, const SInt& rhs, bool isUnsigned)
{
    return isUnsigned ? lhs.Ugt(rhs) : lhs.Sgt(rhs);
}

std::optional<SInt> FindUpperWideningThreshold(
    const std::vector<SInt>& thresholds, const SInt& bound, bool isUnsigned)
{
    std::optional<SInt> result;
    for (const auto& threshold : thresholds) {
        if (threshold.Width() != bound.Width() || StrictlyLess(threshold, bound, isUnsigned)) {
            continue;
        }
        if (!result.has_value() || StrictlyLess(threshold, result.value(), isUnsigned)) {
            result = threshold;
        }
    }
    return result;
}

std::optional<SInt> FindLowerWideningThreshold(
    const std::vector<SInt>& thresholds, const SInt& bound, bool isUnsigned)
{
    std::optional<SInt> result;
    for (const auto& threshold : thresholds) {
        if (threshold.Width() != bound.Width() || StrictlyGreater(threshold, bound, isUnsigned)) {
            continue;
        }
        if (!result.has_value() || StrictlyGreater(threshold, result.value(), isUnsigned)) {
            result = threshold;
        }
    }
    return result;
}

SIntDomain MakeFiniteWideningRange(const SInt& lower, const SInt& upper, bool isUnsigned)
{
    return SIntDomain::Intersects(
        SIntDomain::FromNumeric(RelationalOperation::GE, lower, isUnsigned),
        SIntDomain::FromNumeric(RelationalOperation::LE, upper, isUnsigned));
}

// 对整数区间执行单边 widening，保证循环不动点收敛。
SIntDomain WidenSIntDomain(const SIntDomain& previous, const SIntDomain& current,
    const std::vector<SInt>& thresholds)
{
    auto width = current.Width();
    auto isUnsigned = current.IsUnsigned();
    if (previous.Width() != width || previous.IsUnsigned() != isUnsigned) {
        return SIntDomain::Top(width, isUnsigned);
    }
    const auto& previousNumeric = previous.NumericBound();
    const auto& currentNumeric = current.NumericBound();
    if (previousNumeric.IsEmptySet()) {
        return current;
    }
    auto joinedNumeric = previousNumeric.UnionWith(currentNumeric, PreferFromBool(isUnsigned));
    if (joinedNumeric.IsFullSet() || joinedNumeric.IsWrappedSet() || joinedNumeric.IsSignWrappedSet() ||
        currentNumeric.IsWrappedSet() || currentNumeric.IsSignWrappedSet() || previousNumeric.IsWrappedSet() ||
        previousNumeric.IsSignWrappedSet()) {
        return SIntDomain::Top(width, isUnsigned);
    }

    auto previousMin = previousNumeric.MinValue(isUnsigned);
    auto previousMax = previousNumeric.MaxValue(isUnsigned);
    auto joinedMin = joinedNumeric.MinValue(isUnsigned);
    auto joinedMax = joinedNumeric.MaxValue(isUnsigned);
    auto lowerMovedDown = StrictlyLess(joinedMin, previousMin, isUnsigned);
    auto upperMovedUp = StrictlyGreater(joinedMax, previousMax, isUnsigned);
    if (lowerMovedDown && upperMovedUp) {
        auto thresholdMin = FindLowerWideningThreshold(thresholds, joinedMin, isUnsigned);
        auto thresholdMax = FindUpperWideningThreshold(thresholds, joinedMax, isUnsigned);
        auto lower = thresholdMin.value_or(
            isUnsigned ? SInt::UMinValue(width) : SInt::SMinValue(width));
        auto upper = thresholdMax.value_or(
            isUnsigned ? SInt::UMaxValue(width) : SInt::SMaxValue(width));
        return MakeFiniteWideningRange(lower, upper, isUnsigned);
    }
    if (upperMovedUp) {
        if (auto threshold = FindUpperWideningThreshold(thresholds, joinedMax, isUnsigned);
            threshold.has_value()) {
            return MakeFiniteWideningRange(joinedMin, threshold.value(), isUnsigned);
        }
        return SIntDomain::FromNumeric(RelationalOperation::GE, joinedMin, isUnsigned);
    }
    if (lowerMovedDown) {
        if (auto threshold = FindLowerWideningThreshold(thresholds, joinedMin, isUnsigned);
            threshold.has_value()) {
            return MakeFiniteWideningRange(threshold.value(), joinedMax, isUnsigned);
        }
        return SIntDomain::FromNumeric(RelationalOperation::LE, joinedMax, isUnsigned);
    }
    return SIntDomain{joinedNumeric, isUnsigned};
}

struct LoopWidenCandidate {
    Value* value;
    Type* type;
    bool preserveDuringBodyWidening;
};

// 判断后继路径是否可以回到候选循环头。
bool CanReachBlockForWidening(const Block* start, const Block* target)
{
    if (start == nullptr || target == nullptr) {
        return false;
    }
    std::vector<const Block*> worklist{start};
    std::unordered_set<const Block*> visited;
    while (!worklist.empty()) {
        auto block = worklist.back();
        worklist.pop_back();
        if (!visited.emplace(block).second) {
            continue;
        }
        if (block == target) {
            return true;
        }
        for (auto successor : block->GetSuccessors()) {
            worklist.emplace_back(successor);
        }
    }
    return false;
}

// 识别适合作为 widening 头部的循环分支块。
bool IsLoopHeaderForWidening(const Block* block)
{
    if (block == nullptr) {
        return false;
    }
    if (auto forest = GetLoopForest(block); forest != nullptr && forest->FindByHeader(block) != nullptr) {
        return true;
    }
    auto terminator = block->GetTerminator();
    if (terminator == nullptr || terminator->GetExprKind() != ExprKind::BRANCH) {
        return false;
    }
    auto branch = StaticCast<const Branch*>(terminator);
    if (CanReachBlockForWidening(branch->GetTrueBlock(), block)) {
        return true;
    }
    return CanReachBlockForWidening(branch->GetFalseBlock(), block);
}

// 判断表达式类型是否是可用作循环 guard 的关系比较。
bool IsRelationalExprKindForWidening(ExprKind kind)
{
    switch (kind) {
        case ExprKind::LT:
        case ExprKind::LE:
        case ExprKind::GT:
        case ExprKind::GE:
        case ExprKind::EQUAL:
        case ExprKind::NOTEQUAL:
            return true;
        default:
            return false;
    }
}

// 获取 widening 逻辑使用的局部值定义表达式。
const Expression* GetDefiningExprForWidening(Value* value)
{
    auto local = DynamicCast<LocalVar*>(value);
    if (local == nullptr) {
        return nullptr;
    }
    return local->GetExpr();
}

std::vector<SInt> CollectLoopWideningThresholds(
    const RangeDomain& state, const Block* block, IntWidth width, bool isUnsigned)
{
    std::vector<SInt> thresholds;
    auto forest = GetLoopForest(block);
    auto loop = forest == nullptr ? nullptr : forest->FindInnermost(block);
    if (loop == nullptr) {
        return thresholds;
    }
    const auto addThreshold = [&](const SInt& threshold) {
        if (threshold.Width() == width) {
            thresholds.emplace_back(threshold);
        }
    };
    const auto getConstant = [&state](Value* value) -> std::optional<SInt> {
        auto expression = GetDefiningExprForWidening(value);
        if (expression == nullptr || expression->GetExprKind() != ExprKind::CONSTANT) {
            // CHIR commonly materializes a loop bound through a load or a
            // same-width cast. It remains a sound threshold when the current
            // abstract state proves that operand is a singleton.
            auto abstractDomain = state.CheckAbstractValueWithTopBottom(value);
            auto abstractValue = abstractDomain == nullptr ? nullptr : abstractDomain->CheckAbsVal();
            if (abstractValue == nullptr ||
                abstractValue->GetRangeKind() != ValueRange::RangeKind::SINT) {
                return std::nullopt;
            }
            const auto& domain = StaticCast<const SIntRange*>(abstractValue)->GetVal();
            return domain.IsSingleValue()
                ? std::optional<SInt>{domain.NumericBound().GetSingleElement()}
                : std::nullopt;
        }
        auto literal = StaticCast<const Constant*>(expression)->GetValue();
        if (literal == nullptr || !literal->IsIntLiteral()) {
            return std::nullopt;
        }
        auto domain = SIntDomain::From(*literal);
        return domain.IsSingleValue()
            ? std::optional<SInt>{domain.NumericBound().GetSingleElement()}
            : std::nullopt;
    };
    for (auto loopBlock : loop->blocks) {
        for (auto expression : loopBlock->GetExpressions()) {
            if (expression == nullptr ||
                !IsRelationalExprKindForWidening(expression->GetExprKind())) {
                continue;
            }
            auto binary = StaticCast<const BinaryExpression*>(expression);
            for (auto operand : {binary->GetLHSOperand(), binary->GetRHSOperand()}) {
                if (operand == nullptr || operand->GetType() == nullptr ||
                    !operand->GetType()->IsInteger() ||
                    operand->GetType()->IsUnsignedInteger() != isUnsigned ||
                    ToWidth(*operand->GetType()) != width) {
                    continue;
                }
                auto constant = getConstant(operand);
                if (!constant.has_value()) {
                    continue;
                }
                addThreshold(constant.value());
                auto minimum = isUnsigned ? SInt::UMinValue(width) : SInt::SMinValue(width);
                auto maximum = isUnsigned ? SInt::UMaxValue(width) : SInt::SMaxValue(width);
                if (StrictlyGreater(constant.value(), minimum, isUnsigned)) {
                    addThreshold(constant.value() - 1U);
                }
                if (StrictlyLess(constant.value(), maximum, isUnsigned)) {
                    addThreshold(constant.value() + 1U);
                }
            }
        }
    }
    std::sort(thresholds.begin(), thresholds.end(), [isUnsigned](const SInt& lhs, const SInt& rhs) {
        return StrictlyLess(lhs, rhs, isUnsigned);
    });
    thresholds.erase(std::unique(thresholds.begin(), thresholds.end()), thresholds.end());
    return thresholds;
}

// 判断某个值是否是循环 guard 直接使用的 load。
bool IsLoopConditionLoadForWidening(Value* value)
{
    auto expr = GetDefiningExprForWidening(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::LOAD) {
        return false;
    }
    auto header = expr->GetParentBlock();
    if (!IsLoopHeaderForWidening(header)) {
        return false;
    }
    auto branch = StaticCast<const Branch*>(header->GetTerminator());
    auto condExpr = GetDefiningExprForWidening(branch->GetCondition());
    if (condExpr == nullptr || condExpr->GetExprMajorKind() != ExprMajorKind::BINARY_EXPR ||
        !IsRelationalExprKindForWidening(condExpr->GetExprKind())) {
        return false;
    }
    auto binary = StaticCast<const BinaryExpression*>(condExpr);
    return binary->GetLHSOperand() == value || binary->GetRHSOperand() == value;
}

// 判断循环体 widening 时是否应保留 guard load 的精度。
bool ShouldPreserveLoopGuardValueDuringBodyWidening(const Block* block, Value* value)
{
    auto expr = GetDefiningExprForWidening(value);
    if (expr == nullptr || expr->GetParentBlock() == nullptr || expr->GetParentBlock() == block) {
        return false;
    }
    return IsLoopConditionLoadForWidening(value);
}

// 从状态中读取整数域，缺失时返回类型全域。
const SIntDomain& GetSIntDomainFromState(const RangeDomain& state, Value* value, Type* type)
{
    CJC_ASSERT(type != nullptr && type->IsInteger());
    auto domain = state.CheckAbstractValueWithTopBottom(value);
    if (domain == nullptr || domain->IsTop()) {
        return GetDefaultIntCache(type);
    }
    auto absVal = domain->CheckAbsVal();
    if (absVal == nullptr || absVal->GetRangeKind() != ValueRange::RangeKind::SINT) {
        return GetDefaultIntCache(type);
    }
    return StaticCast<const SIntRange*>(absVal)->GetVal();
}

BoolDomain GetBoolDomainFromStateWithType(const RangeDomain& state, Value* value, Type* type)
{
    if (value == nullptr || type == nullptr || !type->IsBoolean()) {
        return BoolDomain::Top();
    }
    auto domain = state.CheckAbstractValueWithTopBottom(value);
    if (domain == nullptr || domain->IsTop()) {
        return BoolDomain::Top();
    }
    auto absVal = domain->CheckAbsVal();
    if (absVal == nullptr || absVal->GetRangeKind() != ValueRange::RangeKind::BOOL) {
        return BoolDomain::Top();
    }
    return StaticCast<const BoolRange*>(absVal)->GetVal();
}

// 将带类型的整数值加入循环 widening 候选集。
void AddWidenCandidate(std::vector<LoopWidenCandidate>& candidates, std::unordered_set<Value*>& seen, Value* value,
    Type* type, bool preserveDuringBodyWidening = false)
{
    if (value == nullptr || type == nullptr || !type->IsInteger() || seen.find(value) != seen.end()) {
        return;
    }
    seen.emplace(value);
    candidates.emplace_back(LoopWidenCandidate{value, type, preserveDuringBodyWidening});
}

// 将整数值按自身类型加入循环 widening 候选集。
void AddWidenCandidate(std::vector<LoopWidenCandidate>& candidates, std::unordered_set<Value*>& seen, Value* value,
    bool preserveDuringBodyWidening = false)
{
    AddWidenCandidate(candidates, seen, value, value == nullptr ? nullptr : value->GetType(), preserveDuringBodyWidening);
}

// 将 ref 背后的整数抽象对象加入 widening 候选集。
void AddReferencedIntegerObjectCandidate(
    RangeDomain& state, std::vector<LoopWidenCandidate>& candidates, std::unordered_set<Value*>& seen, Value* value)
{
    if (value == nullptr || !value->GetType()->IsRef()) {
        return;
    }
    auto refType = StaticCast<RefType*>(value->GetType());
    if (!refType->GetRootBaseType()->IsInteger()) {
        return;
    }
    AddWidenCandidate(candidates, seen, state.CheckAbstractObjectRefBy(value), refType->GetRootBaseType());
}

// 收集 block 中可能需要循环 widening 的整数值和 ref 对象。
std::vector<LoopWidenCandidate> CollectLoopWidenCandidates(RangeDomain& state, const Block* block)
{
    std::vector<LoopWidenCandidate> candidates;
    std::unordered_set<Value*> seen;
    for (auto expr : block->GetExpressions()) {
        AddWidenCandidate(candidates, seen, expr->GetResult());
        for (auto operand : expr->GetOperands()) {
            AddWidenCandidate(candidates, seen, operand,
                ShouldPreserveLoopGuardValueDuringBodyWidening(block, operand));
        }
        switch (expr->GetExprKind()) {
            case ExprKind::LOAD: {
                auto load = StaticCast<const Load*>(expr);
                AddReferencedIntegerObjectCandidate(state, candidates, seen, load->GetLocation());
                break;
            }
            case ExprKind::STORE: {
                auto store = StaticCast<const Store*>(expr);
                AddReferencedIntegerObjectCandidate(state, candidates, seen, store->GetLocation());
                break;
            }
            default:
                break;
        }
    }
    return candidates;
}

using WidenValueFlowGraph = std::unordered_map<Value*, std::unordered_set<Value*>>;

Value* GetWidenValueFlowNode(RangeDomain& state, Value* value)
{
    if (value == nullptr || value->GetType() == nullptr) {
        return nullptr;
    }
    if (value->GetType()->IsInteger()) {
        return value;
    }
    if (!value->GetType()->IsRef()) {
        return nullptr;
    }
    auto rootType = StaticCast<RefType*>(value->GetType())->GetRootBaseType();
    return rootType != nullptr && rootType->IsInteger() ? state.CheckAbstractObjectRefBy(value) : nullptr;
}

void AddWidenValueFlowEdge(WidenValueFlowGraph& graph, Value* source, Value* target)
{
    if (source == nullptr || target == nullptr) {
        return;
    }
    graph[source].emplace(target);
    graph.try_emplace(target);
}

bool IsSafeExpressionForSelectiveWidening(const Expression* expression)
{
    // ponytail: keep the first version scalar-only; add alias-aware call and
    // aggregate edges when benchmarks show that fallback coverage is limiting.
    if (expression->GetExprMajorKind() == ExprMajorKind::UNARY_EXPR ||
        expression->GetExprMajorKind() == ExprMajorKind::BINARY_EXPR) {
        return true;
    }
    switch (expression->GetExprKind()) {
        case ExprKind::GOTO:
        case ExprKind::BRANCH:
        case ExprKind::MULTIBRANCH:
        case ExprKind::EXIT:
        case ExprKind::ALLOCATE:
        case ExprKind::LOAD:
        case ExprKind::STORE:
        case ExprKind::CONSTANT:
        case ExprKind::DEBUGEXPR:
        case ExprKind::TYPECAST:
            return true;
        default:
            return false;
    }
}

std::optional<std::unordered_set<Value*>> BuildQueryGuidedWideningSelection(
    RangeDomain& state, const Block* block, const std::vector<LoopWidenCandidate>& candidates)
{
    constexpr size_t MIN_SELECTIVE_WIDENING_GRAPH_NODES = 32;
    constexpr size_t MAX_SELECTIVE_WIDENING_GRAPH_NODES = 4096;
    if (std::getenv("CANGJIE_RA_DISABLE_QGSW") != nullptr || block == nullptr) {
        return std::nullopt;
    }
    size_t functionExpressionCount = 0;
    auto function = block->GetTopLevelFunc();
    if (function == nullptr || function->GetBody() == nullptr) {
        return std::nullopt;
    }
    for (auto functionBlock : function->GetBody()->GetAllBlocks()) {
        functionExpressionCount += functionBlock->GetExpressions().size();
        if (functionExpressionCount >= MIN_SELECTIVE_WIDENING_GRAPH_NODES) {
            break;
        }
    }
    if (functionExpressionCount < MIN_SELECTIVE_WIDENING_GRAPH_NODES) {
        return std::nullopt;
    }
    const bool queryGuided = !queryRefinementBlocks.empty();
    if (queryGuided && queryRefinementBlocks.find(block) == queryRefinementBlocks.end()) {
        return std::nullopt;
    }
    const auto isInScope = [queryGuided](const Block* candidate) {
        return !queryGuided || queryRefinementBlocks.find(candidate) != queryRefinementBlocks.end();
    };

    std::unordered_set<const Block*> loopBlocks;
    if (auto forest = GetLoopForest(block); forest != nullptr) {
        if (auto descriptor = forest->FindByHeader(block); descriptor != nullptr) {
            for (auto candidate : descriptor->blocks) {
                if (isInScope(candidate)) {
                    loopBlocks.emplace(candidate);
                }
            }
        }
    }
    if (loopBlocks.empty()) {
        std::unordered_set<const Block*> forward{block};
        std::vector<const Block*> worklist{block};
        while (!worklist.empty()) {
            auto current = worklist.back();
            worklist.pop_back();
            for (auto successor : current->GetSuccessors()) {
                if (isInScope(successor) && forward.emplace(successor).second) {
                    worklist.emplace_back(successor);
                }
            }
        }
        std::unordered_set<const Block*> backward{block};
        worklist = {block};
        while (!worklist.empty()) {
            auto current = worklist.back();
            worklist.pop_back();
            for (auto predecessor : current->GetPredecessors()) {
                if (isInScope(predecessor) && backward.emplace(predecessor).second) {
                    worklist.emplace_back(predecessor);
                }
            }
        }
        for (auto candidate : forward) {
            if (backward.find(candidate) != backward.end()) {
                loopBlocks.emplace(candidate);
            }
        }
    }

    WidenValueFlowGraph graph;
    std::unordered_set<Value*> storageNodes;
    for (auto current : loopBlocks) {
        for (auto expression : current->GetExpressions()) {
            if (!IsSafeExpressionForSelectiveWidening(expression)) {
                return std::nullopt;
            }
            auto result = GetWidenValueFlowNode(state, expression->GetResult());
            for (auto operand : expression->GetOperands()) {
                AddWidenValueFlowEdge(graph, GetWidenValueFlowNode(state, operand), result);
            }
            if (expression->GetExprKind() == ExprKind::LOAD) {
                auto load = StaticCast<const Load*>(expression);
                auto storage = GetWidenValueFlowNode(state, load->GetLocation());
                AddWidenValueFlowEdge(graph, storage, result);
                storageNodes.emplace(storage);
            } else if (expression->GetExprKind() == ExprKind::STORE) {
                auto store = StaticCast<const Store*>(expression);
                auto storage = GetWidenValueFlowNode(state, store->GetLocation());
                AddWidenValueFlowEdge(graph, GetWidenValueFlowNode(state, store->GetValue()), storage);
                storageNodes.emplace(storage);
            }
            if (graph.size() > MAX_SELECTIVE_WIDENING_GRAPH_NODES) {
                return std::nullopt;
            }
        }
    }
    const auto blockSuccessors = block->GetSuccessors();
    const bool selfLoop = std::find(blockSuccessors.begin(), blockSuccessors.end(), block) != blockSuccessors.end();
    if (loopBlocks.size() < 2 && !selfLoop) {
        return std::nullopt;
    }
    if (graph.size() < MIN_SELECTIVE_WIDENING_GRAPH_NODES) {
        return std::nullopt;
    }

    std::unordered_map<Value*, size_t> indices;
    std::unordered_map<Value*, size_t> lowLinks;
    std::unordered_set<Value*> onStack;
    std::unordered_set<Value*> cyclicValues;
    std::vector<Value*> stack;
    size_t nextIndex = 0;
    std::function<void(Value*)> visit = [&](Value* value) {
        indices.emplace(value, nextIndex);
        lowLinks.emplace(value, nextIndex++);
        stack.emplace_back(value);
        onStack.emplace(value);
        for (auto successor : graph[value]) {
            if (indices.find(successor) == indices.end()) {
                visit(successor);
                lowLinks[value] = std::min(lowLinks[value], lowLinks[successor]);
            } else if (onStack.find(successor) != onStack.end()) {
                lowLinks[value] = std::min(lowLinks[value], indices[successor]);
            }
        }
        if (lowLinks[value] != indices[value]) {
            return;
        }
        std::vector<Value*> component;
        while (!stack.empty()) {
            auto member = stack.back();
            stack.pop_back();
            onStack.erase(member);
            component.emplace_back(member);
            if (member == value) {
                break;
            }
        }
        if (component.size() > 1 ||
            (component.size() == 1 && graph[component.front()].find(component.front()) != graph[component.front()].end())) {
            cyclicValues.insert(component.begin(), component.end());
        }
    };
    for (const auto& [value, _] : graph) {
        if (indices.find(value) == indices.end()) {
            visit(value);
        }
    }

    std::unordered_set<Value*> selected;
    for (const auto& candidate : candidates) {
        if (cyclicValues.find(candidate.value) != cyclicValues.end() &&
            storageNodes.find(candidate.value) != storageNodes.end()) {
            selected.emplace(candidate.value);
        }
    }
    if (selected.empty() || selected.size() * 4 > graph.size()) {
        return std::nullopt;
    }
    return selected;
}

// 在 widening 前捕获当前 block 的整数值域快照。
LoopRangeSnapshot CaptureLoopRangeSnapshot(
    RangeDomain& state, const Block* block, const std::unordered_set<Value*>* selected)
{
    LoopRangeSnapshot snapshot;
    for (auto [value, type, preserveDuringBodyWidening] : CollectLoopWidenCandidates(state, block)) {
        if (selected != nullptr && selected->find(value) == selected->end()) {
            continue;
        }
        auto abstractDomain = state.CheckAbstractValueWithTopBottom(value);
        auto abstractValue = abstractDomain == nullptr ? nullptr : abstractDomain->CheckAbsVal();
        auto currentRange = abstractValue != nullptr &&
                abstractValue->GetRangeKind() == ValueRange::RangeKind::SINT
            ? StaticCast<const SIntRange*>(abstractValue)
            : nullptr;
        auto captured = currentRange != nullptr
            ? currentRange->Clone()
            : std::make_unique<SIntRange>(GetSIntDomainFromState(state, value, type));
        snapshot.emplace(value, LoopRangeSnapshotEntry{
            std::unique_ptr<SIntRange>(StaticCast<SIntRange*>(captured.release())),
            type, preserveDuringBodyWidening});
    }
    return snapshot;
}

// 对重复访问的循环 block 应用候选整数值 widening。
void ApplyLoopWidening(
    RangeDomain& state, const LoopRangeSnapshot& previousRanges, const Block* block)
{
    std::map<std::pair<IntWidth, bool>, std::vector<SInt>> thresholdCache;
    for (const auto& [value, entry] : previousRanges) {
        if (entry.preserveDuringBodyWidening) {
            continue;
        }
        const auto& current = GetSIntDomainFromState(state, value, entry.type);
        auto abstractDomain = state.CheckAbstractValueWithTopBottom(value);
        auto abstractValue = abstractDomain == nullptr ? nullptr : abstractDomain->CheckAbsVal();
        auto currentRange = abstractValue != nullptr &&
                abstractValue->GetRangeKind() == ValueRange::RangeKind::SINT
            ? StaticCast<const SIntRange*>(abstractValue)
            : nullptr;
        auto thresholdKey = std::make_pair(current.Width(), current.IsUnsigned());
        auto foundThresholds = thresholdCache.find(thresholdKey);
        if (foundThresholds == thresholdCache.end()) {
            foundThresholds = thresholdCache.emplace(thresholdKey,
                CollectLoopWideningThresholds(
                    state, block, current.Width(), current.IsUnsigned())).first;
        }
        auto widened = WidenSIntDomain(
            entry.range->GetVal(), current, foundThresholds->second);
        auto widenedCongruence = MergeSIntCongruencesForUnion(entry.range->GetVal(),
            entry.range->GetExactValues(), entry.range->GetCongruence(), current,
            currentRange == nullptr ? std::nullopt : currentRange->GetExactValues(),
            currentRange == nullptr ? std::nullopt : currentRange->GetCongruence());
        auto previousKnownBits = entry.range->GetKnownBits();
        auto currentKnownBits = currentRange == nullptr
            ? std::optional<SIntKnownBits>{}
            : currentRange->GetKnownBits();
        if (previousKnownBits.has_value() || currentKnownBits.has_value()) {
            previousKnownBits = GetEffectiveKnownBits(entry.range->GetVal(),
                entry.range->GetExactValues(), entry.range->GetCongruence(), previousKnownBits);
            if (currentRange != nullptr) {
                currentKnownBits = GetEffectiveKnownBits(current, currentRange->GetExactValues(),
                    currentRange->GetCongruence(), currentKnownBits);
            }
        }
        auto widenedKnownBits = MergeSIntKnownBitsForUnion(
            entry.range->GetVal(), previousKnownBits, current, currentKnownBits);
        auto widenedExcludedValues = MergeExcludedSIntValuesForUnion(entry.range->GetVal(),
            entry.range->GetExcludedValues(), current,
            currentRange == nullptr ? std::nullopt : currentRange->GetExcludedValues(), widened);
        const bool droppedSymbolics = widened.SymbolicBounds().Empty() && !current.SymbolicBounds().Empty();
        if (!widened.IsSame(current) || droppedSymbolics ||
            (currentRange != nullptr && (currentRange->GetCongruence() != widenedCongruence ||
                currentRange->GetKnownBits() != widenedKnownBits ||
                currentRange->GetExcludedValues() != widenedExcludedValues))) {
            state.Update(value, std::make_unique<SIntRange>(
                std::move(widened), std::nullopt, std::move(widenedCongruence), std::move(widenedKnownBits),
                std::move(widenedExcludedValues)));
        }
    }
}

inline bool IsBasicArithmeticKind(ExprKind kind)
{
    return (kind >= ExprKind::ADD && kind <= ExprKind::MOD) || kind == ExprKind::EXP;
}

inline bool IsBasicBinaryExpr(const Expression& expr)
{
    return IsBasicArithmeticKind(expr.GetExprKind());
}

// 判断是否是整数位运算二元表达式。
inline bool IsBitwiseBinaryExpr(ExprKind kind)
{
    return kind == ExprKind::BITAND || kind == ExprKind::BITOR || kind == ExprKind::BITXOR;
}

// 判断是否是整数移位二元表达式。
inline bool IsShiftBinaryExpr(ExprKind kind)
{
    return kind == ExprKind::LSHIFT || kind == ExprKind::RSHIFT;
}

// 判断是否是布尔逻辑二元表达式。
inline bool IsLogicalBinaryExpr(ExprKind kind)
{
    return kind == ExprKind::AND || kind == ExprKind::OR;
}

// 根据闭区间端点构造 ConstantRange。
ConstantRange RangeFromMinMax(const SInt& min, const SInt& max, bool isUnsigned)
{
    auto lower = ConstantRange::From(RelationalOperation::GE, min, !isUnsigned);
    auto upper = ConstantRange::From(RelationalOperation::LE, max, !isUnsigned);
    return lower.IntersectWith(upper, PreferFromBool(isUnsigned));
}

// 根据溢出策略计算整数一元负号的值域。
SIntDomain ComputeNegRange(const SIntDomain& operand, const Ptr<Type>& type, OverflowStrategy ov)
{
    auto width = ToWidth(*type);
    auto isUnsigned = type->IsUnsignedInteger();
    if (operand.IsBottom()) {
        return SIntDomain::Bottom(width, isUnsigned);
    }
    if (operand.IsSingleValue()) {
        auto value = operand.NumericBound().GetSingleElement();
        if (isUnsigned) {
            uint64_t res = 0;
            auto isOverflow =
                OverflowChecker::IsUIntOverflow(type->GetTypeKind(), ExprKind::NEG, 0, value.UVal(), ov, &res);
            if (isOverflow && ov == OverflowStrategy::THROWING) {
                return SIntDomain::Top(width, true);
            }
            return {ConstantRange{SInt{width, res}}, true};
        }
        int64_t res = 0;
        auto isOverflow =
            OverflowChecker::IsIntOverflow(type->GetTypeKind(), ExprKind::NEG, 0, value.SVal(), ov, &res);
        if (isOverflow && ov == OverflowStrategy::THROWING) {
            return SIntDomain::Top(width, false);
        }
        return {ConstantRange{SInt{width, static_cast<uint64_t>(res)}}, false};
    }
    if (ov == OverflowStrategy::SATURATING || ov == OverflowStrategy::CHECKED) {
        return SIntDomain::Top(width, isUnsigned);
    }
    return {operand.NumericBound().Negate(), isUnsigned};
}

// 利用 ~x == -x - 1 计算按位取反值域。
SIntDomain ComputeBitNotRange(const SIntDomain& operand, bool isUnsigned)
{
    auto width = operand.Width();
    if (operand.IsBottom()) {
        return SIntDomain::Bottom(width, isUnsigned);
    }
    return {operand.NumericBound().Negate().Subtract(SInt{width, 1u}), isUnsigned};
}

// 从右操作数值域中提取合法常量移位量。
std::optional<unsigned> GetShiftAmount(const SInt& value, bool isUnsigned, IntWidth lhsWidth)
{
    if (!isUnsigned && value.Slt(0)) {
        return std::nullopt;
    }
    auto amount = value.UVal();
    if (amount >= static_cast<unsigned>(lhsWidth)) {
        return std::nullopt;
    }
    return static_cast<unsigned>(amount);
}

std::optional<unsigned> GetShiftAmount(const SIntDomain& range, bool isUnsigned, IntWidth lhsWidth)
{
    if (!range.IsSingleValue()) {
        return std::nullopt;
    }
    return GetShiftAmount(range.NumericBound().GetSingleElement(), isUnsigned, lhsWidth);
}

std::optional<SInt> ApplyExactShift(
    ExprKind kind, const SInt& lhs, const SInt& rhs, bool rhsUnsigned, bool destUnsigned)
{
    auto amount = GetShiftAmount(rhs, rhsUnsigned, lhs.Width());
    if (!amount.has_value()) {
        return std::nullopt;
    }
    if (kind == ExprKind::LSHIFT) {
        return lhs.Shl(*amount);
    }
    return destUnsigned ? lhs.LShr(*amount) : lhs.Ashr(*amount);
}

// 对单点操作数精确计算位运算或移位结果。
SInt ApplyExactBitwise(ExprKind kind, const SInt& lhs, const SInt& rhs, bool isUnsigned)
{
    switch (kind) {
        case ExprKind::BITAND: {
            auto res = lhs;
            res &= rhs;
            return res;
        }
        case ExprKind::BITOR: {
            auto res = lhs;
            res |= rhs;
            return res;
        }
        case ExprKind::BITXOR: {
            auto res = lhs;
            res ^= rhs;
            return res;
        }
        case ExprKind::LSHIFT:
            return lhs.Shl(rhs);
        case ExprKind::RSHIFT:
            return isUnsigned ? lhs.LShr(rhs) : lhs.Ashr(rhs);
        default:
            CJC_ABORT();
            return lhs;
    }
}

// 尝试为常量移位量的移位表达式计算 sound 区间。
std::optional<SIntDomain> TryComputeShiftRange(
    ExprKind kind, const SIntDomain& lhs, const SIntDomain& rhs, bool rhsUnsigned, bool destUnsigned)
{
    auto width = lhs.Width();
    auto amount = GetShiftAmount(rhs, rhsUnsigned, width);
    if (!amount) {
        return SIntDomain::Top(width, destUnsigned);
    }
    if (lhs.IsBottom()) {
        return SIntDomain::Bottom(width, destUnsigned);
    }
    if (lhs.IsSingleValue()) {
        return SIntDomain{ConstantRange{ApplyExactBitwise(kind, lhs.NumericBound().GetSingleElement(),
                                          rhs.NumericBound().GetSingleElement(), destUnsigned)},
            destUnsigned};
    }
    if (*amount == 0U) {
        return SIntDomain{lhs.NumericBound(), destUnsigned};
    }

    auto& range = lhs.NumericBound();
    if (kind == ExprKind::LSHIFT) {
        if (destUnsigned) {
            if (range.IsWrappedSet()) {
                return std::nullopt;
            }
            auto min = range.UMinValue();
            auto max = range.UMaxValue();
            auto maxBeforeShift = SInt::UMaxValue(width).LShr(*amount);
            if (max.Ugt(maxBeforeShift)) {
                return std::nullopt;
            }
            return SIntDomain{RangeFromMinMax(min.Shl(*amount), max.Shl(*amount), true), true};
        }
        if (range.IsSignWrappedSet()) {
            return std::nullopt;
        }
        auto min = range.SMinValue();
        auto max = range.SMaxValue();
        auto maxBeforeShift = SInt::SMaxValue(width).LShr(*amount);
        if (min.Slt(0) || max.Sgt(maxBeforeShift)) {
            return std::nullopt;
        }
        return SIntDomain{RangeFromMinMax(min.Shl(*amount), max.Shl(*amount), false), false};
    }

    if (destUnsigned) {
        if (range.IsWrappedSet()) {
            return std::nullopt;
        }
        return SIntDomain{
            RangeFromMinMax(range.UMinValue().LShr(*amount), range.UMaxValue().LShr(*amount), true), true};
    }
    if (range.IsSignWrappedSet()) {
        return std::nullopt;
    }
    return SIntDomain{
        RangeFromMinMax(range.SMinValue().AShr(*amount), range.SMaxValue().AShr(*amount), false), false};
}

// 利用 mask 规则尝试收窄 x & mask 的值域。
std::optional<SIntDomain> TryComputeBitAndWithMask(const SIntDomain& value, const SInt& mask, bool destUnsigned)
{
    auto width = value.Width();
    if (mask.IsZero()) {
        return SIntDomain{ConstantRange{SInt::Zero(width)}, destUnsigned};
    }
    if (mask.IsAllOnes()) {
        return SIntDomain{value.NumericBound(), destUnsigned};
    }
    if (destUnsigned || !mask.IsSignBitSet()) {
        return SIntDomain{RangeFromMinMax(SInt::Zero(width), mask, destUnsigned), destUnsigned};
    }
    return std::nullopt;
}

// 利用 mask 规则尝试收窄 x | mask 的值域。
std::optional<SIntDomain> TryComputeBitOrWithMask(const SIntDomain& value, const SInt& mask, bool destUnsigned)
{
    auto width = value.Width();
    if (mask.IsZero()) {
        return SIntDomain{value.NumericBound(), destUnsigned};
    }
    auto& range = value.NumericBound();
    if (destUnsigned) {
        return SIntDomain{RangeFromMinMax(mask, SInt::UMaxValue(width), true), true};
    }
    if (mask.IsSignBitSet()) {
        return SIntDomain{RangeFromMinMax(mask, SInt::AllOnes(width), false), false};
    }
    // 对已知非负的有符号值，保守推导 x | mask 的非负结果区间。
    if (!range.IsSignWrappedSet() && range.SMinValue().Sge(0)) {
        return SIntDomain{RangeFromMinMax(mask, SInt::SMaxValue(width), false), false};
    }
    return std::nullopt;
}

// 尝试为位运算和移位表达式计算保守整数区间。
std::optional<SIntDomain> TryComputeBitwiseRange(ExprKind kind, const SIntDomain& lhs, const SIntDomain& rhs,
    Value* lhsValue, Value* rhsValue, bool rhsUnsigned, bool destUnsigned)
{
    auto width = lhs.Width();
    if (lhs.IsBottom() || rhs.IsBottom()) {
        return SIntDomain::Bottom(width, destUnsigned);
    }
    if (IsShiftBinaryExpr(kind)) {
        return TryComputeShiftRange(kind, lhs, rhs, rhsUnsigned, destUnsigned);
    }
    if (lhs.IsSingleValue() && rhs.IsSingleValue()) {
        return SIntDomain{ConstantRange{ApplyExactBitwise(
                              kind, lhs.NumericBound().GetSingleElement(), rhs.NumericBound().GetSingleElement(),
                              destUnsigned)},
            destUnsigned};
    }
    if (lhsValue == rhsValue) {
        if (kind == ExprKind::BITXOR) {
            return SIntDomain{ConstantRange{SInt::Zero(width)}, destUnsigned};
        }
        return SIntDomain{lhs.NumericBound(), destUnsigned};
    }
    if (rhs.IsSingleValue()) {
        auto mask = rhs.NumericBound().GetSingleElement();
        if (kind == ExprKind::BITAND) {
            return TryComputeBitAndWithMask(lhs, mask, destUnsigned);
        }
        if (kind == ExprKind::BITOR) {
            return TryComputeBitOrWithMask(lhs, mask, destUnsigned);
        }
        if (kind == ExprKind::BITXOR && mask.IsZero()) {
            return SIntDomain{lhs.NumericBound(), destUnsigned};
        }
    }
    if (lhs.IsSingleValue()) {
        auto mask = lhs.NumericBound().GetSingleElement();
        if (kind == ExprKind::BITAND) {
            return TryComputeBitAndWithMask(rhs, mask, destUnsigned);
        }
        if (kind == ExprKind::BITOR) {
            return TryComputeBitOrWithMask(rhs, mask, destUnsigned);
        }
        if (kind == ExprKind::BITXOR && mask.IsZero()) {
            return SIntDomain{rhs.NumericBound(), destUnsigned};
        }
    }
    return std::nullopt;
}

std::optional<SIntKnownBits> GetTransferKnownBits(
    const SIntRange* range, const SIntDomain& domain)
{
    if (range != nullptr) {
        return GetEffectiveKnownBits(domain, range->GetExactValues(),
            range->GetCongruence(), range->GetKnownBits());
    }
    return InferKnownBitsFromInterval(domain);
}

std::optional<SIntKnownBits> ComputeBitwiseKnownBits(ExprKind kind, const SIntRange* lhsRange,
    const SIntRange* rhsRange, const SIntDomain& lhs, const SIntDomain& rhs,
    bool rhsUnsigned, bool destUnsigned)
{
    auto width = lhs.Width();
    auto mask = GetSIntWidthMask(width);
    auto lhsBits = GetTransferKnownBits(lhsRange, lhs).value_or(SIntKnownBits{});
    auto rhsBits = GetTransferKnownBits(rhsRange, rhs).value_or(SIntKnownBits{});
    SIntKnownBits result;
    switch (kind) {
        case ExprKind::BITAND:
            result.knownZero = lhsBits.knownZero | rhsBits.knownZero;
            result.knownOne = lhsBits.knownOne & rhsBits.knownOne;
            break;
        case ExprKind::BITOR:
            result.knownZero = lhsBits.knownZero & rhsBits.knownZero;
            result.knownOne = lhsBits.knownOne | rhsBits.knownOne;
            break;
        case ExprKind::BITXOR:
            result.knownZero = (lhsBits.knownZero & rhsBits.knownZero) |
                (lhsBits.knownOne & rhsBits.knownOne);
            result.knownOne = (lhsBits.knownZero & rhsBits.knownOne) |
                (lhsBits.knownOne & rhsBits.knownZero);
            break;
        case ExprKind::LSHIFT:
        case ExprKind::RSHIFT: {
            auto amount = GetShiftAmount(rhs, rhsUnsigned, width);
            if (!amount.has_value()) {
                return std::nullopt;
            }
            if (*amount == 0) {
                return NormalizeKnownBits(lhsBits, width);
            }
            if (kind == ExprKind::LSHIFT) {
                auto lowZeros = (uint64_t{1} << *amount) - 1U;
                result.knownZero = ((lhsBits.knownZero << *amount) | lowZeros) & mask;
                result.knownOne = (lhsBits.knownOne << *amount) & mask;
                break;
            }
            auto remaining = static_cast<unsigned>(width) - *amount;
            auto retainedMask = remaining == 64 ? UINT64_MAX : ((uint64_t{1} << remaining) - 1U);
            auto highMask = mask & ~retainedMask;
            result.knownZero = lhsBits.knownZero >> *amount;
            result.knownOne = lhsBits.knownOne >> *amount;
            if (destUnsigned) {
                result.knownZero |= highMask;
                break;
            }
            auto signBit = uint64_t{1} << (static_cast<unsigned>(width) - 1U);
            if ((lhsBits.knownZero & signBit) != 0) {
                result.knownZero |= highMask;
            } else if ((lhsBits.knownOne & signBit) != 0) {
                result.knownOne |= highMask;
            }
            break;
        }
        default:
            return std::nullopt;
    }
    return NormalizeKnownBits(result, width);
}

std::optional<SIntRange> TryComputeAbstractBitwiseRange(ExprKind kind,
    const SIntRange* lhsRange, const SIntRange* rhsRange, const SIntDomain& lhs,
    const SIntDomain& rhs, Value* lhsValue, Value* rhsValue, bool rhsUnsigned,
    bool destUnsigned)
{
    auto interval = TryComputeBitwiseRange(
        kind, lhs, rhs, lhsValue, rhsValue, rhsUnsigned, destUnsigned);
    auto knownBits = ComputeBitwiseKnownBits(
        kind, lhsRange, rhsRange, lhs, rhs, rhsUnsigned, destUnsigned);
    if (!knownBits.has_value()) {
        if (!interval.has_value() || !interval->IsNonTrivial()) {
            return std::nullopt;
        }
        return SIntRange{std::move(*interval)};
    }
    auto domain = interval.has_value()
        ? SIntDomain::Intersects(*interval,
              DomainFromKnownBits(knownBits, lhs.Width(), destUnsigned))
        : DomainFromKnownBits(knownBits, lhs.Width(), destUnsigned);
    return SIntRange{
        std::move(domain), std::nullopt, std::nullopt, std::move(knownBits)};
}

SIntRange ComputeBitNotSIntRange(
    const SIntRange* operandRange, const SIntDomain& operand, bool destUnsigned)
{
    auto domain = ComputeBitNotRange(operand, destUnsigned);
    auto sourceBits = GetTransferKnownBits(operandRange, operand);
    if (!sourceBits.has_value()) {
        return SIntRange{std::move(domain)};
    }
    auto knownBits = NormalizeKnownBits(
        SIntKnownBits{sourceBits->knownOne, sourceBits->knownZero}, operand.Width());
    return SIntRange{
        std::move(domain), std::nullopt, std::nullopt, std::move(knownBits)};
}

template <> const std::string Analysis<RangeDomain>::name = "range-analysis";
template <> const std::optional<unsigned> Analysis<RangeDomain>::blockLimit = 2048;
template <> RangeDomain::ChildrenMap ValueAnalysis<RangeValueDomain>::globalChildrenMap{};
template <> RangeDomain::AllocatedRefMap ValueAnalysis<RangeValueDomain>::globalAllocatedRefMap{};
template <> RangeDomain::AllocatedObjMap ValueAnalysis<RangeValueDomain>::globalAllocatedObjMap{};
template <> std::vector<std::unique_ptr<Ref>> ValueAnalysis<RangeValueDomain>::globalRefPool{};
template <> std::vector<std::unique_ptr<AbstractObject>> ValueAnalysis<RangeValueDomain>::globalAbsObjPool{};
template <>
RangeDomain ValueAnalysis<RangeValueDomain>::globalState{&globalChildrenMap, &globalAllocatedRefMap,
    nullptr, &globalAllocatedObjMap, &globalRefPool, &globalAbsObjPool};

BoolDomain RangeAnalysis::GetBoolDomainFromState(const RangeDomain& state, const Ptr<Value>& value)
{
    if (!value->GetType()->IsBoolean()) {
        return BoolDomain::Top();
    }
    auto domain = state.CheckAbstractValueWithTopBottom(value);
    if (domain == nullptr || domain->IsTop()) {
        return BoolDomain::Top();
    }
    auto absVal = domain->CheckAbsVal();
    if (absVal == nullptr || absVal->GetRangeKind() != ValueRange::RangeKind::BOOL) {
        return BoolDomain::Top();
    }
    return StaticCast<const BoolRange*>(absVal)->GetVal();
}

// 从分析状态读取整数值域，未知时返回该类型的完整默认区间。
const SIntDomain& RangeAnalysis::GetSIntDomainFromState(const RangeDomain& state, const Ptr<Value>& value)
{
    CJC_ASSERT(value->GetType()->IsInteger());
    auto domain = state.CheckAbstractValueWithTopBottom(value);
    if (domain == nullptr || domain->IsTop()) {
        return GetDefaultIntCache(value->GetType());
    }
    auto absVal = domain->CheckAbsVal();
    if (absVal == nullptr || absVal->GetRangeKind() != ValueRange::RangeKind::SINT) {
        return GetDefaultIntCache(value->GetType());
    }
    return StaticCast<const SIntRange*>(absVal)->GetVal();
}

const SIntRange* GetSIntRangeFromState(
    const RangeDomain& state, const Ptr<Value>& value, Type* explicitType = nullptr)
{
    if (value == nullptr) {
        return nullptr;
    }
    auto type = explicitType != nullptr ? explicitType : value->GetType();
    if (type == nullptr || !type->IsInteger()) {
        return nullptr;
    }
    auto domain = state.CheckAbstractValueWithTopBottom(value);
    if (domain == nullptr || domain->IsTop()) {
        return nullptr;
    }
    auto absVal = domain->CheckAbsVal();
    if (absVal == nullptr || absVal->GetRangeKind() != ValueRange::RangeKind::SINT) {
        return nullptr;
    }
    return StaticCast<const SIntRange*>(absVal);
}

std::optional<std::vector<SInt>> GetExactValuesOrSmallRange(const SIntRange* range, bool asUnsigned)
{
    if (range == nullptr) {
        return std::nullopt;
    }
    if (range->GetExactValues().has_value()) {
        return range->GetExactValues();
    }
    const auto& domain = range->GetVal();
    if (domain.IsBottom() || !domain.SymbolicBounds().Empty()) {
        return std::nullopt;
    }
    const auto& numeric = domain.NumericBound();
    if (numeric.IsEmptySet() || (numeric.IsFullSet() && !range->GetCongruence().has_value())) {
        return std::nullopt;
    }
    auto width = domain.Width();
    auto stride = range->GetCongruence().has_value() ? range->GetCongruence()->stride : 1U;
    auto residue = range->GetCongruence().has_value() ? range->GetCongruence()->residue : 0U;
    std::vector<SInt> values;
    if (asUnsigned) {
        if (!numeric.IsFullSet() && numeric.IsWrappedSet()) {
            return std::nullopt;
        }
        auto min = numeric.IsFullSet() ? SInt::UMinValue(width).UVal() : numeric.UMinValue().UVal();
        auto max = numeric.IsFullSet() ? SInt::UMaxValue(width).UVal() : numeric.UMaxValue().UVal();
        if (max < min) {
            return std::nullopt;
        }
        auto wideMin = static_cast<unsigned __int128>(min);
        auto wideMax = static_cast<unsigned __int128>(max);
        auto first = wideMin;
        if (stride > 1) {
            auto minResidue = static_cast<uint64_t>(wideMin % stride);
            first += (static_cast<unsigned __int128>(residue) + stride - minResidue) % stride;
        }
        if (first > wideMax) {
            return std::nullopt;
        }
        auto count = (wideMax - first) / stride + 1U;
        if (count == 0 || ExceedsExactIntSetLimit(count)) {
            return std::nullopt;
        }
        values.reserve(static_cast<size_t>(count));
        for (unsigned __int128 value = first; value <= wideMax; value += stride) {
            values.emplace_back(width, static_cast<uint64_t>(value));
        }
    } else {
        if (!numeric.IsFullSet() && numeric.IsSignWrappedSet()) {
            return std::nullopt;
        }
        auto min = numeric.IsFullSet() ? SInt::SMinValue(width).SVal() : numeric.SMinValue().SVal();
        auto max = numeric.IsFullSet() ? SInt::SMaxValue(width).SVal() : numeric.SMaxValue().SVal();
        if (max < min) {
            return std::nullopt;
        }
        auto first = static_cast<__int128>(min);
        if (stride > 1) {
            auto minResidue = first % static_cast<__int128>(stride);
            if (minResidue < 0) {
                minResidue += stride;
            }
            first += (static_cast<__int128>(residue) - minResidue + stride) % stride;
        }
        auto wideMax = static_cast<__int128>(max);
        if (first > wideMax) {
            return std::nullopt;
        }
        auto count = (wideMax - first) / stride + 1;
        if (count <= 0 || ExceedsExactIntSetLimit(count)) {
            return std::nullopt;
        }
        values.reserve(static_cast<size_t>(count));
        for (__int128 value = first; value <= wideMax; value += stride) {
            values.emplace_back(width, static_cast<uint64_t>(static_cast<int64_t>(value)));
        }
    }
    return NormalizeExactIntSet(std::move(values));
}

std::optional<SIntRange> TryComputeExactBitwiseRange(
    ExprKind kind, const std::optional<std::vector<SInt>>& lhsValues,
    const std::optional<std::vector<SInt>>& rhsValues, bool rhsUnsigned, bool destUnsigned)
{
    if ((!IsBitwiseBinaryExpr(kind) && !IsShiftBinaryExpr(kind)) || !lhsValues.has_value() ||
        !rhsValues.has_value()) {
        return std::nullopt;
    }
    if (ExceedsExactIntSetProduct(lhsValues->size(), rhsValues->size())) {
        return std::nullopt;
    }
    std::vector<SInt> values;
    values.reserve(lhsValues->size() * rhsValues->size());
    for (const auto& lhsValue : *lhsValues) {
        for (const auto& rhsValue : *rhsValues) {
            if (IsShiftBinaryExpr(kind)) {
                auto value = ApplyExactShift(kind, lhsValue, rhsValue, rhsUnsigned, destUnsigned);
                if (!value.has_value()) {
                    return std::nullopt;
                }
                values.emplace_back(value.value());
                continue;
            }
            values.emplace_back(ApplyExactBitwise(kind, lhsValue, rhsValue, destUnsigned));
        }
    }
    auto exactValues = NormalizeExactIntSet(std::move(values));
    if (!exactValues.has_value()) {
        return std::nullopt;
    }
    auto domain = DomainFromExactIntValues(*exactValues, destUnsigned);
    return SIntRange{std::move(domain), std::move(exactValues)};
}

std::optional<SInt> ApplyExactUnsignedPower(
    const SInt& lhs, const SInt& rhs, OverflowStrategy ov)
{
    auto width = lhs.Width();
    uint64_t mask = SInt::UMaxValue(width).UVal();
    uint64_t result = 1ULL & mask;
    uint64_t base = lhs.UVal() & mask;
    uint64_t exponent = rhs.UVal();
    bool overflow = false;
    const auto multiply = [mask](uint64_t a, uint64_t b, uint64_t& out) {
        auto product = static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
        out = static_cast<uint64_t>(product) & mask;
        return product > static_cast<unsigned __int128>(mask);
    };
    while (exponent != 0) {
        if ((exponent & 1U) != 0) {
            overflow = multiply(result, base, result) || overflow;
        }
        exponent >>= 1U;
        if (exponent != 0) {
            overflow = multiply(base, base, base) || overflow;
        }
    }
    if (overflow && ov == OverflowStrategy::THROWING) {
        return std::nullopt;
    }
    if (overflow && ov == OverflowStrategy::SATURATING) {
        return SInt::UMaxValue(width);
    }
    return SInt{width, result};
}

std::optional<SInt> ApplyExactSignedPower(const SInt& lhs, const SInt& rhs, OverflowStrategy ov)
{
    auto width = lhs.Width();
    uint64_t exponent = rhs.UVal();
    if (ov == OverflowStrategy::WRAPPING) {
        uint64_t mask = SInt::UMaxValue(width).UVal();
        uint64_t result = 1ULL & mask;
        uint64_t base = lhs.UVal() & mask;
        while (exponent != 0) {
            if ((exponent & 1U) != 0) {
                result = static_cast<uint64_t>(
                    static_cast<unsigned __int128>(result) * static_cast<unsigned __int128>(base)) & mask;
            }
            exponent >>= 1U;
            if (exponent != 0) {
                base = static_cast<uint64_t>(
                    static_cast<unsigned __int128>(base) * static_cast<unsigned __int128>(base)) & mask;
            }
        }
        return SInt{width, result};
    }

    auto lower = static_cast<__int128>(SInt::SMinValue(width).SVal());
    auto upper = static_cast<__int128>(SInt::SMaxValue(width).SVal());
    auto result = static_cast<__int128>(1);
    auto base = static_cast<__int128>(lhs.SVal());
    auto originalExponent = exponent;
    bool overflow = false;
    const auto multiply = [lower, upper](__int128 a, __int128 b, __int128& out) {
        auto product = a * b;
        if (product < lower || product > upper) {
            return true;
        }
        out = product;
        return false;
    };
    while (exponent != 0) {
        if ((exponent & 1U) != 0 && multiply(result, base, result)) {
            overflow = true;
            break;
        }
        exponent >>= 1U;
        if (exponent != 0 && multiply(base, base, base)) {
            overflow = true;
            break;
        }
    }
    if (!overflow) {
        return SInt{width, static_cast<uint64_t>(static_cast<int64_t>(result))};
    }
    if (ov == OverflowStrategy::THROWING) {
        return std::nullopt;
    }
    if (ov == OverflowStrategy::SATURATING) {
        if (lhs.Slt(0) && (originalExponent % 2U) == 1U) {
            return SInt::SMinValue(width);
        }
        return SInt::SMaxValue(width);
    }
    return std::nullopt;
}

std::optional<SInt> ApplyExactPower(
    const SInt& lhs, const SInt& rhs, const Ptr<Type>& type, OverflowStrategy ov, bool isUnsigned, bool rhsUnsigned)
{
    if (!rhsUnsigned && rhs.Slt(0)) {
        return std::nullopt;
    }
    if (isUnsigned) {
        return ApplyExactUnsignedPower(lhs, rhs, ov);
    }
    (void)type;
    return ApplyExactSignedPower(lhs, rhs, ov);
}

std::optional<SInt> ApplyExactArithmetic(ExprKind kind, const SInt& lhs, const SInt& rhs,
    const Ptr<Type>& type, OverflowStrategy ov, bool isUnsigned, bool rhsUnsigned)
{
    auto width = lhs.Width();
    if (kind == ExprKind::EXP) {
        return ApplyExactPower(lhs, rhs, type, ov, isUnsigned, rhsUnsigned);
    }
    if (isUnsigned) {
        auto lhsValue = lhs.UVal();
        auto rhsValue = rhs.UVal();
        uint64_t result = 0;
        if ((kind == ExprKind::DIV || kind == ExprKind::MOD) && rhsValue == 0) {
            return std::nullopt;
        }
        auto overflow = OverflowChecker::IsUIntOverflow(type->GetTypeKind(), kind, lhsValue, rhsValue, ov, &result);
        if (overflow && ov == OverflowStrategy::THROWING) {
            return std::nullopt;
        }
        return SInt{width, result};
    }

    auto lhsValue = lhs.SVal();
    auto rhsValue = rhs.SVal();
    int64_t result = 0;
    if ((kind == ExprKind::DIV || kind == ExprKind::MOD) && rhsValue == 0) {
        return std::nullopt;
    }
    auto overflow = OverflowChecker::IsIntOverflow(type->GetTypeKind(), kind, lhsValue, rhsValue, ov, &result);
    if (overflow && ov == OverflowStrategy::THROWING) {
        return std::nullopt;
    }
    return SInt{width, static_cast<uint64_t>(result)};
}

std::optional<SInt> ApplyExactNeg(const SInt& value, const Ptr<Type>& type, OverflowStrategy ov, bool isUnsigned)
{
    auto width = value.Width();
    if (isUnsigned) {
        uint64_t result = 0;
        auto overflow = OverflowChecker::IsUIntOverflow(type->GetTypeKind(), ExprKind::NEG, 0, value.UVal(), ov, &result);
        if (overflow && ov == OverflowStrategy::THROWING) {
            return std::nullopt;
        }
        return SInt{width, result};
    }
    int64_t result = 0;
    auto overflow = OverflowChecker::IsIntOverflow(type->GetTypeKind(), ExprKind::NEG, 0, value.SVal(), ov, &result);
    if (overflow && ov == OverflowStrategy::THROWING) {
        return std::nullopt;
    }
    return SInt{width, static_cast<uint64_t>(result)};
}

std::optional<SInt> ApplyExactTypeCast(const SInt& value, Type* srcType, Type* destType, OverflowStrategy ov)
{
    if (srcType == nullptr || destType == nullptr || !srcType->IsInteger() || !destType->IsInteger()) {
        return std::nullopt;
    }
    SIntDomain sourceDomain{ConstantRange{value}, srcType->IsUnsignedInteger()};
    auto numeric = ComputeTypeCastNumericBound(
        sourceDomain, ToWidth(*destType), destType->IsUnsignedInteger(), ov);
    if (numeric.IsEmptySet() || !numeric.IsSingleElement()) {
        return std::nullopt;
    }
    return numeric.GetSingleElement();
}

struct MathematicalBounds {
    __int128 minimum;
    __int128 maximum;
};

std::optional<MathematicalBounds> GetMathematicalBounds(const SIntDomain& domain)
{
    const auto& numeric = domain.NumericBound();
    if (numeric.IsEmptySet() || numeric.IsFullSet() ||
        (domain.IsUnsigned() ? numeric.IsWrappedSet() : numeric.IsSignWrappedSet())) {
        return std::nullopt;
    }
    return MathematicalBounds{
        ToMathematicalInteger(numeric.MinValue(domain.IsUnsigned()), domain.IsUnsigned()),
        ToMathematicalInteger(numeric.MaxValue(domain.IsUnsigned()), domain.IsUnsigned())};
}

bool CanProveNoArithmeticOverflow(ExprKind kind, const SIntDomain& lhs, const SIntDomain& rhs,
    IntWidth resultWidth, bool resultUnsigned)
{
    auto lhsBounds = GetMathematicalBounds(lhs);
    auto rhsBounds = GetMathematicalBounds(rhs);
    if (!lhsBounds.has_value() || !rhsBounds.has_value()) {
        return false;
    }
    if (resultUnsigned && lhs.IsUnsigned() && rhs.IsUnsigned()) {
        auto lhsMinimum = static_cast<unsigned __int128>(lhsBounds->minimum);
        auto lhsMaximum = static_cast<unsigned __int128>(lhsBounds->maximum);
        auto rhsMaximum = static_cast<unsigned __int128>(rhsBounds->maximum);
        auto typeMaximum = static_cast<unsigned __int128>(SInt::UMaxValue(resultWidth).UVal());
        switch (kind) {
            case ExprKind::ADD:
                return lhsMaximum + rhsMaximum <= typeMaximum;
            case ExprKind::SUB:
                return lhsMinimum >= rhsMaximum;
            case ExprKind::MUL:
                return lhsMaximum * rhsMaximum <= typeMaximum;
            default:
                return false;
        }
    }
    if (lhsBounds->minimum < INT64_MIN || lhsBounds->maximum > INT64_MAX ||
        rhsBounds->minimum < INT64_MIN || rhsBounds->maximum > INT64_MAX) {
        return false;
    }
    __int128 resultMin = 0;
    __int128 resultMax = 0;
    switch (kind) {
        case ExprKind::ADD:
            resultMin = lhsBounds->minimum + rhsBounds->minimum;
            resultMax = lhsBounds->maximum + rhsBounds->maximum;
            break;
        case ExprKind::SUB:
            resultMin = lhsBounds->minimum - rhsBounds->maximum;
            resultMax = lhsBounds->maximum - rhsBounds->minimum;
            break;
        case ExprKind::MUL: {
            std::array<__int128, 4> products{
                lhsBounds->minimum * rhsBounds->minimum,
                lhsBounds->minimum * rhsBounds->maximum,
                lhsBounds->maximum * rhsBounds->minimum,
                lhsBounds->maximum * rhsBounds->maximum};
            auto bounds = std::minmax_element(products.begin(), products.end());
            resultMin = *bounds.first;
            resultMax = *bounds.second;
            break;
        }
        default:
            return false;
    }
    auto typeMinimum = resultUnsigned
        ? static_cast<__int128>(0)
        : static_cast<__int128>(SInt::SMinValue(resultWidth).SVal());
    auto typeMaximum = resultUnsigned
        ? static_cast<__int128>(SInt::UMaxValue(resultWidth).UVal())
        : static_cast<__int128>(SInt::SMaxValue(resultWidth).SVal());
    return resultMin >= typeMinimum && resultMax <= typeMaximum;
}

std::optional<SIntCongruence> ComputeArithmeticCongruence(ExprKind kind, const SIntRange* lhsRange,
    const SIntRange* rhsRange, const SIntDomain& lhsDomain, const SIntDomain& rhsDomain,
    Type* resultType, OverflowStrategy overflowStrategy)
{
    if (lhsRange == nullptr || rhsRange == nullptr || resultType == nullptr || !resultType->IsInteger()) {
        return std::nullopt;
    }
    auto lhs = DescribeCongruence(
        lhsRange->GetExactValues(), lhsRange->GetCongruence(), lhsDomain.IsUnsigned());
    auto rhs = DescribeCongruence(
        rhsRange->GetExactValues(), rhsRange->GetCongruence(), rhsDomain.IsUnsigned());
    // An unconstrained integer is still the congruence 0 (mod 1).  Keeping
    // that neutral element is essential for affine multiplication: if x is
    // arbitrary and c is constant, c*x is 0 (mod |c|).  Requiring both
    // operands to be constrained discarded this fact before the MUL rule.

    unsigned __int128 modulus = 0;
    switch (kind) {
        case ExprKind::ADD:
        case ExprKind::SUB:
            modulus = GcdWide(lhs.modulus, rhs.modulus);
            break;
        case ExprKind::MUL: {
            auto lhsMagnitude = lhs.representative < 0
                ? static_cast<unsigned __int128>(-lhs.representative)
                : static_cast<unsigned __int128>(lhs.representative);
            auto rhsMagnitude = rhs.representative < 0
                ? static_cast<unsigned __int128>(-rhs.representative)
                : static_cast<unsigned __int128>(rhs.representative);
            modulus = GcdWide(lhs.modulus * rhs.modulus, lhs.modulus * rhsMagnitude);
            modulus = GcdWide(modulus, rhs.modulus * lhsMagnitude);
            break;
        }
        default:
            return std::nullopt;
    }
    if (modulus <= 1) {
        return std::nullopt;
    }

    auto width = ToWidth(*resultType);
    auto noOverflow = overflowStrategy == OverflowStrategy::THROWING ||
        CanProveNoArithmeticOverflow(
            kind, lhsDomain, rhsDomain, width, resultType->IsUnsignedInteger());
    if (!noOverflow) {
        if (overflowStrategy != OverflowStrategy::WRAPPING) {
            return std::nullopt;
        }
        auto wrapModulus = static_cast<unsigned __int128>(1) << static_cast<unsigned>(width);
        modulus = GcdWide(modulus, wrapModulus);
    }
    if (modulus <= 1 || modulus > UINT64_MAX) {
        return std::nullopt;
    }

    auto stride = static_cast<uint64_t>(modulus);
    auto lhsResidue = CanonicalResidue(lhs.representative, stride);
    auto rhsResidue = CanonicalResidue(rhs.representative, stride);
    uint64_t resultResidue = 0;
    switch (kind) {
        case ExprKind::ADD:
            resultResidue = static_cast<uint64_t>(
                (static_cast<unsigned __int128>(lhsResidue) + rhsResidue) % stride);
            break;
        case ExprKind::SUB:
            resultResidue = static_cast<uint64_t>(
                (static_cast<unsigned __int128>(lhsResidue) + stride - rhsResidue) % stride);
            break;
        case ExprKind::MUL:
            resultResidue = static_cast<uint64_t>(
                (static_cast<unsigned __int128>(lhsResidue) * rhsResidue) % stride);
            break;
        default:
            return std::nullopt;
    }
    return SIntCongruence{stride, resultResidue};
}

std::optional<SIntRange> TryComputeExactArithmeticRange(ExprKind kind, const Ptr<Type>& type,
    const Ptr<Type>& rhsType, OverflowStrategy ov, const std::optional<std::vector<SInt>>& lhsValues,
    const std::optional<std::vector<SInt>>& rhsValues, bool isUnsigned)
{
    if (type == nullptr || rhsType == nullptr || !lhsValues.has_value() || !rhsValues.has_value()) {
        return std::nullopt;
    }
    if (!IsBasicArithmeticKind(kind)) {
        return std::nullopt;
    }
    if (ExceedsExactIntSetProduct(lhsValues->size(), rhsValues->size())) {
        return std::nullopt;
    }
    std::vector<SInt> values;
    values.reserve(lhsValues->size() * rhsValues->size());
    for (const auto& lhsValue : *lhsValues) {
        for (const auto& rhsValue : *rhsValues) {
            auto value =
                ApplyExactArithmetic(kind, lhsValue, rhsValue, type, ov, isUnsigned, rhsType->IsUnsignedInteger());
            if (!value.has_value()) {
                return std::nullopt;
            }
            values.emplace_back(value.value());
        }
    }
    auto exactValues = NormalizeExactIntSet(std::move(values));
    if (!exactValues.has_value()) {
        return std::nullopt;
    }
    auto domain = DomainFromExactIntValues(*exactValues, isUnsigned);
    return SIntRange{std::move(domain), std::move(exactValues)};
}

std::optional<SIntRange> TryComputeExactArithmeticRange(const BinaryExpression* binaryExpr,
    const std::optional<std::vector<SInt>>& lhsValues, const std::optional<std::vector<SInt>>& rhsValues,
    bool isUnsigned)
{
    if (binaryExpr == nullptr) {
        return std::nullopt;
    }
    return TryComputeExactArithmeticRange(binaryExpr->GetExprKind(), binaryExpr->GetResult()->GetType(),
        binaryExpr->GetRHSOperand()->GetType(), binaryExpr->GetOverflowStrategy(), lhsValues, rhsValues, isUnsigned);
}

std::optional<SIntRange> TryComputeCountedAccumulatorUpdateRange(
    const RangeDomain& state, const BinaryExpression* binaryExpr);
std::optional<SIntRange> TryComputeSimpleInductionLoadExitRange(const RangeDomain& state, const Load* load);
std::optional<SIntRange> TryComputeCountedAccumulatorLoadExitRange(const RangeDomain& state, const Load* load);
std::optional<SIntRange> TryComputeCountedAccumulatorBodyLoadRange(const RangeDomain& state, const Load* load);
std::optional<SIntRange> TryComputeLockstepDifferenceRange(
    const RangeDomain& state, const BinaryExpression* binaryExpr);
std::optional<SIntRange> TryComputeCommonSymbolDifferenceRange(
    const RangeDomain& state, const BinaryExpression* binaryExpr);
namespace {
std::optional<SIntRange> TryComputeSimpleLoopLoadRange(const RangeDomain& state, Value* value);
std::optional<SIntRange> TryComputePairLoopLoadRange(const RangeDomain& state, const Load* load);

Type* GetTrackedMutableGlobalBaseType(Value* location)
{
    if (location == nullptr || !location->IsGlobalVarWithInitializer()) {
        return nullptr;
    }
    auto global = StaticCast<GlobalVar*>(location);
    if (global->TestAttr(Attribute::READONLY) || global->GetType() == nullptr || !global->GetType()->IsRef()) {
        return nullptr;
    }
    auto baseType = GetRefRootBaseType(global->GetType());
    return baseType != nullptr && (baseType->IsBoolean() || baseType->IsInteger() || baseType->IsClass() ||
        baseType->IsStruct() || baseType->IsTuple())
        ? baseType
        : nullptr;
}

AbstractObject* EnsureMutableGlobalValueInitialized(RangeDomain& state, GlobalVar* global)
{
    if (global == nullptr) {
        return nullptr;
    }
    if (state.CheckAbstractValueWithTopBottom(global) != nullptr) {
        return state.CheckAbstractObjectRefBy(global);
    }
    auto object = state.GetReferencedObjAndSetToTop(global, nullptr);
    auto initializer = global->GetInitializer();
    if (initializer != nullptr && !initializer->IsNullLiteral()) {
        state.Update(object, HandleNonNullLiteralValue<RangeValueDomain>(initializer));
    }
    return object;
}

std::mutex trackedMutableGlobalCacheMtx;
std::unordered_map<const BlockGroup*, std::unordered_map<const Package*, TrackedMutableGlobals>>
    trackedMutableGlobalCache;

TrackedMutableGlobals ComputeTrackedMutableGlobals(const BlockGroup* initialBody, const Package* package)
{
    TrackedMutableGlobals result;
    if (initialBody == nullptr) {
        return result;
    }

    std::vector<const BlockGroup*> worklist{initialBody};
    std::unordered_set<const BlockGroup*> visited;
    std::unordered_set<GlobalVar*> seenGlobals;
    bool includePackageGlobals = false;
    bool requiresContextualDispatch = false;
    auto addGlobal = [&](GlobalVar* global) {
        if (global == nullptr || GetTrackedMutableGlobalBaseType(global) == nullptr ||
            !seenGlobals.emplace(global).second) {
            return true;
        }
        if (result.values.size() >= MAX_CONTEXT_GLOBALS) {
            return false;
        }
        result.values.emplace_back(global);
        return true;
    };
    while (!worklist.empty()) {
        auto body = worklist.back();
        worklist.pop_back();
        if (body == nullptr || visited.find(body) != visited.end()) {
            continue;
        }
        if (visited.size() >= MAX_CONTEXT_CALL_CLOSURE_FUNCTIONS) {
            result.values.clear();
            result.complete = false;
            return result;
        }
        visited.emplace(body);
        for (auto block : body->GetBlocks()) {
            for (auto expression : block->GetExpressions()) {
                Value* location = nullptr;
                Value* callee = nullptr;
                bool isCall = false;
                switch (expression->GetExprKind()) {
                    case ExprKind::LOAD:
                        location = StaticCast<Load*>(expression)->GetLocation();
                        break;
                    case ExprKind::STORE:
                        location = StaticCast<Store*>(expression)->GetLocation();
                        break;
                    case ExprKind::APPLY:
                        isCall = true;
                        callee = StaticCast<Apply*>(expression)->GetCallee();
                        break;
                    case ExprKind::APPLY_WITH_EXCEPTION:
                        isCall = true;
                        callee = StaticCast<ApplyWithException*>(expression)->GetCallee();
                        break;
                    case ExprKind::INVOKESTATIC:
                    case ExprKind::INVOKESTATIC_WITH_EXCEPTION:
                        includePackageGlobals = true;
                        break;
                    case ExprKind::INVOKE:
                    case ExprKind::INVOKE_WITH_EXCEPTION:
                        requiresContextualDispatch = true;
                        break;
                    default:
                        break;
                }
                if (!addGlobal(DynamicCast<GlobalVar*>(location))) {
                    result.values.clear();
                    result.complete = false;
                    return result;
                }
                if (!isCall) {
                    continue;
                }
                if (auto lambda = IsApplyToLambda(expression); lambda != nullptr && lambda->GetBody() != nullptr) {
                    worklist.emplace_back(lambda->GetBody());
                    continue;
                }
                if (auto calledFunction = DynamicCast<Function*>(callee); calledFunction != nullptr) {
                    if (calledFunction->GetBody() != nullptr) {
                        worklist.emplace_back(calledFunction->GetBody());
                        continue;
                    }
                    if (calledFunction->TestAttr(Attribute::NO_SIDE_EFFECT)) {
                        continue;
                    }
                }
                includePackageGlobals = true;
            }
        }
    }
    if (includePackageGlobals) {
        if (package == nullptr) {
            result.values.clear();
            result.complete = false;
            return result;
        }
        for (auto global : package->GetGlobalVars()) {
            if (!addGlobal(global)) {
                result.values.clear();
                result.complete = false;
                return result;
            }
        }
    }
    if (requiresContextualDispatch) {
        result.complete = false;
    }
    std::sort(result.values.begin(), result.values.end());
    return result;
}

TrackedMutableGlobals CollectTrackedMutableGlobals(const BlockGroup* initialBody, const Package* package)
{
    if (initialBody == nullptr) {
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(trackedMutableGlobalCacheMtx);
        auto body = trackedMutableGlobalCache.find(initialBody);
        if (body != trackedMutableGlobalCache.end()) {
            auto cached = body->second.find(package);
            if (cached != body->second.end()) {
                return cached->second;
            }
        }
    }

    auto result = ComputeTrackedMutableGlobals(initialBody, package);
    {
        std::lock_guard<std::mutex> lock(trackedMutableGlobalCacheMtx);
        trackedMutableGlobalCache[initialBody].emplace(package, result);
    }
    return result;
}

void ClearTrackedMutableGlobalCache()
{
    std::lock_guard<std::mutex> lock(trackedMutableGlobalCacheMtx);
    trackedMutableGlobalCache.clear();
}

TrackedMutableGlobals CollectTrackedMutableGlobals(const Function* function, const Package* package)
{
    return CollectTrackedMutableGlobals(function == nullptr ? nullptr : function->GetBody(), package);
}

TrackedMutableGlobals CollectTrackedMutableGlobals(const Lambda* lambda, const Package* package)
{
    return CollectTrackedMutableGlobals(lambda == nullptr ? nullptr : lambda->GetBody(), package);
}
}

// 根据普通表达式类别分派对应 transfer，并在调试模式下打印可见值域。
void RangeAnalysis::HandleNormalExpressionEffect(RangeDomain& state, const Expression* expression)
{
    switch (expression->GetExprMajorKind()) {
        case ExprMajorKind::MEMORY_EXPR:
            if (expression->GetExprKind() == ExprKind::STORE) {
                auto store = StaticCast<const Store*>(expression);
                auto global = DynamicCast<GlobalVar*>(store->GetLocation());
                auto baseType = GetTrackedMutableGlobalBaseType(global);
                if (global != nullptr && baseType != nullptr) {
                    EnsureMutableGlobalValueInitialized(state, global);
                    if (GetRefDims(*global->GetType()) > 1) {
                        auto storedObject = state.CheckAbstractObjectRefBy(store->GetValue());
                        if (storedObject == nullptr) {
                            state.SetToTopOrTopRef(global, /* isRef = */ true);
                        } else {
                            state.SetRefToObject(global, storedObject);
                        }
                        // A mutable global whose type is Ref<Ref<T>> is a
                        // storage slot containing an object reference. Store
                        // replaces that inner reference; copying fields into
                        // the old object would incorrectly update aliases of
                        // the previous referent and lose the new identity.
                        return;
                    }
                    auto object = state.CheckAbstractObjectRefBy(global);
                    auto value = CaptureContextValue(state, store->GetValue(), /* preserveIntervals = */ true);
                    if (value.IsTop()) {
                        state.Update(object, /* isTop = */ true);
                    } else {
                        ApplyContextValue(state, global, value);
                    }
                } else {
                    PropagateArrayLiteralInfoOnStore(store);
                }
                return;
            }
            if (expression->GetExprKind() == ExprKind::LOAD) {
                auto load = StaticCast<const Load*>(expression);
                auto global = DynamicCast<GlobalVar*>(load->GetLocation());
                auto baseType = GetTrackedMutableGlobalBaseType(global);
                if (global != nullptr && baseType != nullptr) {
                    auto object = EnsureMutableGlobalValueInitialized(state, global);
                    auto value = CaptureContextValue(state, global, /* preserveIntervals = */ true);
                    if (value.IsTop()) {
                        state.SetToTopOrTopRef(load->GetResult(), load->GetResult()->GetType()->IsRef());
                    } else if (load->GetResult()->GetType()->IsRef() &&
                        (baseType->IsClass() || baseType->IsStruct() || baseType->IsTuple())) {
                        state.SetRefToObject(load->GetResult(), object);
                    } else {
                        ApplyContextValue(state, load->GetResult(), baseType, value);
                    }
                    return;
                }
                PropagateArrayLiteralInfoOnLoad(load);
                if (boundedLoopEvaluationOwner == this) {
                    return;
                }
                if (auto inductionExitRange = TryComputeSimpleInductionLoadExitRange(state, load);
                    inductionExitRange.has_value()) {
                    auto range = std::move(inductionExitRange.value());
                    auto objectRange = range.Clone();
                    state.Update(expression->GetResult(), std::make_unique<SIntRange>(std::move(range)));
                    if (auto object = state.CheckAbstractObjectRefBy(load->GetLocation()); object != nullptr) {
                        state.Update(object, std::move(objectRange));
                    }
                } else if (auto countedAccumulatorRange = TryComputeCountedAccumulatorLoadExitRange(state, load);
                    countedAccumulatorRange.has_value()) {
                    auto range = std::move(countedAccumulatorRange.value());
                    auto objectRange = range.Clone();
                    state.Update(expression->GetResult(), std::make_unique<SIntRange>(std::move(range)));
                    if (auto object = state.CheckAbstractObjectRefBy(load->GetLocation()); object != nullptr) {
                        state.Update(object, std::move(objectRange));
                    }
                } else if (auto accumulatorBodyRange = TryComputeCountedAccumulatorBodyLoadRange(state, load);
                    accumulatorBodyRange.has_value()) {
                    auto range = std::move(accumulatorBodyRange.value());
                    state.Update(expression->GetResult(), std::make_unique<SIntRange>(std::move(range)));
                } else if (auto pairLoopRange = TryComputePairLoopLoadRange(state, load);
                    pairLoopRange.has_value()) {
                    auto range = std::move(pairLoopRange.value());
                    auto objectRange = range.Clone();
                    state.Update(expression->GetResult(), std::make_unique<SIntRange>(std::move(range)));
                    if (auto object = state.CheckAbstractObjectRefBy(load->GetLocation()); object != nullptr) {
                        state.Update(object, std::move(objectRange));
                    }
                } else if (auto loopLoadRange = TryComputeSimpleLoopLoadRange(state, load->GetResult());
                    loopLoadRange.has_value()) {
                    auto range = std::move(loopLoadRange.value());
                    auto objectRange = range.Clone();
                    state.Update(expression->GetResult(), std::make_unique<SIntRange>(std::move(range)));
                    if (auto object = state.CheckAbstractObjectRefBy(load->GetLocation()); object != nullptr) {
                        state.Update(object, std::move(objectRange));
                    }
                }
            }
            return;
        case ExprMajorKind::UNARY_EXPR:
            HandleUnaryExpr(state, StaticCast<const UnaryExpression*>(expression));
            break;
        case ExprMajorKind::BINARY_EXPR:
            HandleBinaryExpr(state, StaticCast<const BinaryExpression*>(expression));
            break;
        case ExprMajorKind::OTHERS:
            HandleOthersExpr(state, expression);
            break;
        case ExprMajorKind::STRUCTURED_CTRL_FLOW_EXPR:
        default: {
#ifndef NDEBUG
            CJC_ABORT();
#else
            return;
#endif
        }
    }
    if (expression->GetExprMajorKind() != ExprMajorKind::UNARY_EXPR &&
        expression->GetExprMajorKind() != ExprMajorKind::BINARY_EXPR &&
        expression->GetExprKind() != ExprKind::TYPECAST) {
        return;
    }
    if (isDebug && !expression->GetResult()->GetType()->IsRef()) {
        if (expression->GetResult()->GetType()->IsBoolean()) {
            auto domain = GetBoolDomainFromState(state, expression->GetResult());
            if (!domain.IsTop()) {
                PrintDebugMessage<BoolDomain>(expression, domain);
            }
        } else if (expression->GetResult()->GetType()->IsInteger()) {
            auto domain = GetSIntDomainFromState(state, expression->GetResult());
            if (!domain.IsTop()) {
                PrintDebugMessage<SIntDomain>(expression, domain);
            }
        }
    }
}

// 由二元比较或布尔等值表达式生成 BoolDomain 结果。
BoolDomain RangeAnalysis::GenerateBoolRangeFromBinaryOp(
    RangeDomain& state, const Ptr<const BinaryExpression>& binaryExpr) const
{
    auto lhs = binaryExpr->GetLHSOperand();
    auto rhs = binaryExpr->GetRHSOperand();
    if (lhs->GetType()->IsInteger()) {
        const auto& lRange = GetSIntDomainFromState(state, lhs);
        const auto& rRange = GetSIntDomainFromState(state, rhs);
        return ComputeRelIntBinop(CHIRRelIntBinopArgs{
            lRange, rRange, lhs, rhs, binaryExpr->GetExprKind(), IsUnsignedArithmetic(*binaryExpr)});
    }
    const auto& lRange = GetBoolDomainFromState(state, lhs);
    const auto& rRange = GetBoolDomainFromState(state, rhs);
    return ComputeEqualityBoolBinop(lRange, rRange, binaryExpr->GetExprKind());
}

// 统计 block 入队次数，并在硬上限前应用循环 widening 快照。
bool RangeAnalysis::CheckInQueueTimes(const Block* block, RangeDomain& curState)
{
    ScopedContextAnalysisStage stage("context-fixpoint-work");
    if (isContextAnalysis && !ConsumeContextAnalysisWork(block)) {
        RecordContextTopSource("fixpoint-budget-clear-state");
        curState.ClearState();
        return true;
    }
    if (inqueueTimes.count(block) == 0) {
        inqueueTimes[block] = 1;
        return false;
    }
    inqueueTimes[block]++;
    auto times = inqueueTimes.at(block);
    const auto snapshotStart = rangeAnalysisConfig.loopWideningStartTimes - 1;
    const std::unordered_set<Value*>* selected = nullptr;
    if (static_cast<size_t>(times) >= snapshotStart &&
        queryGuidedWideningFallbackBlocks.find(block) == queryGuidedWideningFallbackBlocks.end()) {
        auto found = queryGuidedWideningValues.find(block);
        if (found == queryGuidedWideningValues.end()) {
            auto candidates = CollectLoopWidenCandidates(curState, block);
            auto selection = BuildQueryGuidedWideningSelection(curState, block, candidates);
            if (selection.has_value()) {
                auto [inserted, _] = queryGuidedWideningValues.emplace(block, std::move(selection.value()));
                found = inserted;
                if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                    std::cerr << "[RangeAnalysisQGSW] block=" << block->GetIdentifier()
                              << " candidates=" << candidates.size()
                              << " selected=" << found->second.size() << '\n';
                }
            } else {
                queryGuidedWideningFallbackBlocks.emplace(block);
                if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                    std::cerr << "[RangeAnalysisQGSW] block=" << block->GetIdentifier()
                              << " candidates=" << candidates.size() << " fallback=1\n";
                }
            }
        }
        if (found != queryGuidedWideningValues.end()) {
            selected = &found->second;
        }
    }
    if (IsRangeAnalysisPerfCollectionEnabled()) {
        rangeAnalysisTuningStats.maxInqueueCount =
            std::max(rangeAnalysisTuningStats.maxInqueueCount, static_cast<size_t>(times));
    }
    std::lock_guard<std::mutex> lock(loopRangeSnapshotsMtx);
    auto& snapshots = loopRangeSnapshots[this];
    if (static_cast<size_t>(times) >= rangeAnalysisConfig.loopWideningStartTimes) {
        auto previous = snapshots.find(block);
        if (previous != snapshots.end()) {
            ApplyLoopWidening(curState, previous->second, block);
            if (IsRangeAnalysisPerfCollectionEnabled()) {
                ++rangeAnalysisTuningStats.loopWideningApplications;
            }
        }
    }
    if (static_cast<size_t>(times) >= snapshotStart) {
        snapshots[block] = CaptureLoopRangeSnapshot(curState, block, selected);
    }
    if (static_cast<size_t>(times) >= rangeAnalysisConfig.maxInqueueTimes) {
        if (IsRangeAnalysisPerfCollectionEnabled()) {
            ++rangeAnalysisTuningStats.inqueueLimitHits;
        }
        curState.ClearState();
        return true;
    }
    return false;
}

// 处理一元布尔和整数表达式的值域转移。
unsigned RangeAnalysis::GetNarrowingIterationLimit() const
{
    return queryGuidedWideningValues.empty() ? 6 : 3;
}

bool RangeAnalysis::NarrowState(RangeDomain& state, const RangeDomain& candidate)
{
    return state.NarrowValuesWith(candidate,
        [](Value*, RangeValueDomain& current, const RangeValueDomain& narrowed) {
            return NarrowRangeValue(current, narrowed);
        });
}

void RangeAnalysis::HandleUnaryExpr(RangeDomain& state, const UnaryExpression* unaryExpr) const
{
    auto dest = unaryExpr->GetResult();
    if (unaryExpr->GetExprKind() == ExprKind::NOT && dest->GetType()->IsBoolean()) {
        auto operand = unaryExpr->GetOperand();
        auto operandRange = GetBoolDomainFromState(state, operand);
        if (operandRange.IsNonTrivial()) {
            return state.Update(dest, std::make_unique<BoolRange>(!operandRange));
        }
    }
    if (dest->GetType()->IsInteger()) {
        auto operand = unaryExpr->GetOperand();
        const auto& operandRange = GetSIntDomainFromState(state, operand);
        if (unaryExpr->GetExprKind() == ExprKind::NEG) {
            auto range = ComputeNegRange(operandRange, dest->GetType(), unaryExpr->GetOverflowStrategy());
            if (range.IsNonTrivial()) {
                return state.Update(dest, std::make_unique<SIntRange>(std::move(range)));
            }
        } else if (unaryExpr->GetExprKind() == ExprKind::BITNOT) {
            auto range = ComputeBitNotSIntRange(
                GetSIntRangeFromState(state, operand), operandRange,
                dest->GetType()->IsUnsignedInteger());
            if (range.GetVal().IsNonTrivial() || range.GetKnownBits().has_value()) {
                return state.Update(dest, std::make_unique<SIntRange>(std::move(range)));
            }
        }
    }
    return state.SetToBound(dest, true);
}

std::string GenerateTypeRangePrompt(const Ptr<Type>& type)
{
    const static std::unordered_map<Type::TypeKind, std::pair<int64_t, uint64_t>> TYPE_TO_RANGE = {
        {Type::TypeKind::TYPE_INT8, {std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max()}},
        {Type::TypeKind::TYPE_INT16, {std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()}},
        {Type::TypeKind::TYPE_INT32, {std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()}},
        {Type::TypeKind::TYPE_INT64, {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()}},
        {Type::TypeKind::TYPE_INT_NATIVE, {std::numeric_limits<ssize_t>::min(), std::numeric_limits<ssize_t>::max()}},
        {Type::TypeKind::TYPE_UINT8, {std::numeric_limits<uint8_t>::min(), std::numeric_limits<uint8_t>::max()}},
        {Type::TypeKind::TYPE_UINT16, {std::numeric_limits<uint16_t>::min(), std::numeric_limits<uint16_t>::max()}},
        {Type::TypeKind::TYPE_UINT32, {std::numeric_limits<uint32_t>::min(), std::numeric_limits<uint32_t>::max()}},
        {Type::TypeKind::TYPE_UINT64, {std::numeric_limits<uint64_t>::min(), std::numeric_limits<uint64_t>::max()}},
        {Type::TypeKind::TYPE_UINT_NATIVE, {std::numeric_limits<size_t>::min(), std::numeric_limits<size_t>::max()}}};

    auto [min, max] = TYPE_TO_RANGE.at(type->GetTypeKind());
    return "range of " + type->ToString() + " is " + std::to_string(min) + " ~ " + std::to_string(max);
}

template <typename TBinary, typename T>
void RaiseArithmeticOverflowError(const TBinary* expr, ExprKind kind, T leftVal, T rightVal, DiagnosticEngine& diag)
{
    auto& loc = expr->GetDebugLocation();
    auto ty = expr->GetResult()->GetType();
    const static std::unordered_map<ExprKind, std::string> OPS = {
        {ExprKind::ADD, "+"},
        {ExprKind::SUB, "-"},
        {ExprKind::MUL, "*"},
        {ExprKind::DIV, "/"},
        {ExprKind::MOD, "%"},
        {ExprKind::EXP, "**"},
    };
    auto token = OPS.find(kind);
    CJC_ASSERT(token != OPS.end());
    auto builder =
        diag.DiagnoseRefactor(DiagKindRefactor::chir_arithmetic_operator_overflow, ToRange(loc), token->second);
    std::string hint = ty->ToString() + "(" + std::to_string(leftVal) + ") " + token->second + " " +
        expr->GetRHSOperand()->GetType()->ToString() + "(" + std::to_string(rightVal) + ")";
    builder.AddMainHintArguments(hint);
    builder.AddNote(GenerateTypeRangePrompt(expr->GetResult()->GetType()));
}

template <typename T>
bool CheckDivZero(ExprKind exprKind, const Ptr<const BinaryExpression>& binary, T rVal, DiagnosticEngine& diag)
{
    if (rVal == 0 && (exprKind == ExprKind::DIV || exprKind == ExprKind::MOD)) {
        auto& loc = binary->GetDebugLocation();
        auto prompt = exprKind == ExprKind::DIV ? "divide" : "modulo";
        auto builder = diag.DiagnoseRefactor(DiagKindRefactor::chir_divisor_is_zero, ToRange(loc), prompt);
        builder.AddMainHintArguments(prompt);
        return true;
    }
    return false;
}

// 对单点算术表达式执行精确计算，并处理除零和 throwing 溢出诊断。
SIntDomain CheckSingleValueOverflow(
    const CHIRArithmeticBinopArgs& args, const Ptr<const BinaryExpression>& expr, ExprKind exprKind, DiagnosticEngine& diag)
{
    bool isOv = false;
    if (args.uns) {
        uint64_t a = args.ld.NumericBound().Lower().UVal();
        uint64_t b = args.rd.NumericBound().Lower().UVal();
        uint64_t res = 0;
        if (CheckDivZero(exprKind, expr, b, diag)) {
            return SIntDomain::Top(args.ld.Width(), true);
        }
        isOv = OverflowChecker::IsUIntOverflow(args.l->GetType()->GetTypeKind(), exprKind, a, b, args.ov, &res);
        if (isOv && args.ov == OverflowStrategy::THROWING) {
            RaiseArithmeticOverflowError(expr.get(), exprKind, a, b, diag);
            return SIntDomain::Top(args.ld.Width(), true);
        }
        return {ConstantRange{{args.ld.Width(), res}}, true};
    } else {
        int64_t a = args.ld.NumericBound().Lower().SVal();
        int64_t b = args.rd.NumericBound().Lower().SVal();
        int64_t res = 0;
        if (CheckDivZero(exprKind, expr, b, diag)) {
            return SIntDomain::Top(args.ld.Width(), false);
        }
        isOv = OverflowChecker::IsIntOverflow(args.l->GetType()->GetTypeKind(), exprKind, a, b, args.ov, &res);
        if (isOv && args.ov == OverflowStrategy::THROWING) {
            RaiseArithmeticOverflowError(expr.get(), exprKind, a, b, diag);
            return SIntDomain::Top(args.ld.Width(), false);
        }
        return {ConstantRange{{args.ld.Width(), static_cast<uint64_t>(res)}}, false};
    }
}

// 处理算术、位运算、移位、关系和布尔二元表达式的值域转移。
void RangeAnalysis::HandleBinaryExpr(RangeDomain& state, const BinaryExpression* binaryExpr)
{
    auto dest = binaryExpr->GetResult();
    auto lhs = binaryExpr->GetLHSOperand();
    auto rhs = binaryExpr->GetRHSOperand();
    if (!CanAnalyse(dest->GetType()) || !CanAnalyse(lhs->GetType()) || !CanAnalyse(rhs->GetType())) {
        return state.SetToBound(binaryExpr->GetResult(), true);
    }
    if (dest->GetType()->IsInteger()) {
        auto kind = binaryExpr->GetExprKind();
        if (!IsBasicBinaryExpr(*binaryExpr) && !IsBitwiseBinaryExpr(kind) && !IsShiftBinaryExpr(kind)) {
            return state.SetToBound(binaryExpr->GetResult(), true);
        }
        if (boundedLoopEvaluationOwner != this) {
            if (auto symbolicDifference =
                    TryComputeCommonSymbolDifferenceRange(state, binaryExpr);
                symbolicDifference.has_value()) {
                return state.Update(
                    dest, std::make_unique<SIntRange>(std::move(symbolicDifference.value())));
            }
            if (auto lockstepRange = TryComputeLockstepDifferenceRange(state, binaryExpr);
                lockstepRange.has_value()) {
                return state.Update(dest, std::make_unique<SIntRange>(std::move(lockstepRange.value())));
            }
            if (auto countedAccumulatorRange = TryComputeCountedAccumulatorUpdateRange(state, binaryExpr);
                countedAccumulatorRange.has_value()) {
                return state.Update(dest, std::make_unique<SIntRange>(std::move(countedAccumulatorRange.value())));
            }
            if (auto inductionRange = TryComputeSimpleInductionUpdateRange(binaryExpr);
                inductionRange.has_value() && inductionRange->IsNonTrivial()) {
                return state.Update(dest, std::make_unique<SIntRange>(std::move(inductionRange.value())));
            }
        }
        const auto& lRange = GetSIntDomainFromState(state, lhs);
        const auto& rRange = GetSIntDomainFromState(state, rhs);
        if (IsBitwiseBinaryExpr(kind) || IsShiftBinaryExpr(kind)) {
            auto lhsSIntRange = GetSIntRangeFromState(state, lhs);
            auto rhsSIntRange = GetSIntRangeFromState(state, rhs);
            auto lhsValues = GetExactValuesOrSmallRange(
                lhsSIntRange, lhs->GetType()->IsUnsignedInteger());
            auto rhsValues = GetExactValuesOrSmallRange(
                rhsSIntRange, rhs->GetType()->IsUnsignedInteger());
            if (auto exactRes = TryComputeExactBitwiseRange(kind, lhsValues, rhsValues,
                    rhs->GetType()->IsUnsignedInteger(), dest->GetType()->IsUnsignedInteger());
                exactRes.has_value()) {
                return state.Update(dest, std::make_unique<SIntRange>(std::move(exactRes.value())));
            }
            auto res = TryComputeAbstractBitwiseRange(kind, lhsSIntRange, rhsSIntRange,
                lRange, rRange, lhs, rhs, rhs->GetType()->IsUnsignedInteger(),
                dest->GetType()->IsUnsignedInteger());
            if (res.has_value()) {
                return state.Update(dest, std::make_unique<SIntRange>(std::move(*res)));
            }
            return state.SetToBound(binaryExpr->GetResult(), true);
        }
        auto ov = binaryExpr->GetOverflowStrategy();
        auto isUnsigned = IsUnsignedArithmetic(*binaryExpr);
        auto lhsRange = GetSIntRangeFromState(state, lhs);
        auto rhsRange = GetSIntRangeFromState(state, rhs);
        auto lhsValues = GetExactValuesOrSmallRange(lhsRange, isUnsigned);
        auto rhsValues = GetExactValuesOrSmallRange(rhsRange, rhs->GetType()->IsUnsignedInteger());
        if (auto exactRes = TryComputeExactArithmeticRange(binaryExpr, lhsValues, rhsValues, isUnsigned);
            exactRes.has_value()) {
            return state.Update(dest, std::make_unique<SIntRange>(std::move(exactRes.value())));
        }
        if (kind == ExprKind::EXP) {
            return state.SetToBound(binaryExpr->GetResult(), true);
        }
        if (lRange.IsSingleValue() && rRange.IsSingleValue()) {
            auto domain = CheckSingleValueOverflow(
                CHIRArithmeticBinopArgs{lRange, rRange, lhs, rhs, kind, ov, isUnsigned}, binaryExpr, kind, *diag);
            state.Update(dest, std::make_unique<SIntRange>(domain));
            return;
        }
        auto res = ComputeArithmeticBinop(CHIRArithmeticBinopArgs{lRange, rRange, lhs, rhs, kind, ov, isUnsigned});
        auto congruence = ComputeArithmeticCongruence(
            kind, lhsRange, rhsRange, lRange, rRange, dest->GetType(), ov);
        if (res.IsNonTrivial() || congruence.has_value()) {
            return state.Update(dest, std::make_unique<SIntRange>(
                std::move(res), std::nullopt, std::move(congruence)));
        }
    }
    if (dest->GetType()->IsBoolean()) {
        auto kind = binaryExpr->GetExprKind();
        auto res = IsLogicalBinaryExpr(kind)
            ? (kind == ExprKind::AND ? LogicalAnd(GetBoolDomainFromState(state, lhs), GetBoolDomainFromState(state, rhs))
                                      : LogicalOr(GetBoolDomainFromState(state, lhs), GetBoolDomainFromState(state, rhs)))
            : GenerateBoolRangeFromBinaryOp(state, binaryExpr);
        if (res.IsNonTrivial()) {
            return state.Update(dest, std::make_unique<BoolRange>(res));
        }
    }
    state.SetToBound(binaryExpr->GetResult(), true);
}

RangeAnalysis::ExceptionKind RangeAnalysis::HandleIntOpWithException(RangeDomain& state, const IntOpWithException* intOp)
{
    if (intOp == nullptr || intOp->GetResult() == nullptr) {
        return ExceptionKind::NA;
    }
    auto dest = intOp->GetResult();
    auto kind = intOp->GetOpKind();
    if (kind == ExprKind::NOT && dest->GetType()->IsBoolean()) {
        auto operandRange = GetBoolDomainFromState(state, intOp->GetOperand(0));
        if (operandRange.IsNonTrivial()) {
            state.Update(dest, std::make_unique<BoolRange>(!operandRange));
        } else {
            state.SetToBound(dest, true);
        }
        return ExceptionKind::NA;
    }
    if (!dest->GetType()->IsInteger()) {
        state.SetToTopOrTopRef(dest, dest->GetType()->IsRef());
        return ExceptionKind::NA;
    }
    if (kind == ExprKind::NEG || kind == ExprKind::BITNOT) {
        auto operand = intOp->GetOperand(0);
        if (operand == nullptr || !operand->GetType()->IsInteger()) {
            state.SetToBound(dest, true);
            return ExceptionKind::NA;
        }
        const auto& operandRange = GetSIntDomainFromState(state, operand);
        if (kind == ExprKind::NEG) {
            auto operandValues = GetExactValuesOrSmallRange(
                GetSIntRangeFromState(state, operand), operand->GetType()->IsUnsignedInteger());
            if (operandValues.has_value() &&
                !ExceedsExactIntSetLimit(operandValues->size())) {
                std::vector<SInt> values;
                bool hasSuccess = false;
                bool hasFail = false;
                for (const auto& operandValue : *operandValues) {
                    auto value = ApplyExactNeg(
                        operandValue, dest->GetType(), intOp->GetOverflowStrategy(),
                        operand->GetType()->IsUnsignedInteger());
                    if (value.has_value()) {
                        hasSuccess = true;
                        values.emplace_back(value.value());
                    } else {
                        hasFail = true;
                    }
                }
                if (hasSuccess) {
                    if (auto exactValues = NormalizeExactIntSet(std::move(values)); exactValues.has_value()) {
                        auto domain = DomainFromExactIntValues(*exactValues, dest->GetType()->IsUnsignedInteger());
                        state.Update(dest, std::make_unique<SIntRange>(std::move(domain), std::move(exactValues)));
                    }
                    return hasFail ? ExceptionKind::NA : ExceptionKind::SUCCESS;
                }
                if (hasFail) {
                    return ExceptionKind::FAIL;
                }
            }
            auto range = ComputeNegRange(operandRange, dest->GetType(), intOp->GetOverflowStrategy());
            if (range.IsNonTrivial()) {
                state.Update(dest, std::make_unique<SIntRange>(std::move(range)));
            } else {
                state.SetToBound(dest, true);
            }
            return ExceptionKind::NA;
        }
        auto range = ComputeBitNotSIntRange(
            GetSIntRangeFromState(state, operand), operandRange,
            dest->GetType()->IsUnsignedInteger());
        if (range.GetVal().IsNonTrivial() || range.GetKnownBits().has_value()) {
            state.Update(dest, std::make_unique<SIntRange>(std::move(range)));
        } else {
            state.SetToBound(dest, true);
        }
        return ExceptionKind::NA;
    }
    if (intOp->GetNumOfOperands() < 2) {
        state.SetToBound(dest, true);
        return ExceptionKind::NA;
    }
    auto lhs = intOp->GetLHSOperand();
    auto rhs = intOp->GetRHSOperand();
    if (lhs == nullptr || rhs == nullptr || !CanAnalyse(dest->GetType()) || !CanAnalyse(lhs->GetType()) ||
        !CanAnalyse(rhs->GetType()) || !lhs->GetType()->IsInteger() || !rhs->GetType()->IsInteger()) {
        state.SetToBound(dest, true);
        return ExceptionKind::NA;
    }
    if (!IsBasicArithmeticKind(kind) && !IsBitwiseBinaryExpr(kind) && !IsShiftBinaryExpr(kind)) {
        state.SetToBound(dest, true);
        return ExceptionKind::NA;
    }
    const auto& lRange = GetSIntDomainFromState(state, lhs);
    const auto& rRange = GetSIntDomainFromState(state, rhs);
    auto destUnsigned = dest->GetType()->IsUnsignedInteger();
    auto rhsUnsigned = rhs->GetType()->IsUnsignedInteger();
    if (IsBitwiseBinaryExpr(kind) || IsShiftBinaryExpr(kind)) {
        auto lhsSIntRange = GetSIntRangeFromState(state, lhs);
        auto rhsSIntRange = GetSIntRangeFromState(state, rhs);
        auto lhsValues = GetExactValuesOrSmallRange(
            lhsSIntRange, lhs->GetType()->IsUnsignedInteger());
        auto rhsValues = GetExactValuesOrSmallRange(rhsSIntRange, rhsUnsigned);
        if (lhsValues.has_value() && rhsValues.has_value() &&
            !ExceedsExactIntSetProduct(lhsValues->size(), rhsValues->size())) {
            std::vector<SInt> values;
            bool hasSuccess = false;
            bool hasFail = false;
            values.reserve(lhsValues->size() * rhsValues->size());
            for (const auto& lhsValue : *lhsValues) {
                for (const auto& rhsValue : *rhsValues) {
                    if (IsShiftBinaryExpr(kind)) {
                        auto value = ApplyExactShift(kind, lhsValue, rhsValue, rhsUnsigned, destUnsigned);
                        if (value.has_value()) {
                            hasSuccess = true;
                            values.emplace_back(value.value());
                        } else {
                            hasFail = true;
                        }
                        continue;
                    }
                    hasSuccess = true;
                    values.emplace_back(ApplyExactBitwise(kind, lhsValue, rhsValue, destUnsigned));
                }
            }
            if (hasSuccess) {
                if (auto exactValues = NormalizeExactIntSet(std::move(values)); exactValues.has_value()) {
                    auto domain = DomainFromExactIntValues(*exactValues, destUnsigned);
                    state.Update(dest, std::make_unique<SIntRange>(std::move(domain), std::move(exactValues)));
                }
                return hasFail ? ExceptionKind::NA : ExceptionKind::SUCCESS;
            }
            if (hasFail) {
                return ExceptionKind::FAIL;
            }
        }
        auto res = TryComputeAbstractBitwiseRange(kind, lhsSIntRange, rhsSIntRange,
            lRange, rRange, lhs, rhs, rhsUnsigned, destUnsigned);
        if (res.has_value()) {
            state.Update(dest, std::make_unique<SIntRange>(std::move(*res)));
        } else {
            state.SetToBound(dest, true);
        }
        return ExceptionKind::NA;
    }
    auto isUnsigned = lhs->GetType()->IsUnsignedInteger() || IsStructEnum(lhs->GetType());
    auto ov = intOp->GetOverflowStrategy();
    auto lhsValues = GetExactValuesOrSmallRange(GetSIntRangeFromState(state, lhs), isUnsigned);
    auto rhsValues = GetExactValuesOrSmallRange(GetSIntRangeFromState(state, rhs), rhsUnsigned);
    if (lhsValues.has_value() && rhsValues.has_value() &&
        !ExceedsExactIntSetProduct(lhsValues->size(), rhsValues->size())) {
        std::vector<SInt> values;
        bool hasSuccess = false;
        bool hasFail = false;
        values.reserve(lhsValues->size() * rhsValues->size());
        for (const auto& lhsValue : *lhsValues) {
            for (const auto& rhsValue : *rhsValues) {
                auto value =
                    ApplyExactArithmetic(kind, lhsValue, rhsValue, dest->GetType(), ov, isUnsigned, rhsUnsigned);
                if (value.has_value()) {
                    hasSuccess = true;
                    values.emplace_back(value.value());
                } else {
                    hasFail = true;
                }
            }
        }
        if (hasSuccess) {
            if (auto exactValues = NormalizeExactIntSet(std::move(values)); exactValues.has_value()) {
                auto domain = DomainFromExactIntValues(*exactValues, isUnsigned);
                state.Update(dest, std::make_unique<SIntRange>(std::move(domain), std::move(exactValues)));
            }
            return hasFail ? ExceptionKind::NA : ExceptionKind::SUCCESS;
        }
        if (hasFail) {
            return ExceptionKind::FAIL;
        }
    }
    if (kind == ExprKind::EXP) {
        state.SetToBound(dest, true);
        return ExceptionKind::NA;
    }
    auto res = ComputeArithmeticBinop(CHIRArithmeticBinopArgs{lRange, rRange, lhs, rhs, kind, ov, isUnsigned});
    if (res.IsNonTrivial()) {
        state.Update(dest, std::make_unique<SIntRange>(std::move(res)));
    } else {
        state.SetToBound(dest, true);
    }
    return ExceptionKind::NA;
}

bool CanProveTypeCastWithoutOverflow(
    const SIntDomain& source, IntWidth targetWidth, bool targetUnsigned)
{
    auto bounds = GetMathematicalBounds(source);
    if (!bounds.has_value()) {
        return false;
    }
    const auto width = static_cast<unsigned>(targetWidth);
    const __int128 minimum = targetUnsigned
        ? 0
        : -(static_cast<__int128>(1) << (width - 1U));
    const __int128 maximum = targetUnsigned
        ? (static_cast<__int128>(1) << width) - 1
        : (static_cast<__int128>(1) << (width - 1U)) - 1;
    return bounds->minimum >= minimum && bounds->maximum <= maximum;
}

std::optional<SIntKnownBits> TransformKnownBitsForIntegerCast(
    const SIntRange* sourceRange, const SIntDomain& sourceDomain,
    Type* sourceType, Type* targetType, OverflowStrategy overflowStrategy)
{
    if (sourceType == nullptr || targetType == nullptr ||
        !sourceType->IsInteger() || !targetType->IsInteger()) {
        return std::nullopt;
    }
    auto sourceBits = sourceRange == nullptr
        ? InferKnownBitsFromInterval(sourceDomain)
        : GetEffectiveKnownBits(sourceDomain, sourceRange->GetExactValues(),
              sourceRange->GetCongruence(), sourceRange->GetKnownBits());
    if (!sourceBits.has_value()) {
        return std::nullopt;
    }

    const auto sourceWidth = sourceDomain.Width();
    const auto targetWidth = ToWidth(*targetType);
    const bool targetUnsigned = targetType->IsUnsignedInteger();
    const bool noOverflow =
        CanProveTypeCastWithoutOverflow(sourceDomain, targetWidth, targetUnsigned);
    if (!noOverflow && overflowStrategy != OverflowStrategy::WRAPPING &&
        overflowStrategy != OverflowStrategy::THROWING) {
        // Saturating and checked conversions can replace overflowing inputs
        // with values whose bits are unrelated to the source bit pattern.
        return std::nullopt;
    }

    const auto sourceMask = GetSIntWidthMask(sourceWidth);
    const auto targetMask = GetSIntWidthMask(targetWidth);
    const auto retainedMask = sourceWidth < targetWidth ? sourceMask : targetMask;
    SIntKnownBits result{sourceBits->knownZero & retainedMask,
        sourceBits->knownOne & retainedMask};
    if (targetWidth <= sourceWidth) {
        return NormalizeKnownBits(result, targetWidth);
    }

    const auto extensionMask = targetMask & ~sourceMask;
    const bool sourceUnsigned = sourceType->IsUnsignedInteger();
    const bool zeroExtend = sourceUnsigned ||
        (!sourceUnsigned && targetUnsigned &&
            (noOverflow || overflowStrategy == OverflowStrategy::THROWING));
    if (zeroExtend) {
        result.knownZero |= extensionMask;
        return NormalizeKnownBits(result, targetWidth);
    }

    const auto sourceSignBit =
        uint64_t{1} << (static_cast<unsigned>(sourceWidth) - 1U);
    if ((sourceBits->knownZero & sourceSignBit) != 0) {
        result.knownZero |= extensionMask;
    } else if ((sourceBits->knownOne & sourceSignBit) != 0) {
        result.knownOne |= extensionMask;
    }
    return NormalizeKnownBits(result, targetWidth);
}

RangeAnalysis::ExceptionKind RangeAnalysis::HandleTypeCastWithException(
    RangeDomain& state, const TypeCastWithException* cast)
{
    if (cast == nullptr || cast->GetResult() == nullptr) {
        return ExceptionKind::NA;
    }
    auto from = cast->GetSourceTy();
    auto to = cast->GetTargetTy();
    auto dest = cast->GetResult();
    if (!from->IsInteger() || !to->IsInteger()) {
        state.SetToTopOrTopRef(dest, dest->GetType()->IsRef());
        return ExceptionKind::NA;
    }
    auto value = cast->GetSourceValue();
    auto sourceValues = GetExactValuesOrSmallRange(GetSIntRangeFromState(state, value), from->IsUnsignedInteger());
    if (sourceValues.has_value() &&
        !ExceedsExactIntSetLimit(sourceValues->size())) {
        std::vector<SInt> values;
        bool hasSuccess = false;
        bool hasFail = false;
        values.reserve(sourceValues->size());
        for (const auto& sourceValue : *sourceValues) {
            auto castValue = ApplyExactTypeCast(sourceValue, from, to, cast->GetOverflowStrategy());
            if (castValue.has_value()) {
                hasSuccess = true;
                values.emplace_back(castValue.value());
            } else {
                hasFail = true;
            }
        }
        if (hasSuccess) {
            if (auto exactValues = NormalizeExactIntSet(std::move(values)); exactValues.has_value()) {
                auto domain = DomainFromExactIntValues(*exactValues, to->IsUnsignedInteger());
                state.Update(dest, std::make_unique<SIntRange>(std::move(domain), std::move(exactValues)));
            }
            return hasFail ? ExceptionKind::NA : ExceptionKind::SUCCESS;
        }
        if (hasFail) {
            return ExceptionKind::FAIL;
        }
    }

    auto range = ComputeTypeCastRange(
        state, value, from, to, cast->GetOverflowStrategy());
    state.Update(dest, std::make_unique<SIntRange>(std::move(range)));
    return ExceptionKind::NA;
}

// 计算整数 typecast 后的值域，并在安全时保留非负符号约束。
SIntDomain RangeAnalysis::ComputeTypeCast(RangeDomain& state, PtrSymbol oldSymbol, const SIntDomain& v,
    IntWidth dstSize, bool dstUnsigned, OverflowStrategy ov) const
{
    auto numericRange{ComputeTypeCastNumericBound(v, dstSize, dstUnsigned, ov)};
    if (dstSize < v.Width() || v.IsUnsigned() || !dstUnsigned || ov == OverflowStrategy::SATURATING ||
        numericRange.SMinValue().Slt({dstSize, 0u})) {
        return {numericRange, dstUnsigned};
    }
    // signed to unsigned, same width or larger width
    // in this special case, if we have a symbolic range a<b and we know definitely b>=0 && a>=0,
    // this range can be preserved
    SIntDomain::SymbolicBoundsMap mp{};
    for (auto it = v.SymbolicBounds().Begin(); it != v.SymbolicBounds().End(); it++) {
        auto absVal = state.CheckAbstractValue(it->first);
        if (absVal != nullptr && absVal->GetRangeKind() == ValueRange::RangeKind::SINT) {
            auto range = StaticCast<SIntRange>(absVal)->GetVal();
            if (range.NumericBound().SMinValue().Sge(SInt{range.Width(), 0u})) {
                mp.emplace(it->first,
                    NumericConversion(it->second, dstSize, false, false,
                        OverflowStrategy::THROWING)); // this typecast can never wrap, pass THROWING for better
                                                      // performance
            }
        }
    }
    mp.emplace(oldSymbol, ConstantRange{{dstSize, 0u}});
    return SIntDomain{numericRange, std::move(mp), dstUnsigned};
}

SIntRange RangeAnalysis::ComputeTypeCastRange(RangeDomain& state, PtrSymbol value,
    Type* sourceType, Type* targetType, OverflowStrategy overflowStrategy) const
{
    const auto& sourceDomain = GetSIntDomainFromState(state, value);
    auto resultDomain = ComputeTypeCast(state, value, sourceDomain,
        ToWidth(*targetType), targetType->IsUnsignedInteger(), overflowStrategy);
    const auto* sourceRange = GetSIntRangeFromState(state, value);

    std::optional<std::vector<SInt>> exactValues;
    if (sourceRange != nullptr && sourceRange->GetExactValues().has_value()) {
        std::vector<SInt> converted;
        converted.reserve(sourceRange->GetExactValues()->size());
        for (const auto& sourceValue : *sourceRange->GetExactValues()) {
            auto convertedValue = ApplyExactTypeCast(
                sourceValue, sourceType, targetType, overflowStrategy);
            if (convertedValue.has_value()) {
                converted.emplace_back(convertedValue.value());
            }
        }
        exactValues = NormalizeExactIntSet(std::move(converted));
    }

    auto knownBits = TransformKnownBitsForIntegerCast(sourceRange, sourceDomain,
        sourceType, targetType, overflowStrategy);
    return SIntRange{std::move(resultDomain), std::move(exactValues),
        std::nullopt, std::move(knownBits)};
}

// 处理 typecast 等其它表达式，并对未知结果设置保守 Top 或 TopRef。
bool CopyKnownScalarRange(RangeDomain& state, Value* dest, Value* source)
{
    if (dest == nullptr || source == nullptr || dest->GetType()->IsRef()) {
        return false;
    }
    auto domain = state.CheckAbstractValueWithTopBottom(source);
    if (domain == nullptr || domain->IsTop()) {
        return false;
    }
    auto absVal = domain->CheckAbsVal();
    if (absVal == nullptr) {
        return false;
    }
    if (dest->GetType()->IsBoolean()) {
        if (absVal->GetRangeKind() != ValueRange::RangeKind::BOOL) {
            return false;
        }
        auto range = StaticCast<const BoolRange*>(absVal)->GetVal();
        state.Update(dest, std::make_unique<BoolRange>(range));
        return true;
    }
    if (dest->GetType()->IsInteger()) {
        if (absVal->GetRangeKind() != ValueRange::RangeKind::SINT) {
            return false;
        }
        state.Update(dest, StaticCast<const SIntRange*>(absVal)->Clone());
        return true;
    }
    return false;
}

Value* GetAggregateLiteralElement(Value* aggregate, size_t index)
{
    auto expr = GetDefiningExprForWidening(aggregate);
    if (expr == nullptr) {
        return nullptr;
    }
    if (expr->GetExprKind() == ExprKind::TUPLE) {
        auto elements = StaticCast<const Tuple*>(expr)->GetElementValues();
        return index < elements.size() ? elements[index] : nullptr;
    }
    if (expr->GetExprKind() == ExprKind::VARRAY) {
        auto elements = expr->GetOperands();
        return index < elements.size() ? elements[index] : nullptr;
    }
    return nullptr;
}

bool TryHandleFieldFromAggregateLiteral(RangeDomain& state, const Field* field)
{
    auto path = field->GetPath();
    if (path.size() != 1) {
        return false;
    }
    auto element = GetAggregateLiteralElement(field->GetBase(), static_cast<size_t>(path.front()));
    return CopyKnownScalarRange(state, field->GetResult(), element);
}

bool HasOnlyDirectEnumLoadStoreUsers(Value* location)
{
    if (location == nullptr || location->IsGlobal() || location->TestAttr(Attribute::STATIC)) {
        return false;
    }
    for (auto user : location->GetUsers()) {
        if (user->GetExprKind() == ExprKind::DEBUGEXPR) {
            continue;
        }
        if (user->GetExprKind() == ExprKind::LOAD && StaticCast<const Load*>(user)->GetLocation() == location) {
            continue;
        }
        if (user->GetExprKind() == ExprKind::STORE && StaticCast<const Store*>(user)->GetLocation() == location) {
            continue;
        }
        return false;
    }
    return true;
}

std::optional<uint64_t> GetSingleEnumTagRangeValue(const RangeDomain& state, Value* value)
{
    auto range = state.CheckAbstractValue(value);
    if (range == nullptr) {
        return std::nullopt;
    }
    if (range->GetRangeKind() == ValueRange::RangeKind::BOOL) {
        const auto& domain = StaticCast<const BoolRange*>(range)->GetVal();
        return domain.IsSingleValue() ? std::optional<uint64_t>{domain.IsTrue() ? 1U : 0U} : std::nullopt;
    }
    if (range->GetRangeKind() != ValueRange::RangeKind::SINT) {
        return std::nullopt;
    }
    const auto& domain = StaticCast<const SIntRange*>(range)->GetVal();
    if (!domain.IsSingleValue()) {
        return std::nullopt;
    }
    return domain.NumericBound().GetSingleElement().UVal();
}

std::optional<uint64_t> GetEnumConstructorIndex(Value* enumTuple)
{
    auto expression = GetDefiningExprForWidening(enumTuple);
    if (expression == nullptr || expression->GetExprKind() != ExprKind::TUPLE ||
        enumTuple->GetType() == nullptr || !enumTuple->GetType()->IsEnum()) {
        return std::nullopt;
    }
    auto elements = StaticCast<const Tuple*>(expression)->GetElementValues();
    if (elements.empty()) {
        return std::nullopt;
    }
    auto indexExpression = GetDefiningExprForWidening(elements.front());
    if (indexExpression == nullptr || indexExpression->GetExprKind() != ExprKind::CONSTANT) {
        return std::nullopt;
    }
    auto literal = StaticCast<const Constant*>(indexExpression)->GetValue();
    if (literal == nullptr) {
        return std::nullopt;
    }
    if (literal->IsBoolLiteral()) {
        return StaticCast<BoolLiteral*>(literal)->GetVal() ? 1U : 0U;
    }
    if (!literal->IsIntLiteral()) {
        return std::nullopt;
    }
    auto domain = SIntDomain::From(*literal);
    return domain.IsSingleValue()
        ? std::optional<uint64_t>{domain.NumericBound().GetSingleElement().UVal()}
        : std::nullopt;
}

std::optional<uint64_t> GetKnownEnumConstructorIndex(RangeDomain& state, Value* source)
{
    std::optional<uint64_t> result;
    const auto mergeIndex = [&result](std::optional<uint64_t> index) {
        if (!index.has_value()) {
            return true;
        }
        if (result.has_value() && result.value() != index.value()) {
            return false;
        }
        result = index;
        return true;
    };

    auto children = state.GetChildren(source);
    if (!children.empty() && !mergeIndex(GetSingleEnumTagRangeValue(state, children.front()))) {
        return std::nullopt;
    }
    for (auto user : source->GetUsers()) {
        if (user->GetExprKind() != ExprKind::FIELD) {
            continue;
        }
        auto field = StaticCast<const Field*>(user);
        auto path = field->GetPath();
        if (path.size() == 1 && path.front() == 0 &&
            !mergeIndex(GetSingleEnumTagRangeValue(state, field->GetResult()))) {
            return std::nullopt;
        }
    }
    return result;
}

std::unique_ptr<ValueRange> GetKnownScalarAggregateRange(RangeDomain& state, Value* value)
{
    if (value == nullptr || value->GetType() == nullptr ||
        (!value->GetType()->IsInteger() && !value->GetType()->IsBoolean())) {
        return nullptr;
    }
    if (auto range = state.CheckAbstractValue(value); range != nullptr) {
        return range->Clone();
    }
    auto expression = GetDefiningExprForWidening(value);
    if (expression == nullptr || expression->GetExprKind() != ExprKind::CONSTANT) {
        return nullptr;
    }
    auto literal = StaticCast<const Constant*>(expression)->GetValue();
    if (literal == nullptr || literal->IsNullLiteral()) {
        return nullptr;
    }
    auto domain = HandleNonNullLiteralValue<RangeValueDomain>(literal);
    auto range = domain.CheckAbsVal();
    return range == nullptr ? nullptr : range->Clone();
}

bool TryHandleEnumTupleTypeCast(RangeDomain& state, const TypeCast* cast)
{
    auto source = cast == nullptr ? nullptr : cast->GetSourceValue();
    auto targetType = cast == nullptr ? nullptr : cast->GetTargetTy();
    if (source == nullptr || source->GetType() == nullptr || !source->GetType()->IsEnum() ||
        targetType == nullptr || !targetType->IsTuple()) {
        return false;
    }
    const auto traceReject = [cast](const char* reason) {
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") == nullptr) {
            return;
        }
        const auto& location = cast->GetDebugLocation();
        std::cerr << "[RangeAnalysisEnumCast] result=reject reason=" << reason
                  << " line=" << location.GetBeginPos().line << '\n';
    };
    auto constructorIndex = GetKnownEnumConstructorIndex(state, source);
    if (!constructorIndex.has_value()) {
        traceReject("unknown-tag");
        return false;
    }

    std::vector<Value*> possibleTuples;
    auto sourceExpression = GetDefiningExprForWidening(source);
    if (sourceExpression != nullptr && sourceExpression->GetExprKind() == ExprKind::TUPLE) {
        possibleTuples.emplace_back(source);
    } else if (sourceExpression != nullptr && sourceExpression->GetExprKind() == ExprKind::LOAD) {
        auto location = StaticCast<const Load*>(sourceExpression)->GetLocation();
        if (!HasOnlyDirectEnumLoadStoreUsers(location)) {
            traceReject("aliased-location");
            return false;
        }
        bool usedConcreteStore = false;
        if (boundedAggregateStores != nullptr) {
            auto concreteStore = boundedAggregateStores->find(location);
            if (concreteStore != boundedAggregateStores->end()) {
                possibleTuples.emplace_back(concreteStore->second);
                usedConcreteStore = true;
            }
        }
        if (!usedConcreteStore) {
            for (auto user : location->GetUsers()) {
                if (user->GetExprKind() != ExprKind::STORE) {
                    continue;
                }
                auto storedValue = StaticCast<const Store*>(user)->GetValue();
                if (!GetEnumConstructorIndex(storedValue).has_value()) {
                    traceReject("unknown-stored-constructor");
                    return false;
                }
                possibleTuples.emplace_back(storedValue);
            }
        }
    } else {
        traceReject("unsupported-source");
        return false;
    }

    auto elementTypes = StaticCast<TupleType*>(targetType)->GetElementTypes();
    if (elementTypes.empty()) {
        traceReject("empty-target");
        return false;
    }
    std::vector<std::unique_ptr<ValueRange>> fieldRanges(elementTypes.size());
    bool foundMatchingConstructor = false;
    for (auto tupleValue : possibleTuples) {
        auto index = GetEnumConstructorIndex(tupleValue);
        if (!index.has_value() || index.value() != constructorIndex.value()) {
            continue;
        }
        auto tupleExpression = StaticCast<const Tuple*>(GetDefiningExprForWidening(tupleValue));
        auto elements = tupleExpression->GetElementValues();
        if (elements.size() < elementTypes.size()) {
            traceReject("short-constructor");
            return false;
        }
        foundMatchingConstructor = true;
        for (size_t fieldIndex = 0; fieldIndex < elementTypes.size(); ++fieldIndex) {
            auto type = elementTypes[fieldIndex];
            if (type == nullptr || (!type->IsInteger() && !type->IsBoolean())) {
                continue;
            }
            auto incoming = GetKnownScalarAggregateRange(state, elements[fieldIndex]);
            if (incoming == nullptr) {
                traceReject("unknown-field");
                return false;
            }
            if (fieldRanges[fieldIndex] == nullptr) {
                fieldRanges[fieldIndex] = std::move(incoming);
            } else if (fieldRanges[fieldIndex]->GetRangeKind() != incoming->GetRangeKind()) {
                traceReject("field-kind-mismatch");
                return false;
            } else if (auto joined = fieldRanges[fieldIndex]->Join(*incoming); joined.has_value()) {
                fieldRanges[fieldIndex] = std::move(joined.value());
            }
        }
    }
    if (!foundMatchingConstructor) {
        traceReject("no-matching-constructor");
        return false;
    }

    auto dest = cast->GetResult();
    state.SetToBound(dest, /* isTop = */ true);
    state.EnsureChildren(dest, elementTypes.size(), [&state, &elementTypes](AbstractObject* child, size_t index) {
        state.SetToTopOrTopRef(child, elementTypes[index]->IsRef());
    });
    auto children = state.GetChildren(dest);
    if (children.size() != elementTypes.size()) {
        traceReject("child-layout");
        return false;
    }
    for (size_t fieldIndex = 0; fieldIndex < fieldRanges.size(); ++fieldIndex) {
        if (fieldRanges[fieldIndex] != nullptr) {
            state.Update(children[fieldIndex], std::move(fieldRanges[fieldIndex]));
        }
    }
    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
        const auto& location = cast->GetDebugLocation();
        std::cerr << "[RangeAnalysisEnumCast] result=success tag=" << constructorIndex.value()
                  << " line=" << location.GetBeginPos().line << '\n';
    }
    return true;
}

std::optional<size_t> GetSingleNonNegativeIndex(const RangeDomain& state, Value* value)
{
    if (value == nullptr) {
        return std::nullopt;
    }
    // Aggregate fields are represented by untyped AbstractObjects. Read the
    // stored scalar domain directly instead of rejecting them via GetType().
    auto domain = state.CheckAbstractValueWithTopBottom(value);
    if (domain == nullptr || domain->IsTop()) {
        return std::nullopt;
    }
    auto abstractValue = domain->CheckAbsVal();
    if (abstractValue == nullptr ||
        abstractValue->GetRangeKind() != ValueRange::RangeKind::SINT) {
        return std::nullopt;
    }
    auto range = StaticCast<const SIntRange*>(abstractValue);
    if (!range->GetVal().IsSingleValue()) {
        return std::nullopt;
    }
    auto index = range->GetVal().NumericBound().GetSingleElement();
    if (index.Slt(0)) {
        return std::nullopt;
    }
    return static_cast<size_t>(index.UVal());
}

bool IsRawArrayRefValue(Value* value)
{
    if (value == nullptr || !value->GetType()->IsRef()) {
        return false;
    }
    return value->GetType()->StripAllRefs()->IsRawArray();
}

bool IsStructArrayValue(Value* value)
{
    if (value == nullptr || value->GetType() == nullptr) {
        return false;
    }
    auto type = value->GetType()->StripAllRefs();
    return type != nullptr && type->IsStructArray();
}

void CopyScalarStateOrTop(RangeDomain& state, Value* source, Value* dest)
{
    if (source == nullptr || dest == nullptr) {
        return;
    }
    auto domain = state.CheckAbstractValueWithTopBottom(source);
    if (domain != nullptr && domain->GetKind() == RangeValueDomain::ValueKind::VAL) {
        state.Update(dest, *domain);
        return;
    }
    state.SetToTopOrTopRef(dest, /* isRef = */ false);
}

void RecordRawArrayLiteralInit(RangeDomain& state, const RawArrayLiteralInit* init)
{
    if (init == nullptr || !IsRawArrayRefValue(init->GetRawArray())) {
        return;
    }
    auto rawObj = state.CheckAbstractObjectRefBy(init->GetRawArray());
    if (rawObj == nullptr) {
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisArrayLiteral] raw-init=no-object\n";
        }
        return;
    }
    auto elements = init->GetElements();
    const auto setChildState = [&state, &elements](AbstractObject* child, size_t index) {
        CopyScalarStateOrTop(state, elements[index], child);
    };
    state.EnsureChildren(rawObj, elements.size(), setChildState);
    auto children = state.GetChildren(rawObj);
    if (children.size() != elements.size()) {
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisArrayLiteral] raw-init=child-count-mismatch elements="
                      << elements.size() << " children=" << children.size() << '\n';
        }
        state.ForgetChildren(rawObj);
        return;
    }
    for (size_t i = 0; i < elements.size(); ++i) {
        CopyScalarStateOrTop(state, elements[i], children[i]);
    }
    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
        std::cerr << "[RangeAnalysisArrayLiteral] raw-init=ready elements=" << elements.size() << '\n';
    }
}

void InvalidateRawArrayLiteral(RangeDomain& state, Value* rawArray)
{
    if (rawArray == nullptr) {
        return;
    }
    if (auto rawObj = state.CheckAbstractObjectRefBy(rawArray); rawObj != nullptr) {
        state.ForgetChildren(rawObj);
    }
    {
        std::lock_guard<std::mutex> lock(aggregateLiteralMtx);
        for (auto it = structArrayLiteralInfos.begin(); it != structArrayLiteralInfos.end();) {
            if (it->second.rawArray == rawArray) {
                it = structArrayLiteralInfos.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void InvalidateStructArrayLiteral(Value* arrayValue)
{
    if (arrayValue == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(aggregateLiteralMtx);
    structArrayLiteralInfos.erase(arrayValue);
}

void ForgetReferenceArgument(RangeDomain& state, Value* arg)
{
    if (arg == nullptr || arg->GetType() == nullptr) {
        return;
    }
    if (IsStructArrayValue(arg)) {
        auto info = LookupStructArrayLiteralInfo(state, arg);
        if (info.has_value()) {
            InvalidateRawArrayLiteral(state, info->rawArray);
        }
        InvalidateStructArrayLiteral(arg);
        if (!arg->GetType()->IsRef()) {
            return;
        }
    }
    if (!arg->GetType()->IsRef()) {
        return;
    }
    if (IsRawArrayRefValue(arg)) {
        InvalidateRawArrayLiteral(state, arg);
    }

    auto baseType = GetRefRootBaseType(arg->GetType());
    auto object = state.CheckAbstractObjectRefBy(arg);
    if (object == nullptr || object->IsTopObjInstance()) {
        state.ClearObjectState();
        return;
    }
    if (baseType == nullptr) {
        state.ClearObjectState();
        return;
    }
    state.ForgetValueAndChildren(arg);
}

std::optional<StructArrayLiteralInfo> LookupStructArrayLiteralInfo(Value* arrayValue)
{
    std::lock_guard<std::mutex> lock(aggregateLiteralMtx);
    auto it = structArrayLiteralInfos.find(arrayValue);
    if (it == structArrayLiteralInfos.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<StructArrayLiteralInfo> LookupStructArrayLiteralInfo(
    const RangeDomain& state, Value* arrayValue)
{
    const bool traceArrayLookup = std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr;
    if (!IsStructArrayValue(arrayValue)) {
        return std::nullopt;
    }
    Value* receiver = arrayValue;
    if (arrayValue->GetType()->IsRef()) {
        receiver = state.CheckAbstractObjectRefBy(arrayValue);
    }
    if (receiver == nullptr) {
        if (traceArrayLookup) {
            std::cerr << "[RangeAnalysisArrayState] recovery=no-receiver\n";
        }
        return std::nullopt;
    }
    auto& mutableState = const_cast<RangeDomain&>(state);
    auto fields = mutableState.GetChildren(receiver);
    if (fields.size() != 3) {
        if (traceArrayLookup) {
            std::cerr << "[RangeAnalysisArrayState] recovery=field-count count="
                      << fields.size() << '\n';
        }
        return std::nullopt;
    }
    auto rawDomain = state.CheckAbstractValueWithTopBottom(fields[0]);
    if (rawDomain == nullptr || rawDomain->GetKind() != RangeValueDomain::ValueKind::REF) {
        if (traceArrayLookup) {
            std::cerr << "[RangeAnalysisArrayState] recovery=raw-domain kind="
                      << (rawDomain == nullptr
                              ? -1
                              : static_cast<int>(rawDomain->GetKind()))
                      << '\n';
        }
        return std::nullopt;
    }
    auto rawObject = state.CheckAbstractObjectRefBy(fields[0]);
    auto start = GetSingleNonNegativeIndex(state, fields[1]);
    auto length = GetSingleNonNegativeIndex(state, fields[2]);
    if (rawObject == nullptr || rawObject->IsTopObjInstance()) {
        if (traceArrayLookup) {
            std::cerr << "[RangeAnalysisArrayState] recovery=metadata raw="
                      << (rawObject != nullptr)
                      << " start=" << (start.has_value() ? std::to_string(*start) : "unknown")
                      << " length=" << (length.has_value() ? std::to_string(*length) : "unknown")
                      << '\n';
        }
        return std::nullopt;
    }
    auto elements = mutableState.GetChildren(rawObject);
    Value* startValue = fields[1];
    Value* lengthValue = fields[2];
    if (!start.has_value() || !length.has_value()) {
        std::vector<StructArrayLiteralInfo> recordedInfos;
        {
            std::lock_guard<std::mutex> lock(aggregateLiteralMtx);
            if (structArrayLiteralInfos.size() > MAX_ARRAY_METADATA_CANDIDATES) {
                if (traceArrayLookup) {
                    std::cerr << "[RangeAnalysisArrayState] recovery=metadata-budget size="
                              << structArrayLiteralInfos.size() << '\n';
                }
                return std::nullopt;
            }
            recordedInfos.reserve(structArrayLiteralInfos.size());
            for (const auto& [recordedArray, recordedInfo] : structArrayLiteralInfos) {
                (void)recordedArray;
                recordedInfos.emplace_back(recordedInfo);
            }
        }
        std::optional<size_t> matchedStart;
        std::optional<size_t> matchedLength;
        Value* matchedStartValue = nullptr;
        Value* matchedLengthValue = nullptr;
        bool conflictingMetadata = false;
        for (const auto& recorded : recordedInfos) {
            if (recorded.rawArray == nullptr ||
                state.CheckAbstractObjectRefBy(recorded.rawArray) != rawObject) {
                continue;
            }
            auto candidateStart = GetSingleNonNegativeIndex(state, recorded.start);
            auto candidateLength = GetSingleNonNegativeIndex(state, recorded.length);
            if (!candidateStart.has_value() || !candidateLength.has_value() ||
                candidateStart.value() > elements.size() ||
                candidateLength.value() > elements.size() - candidateStart.value()) {
                continue;
            }
            if ((start.has_value() && start.value() != candidateStart.value()) ||
                (length.has_value() && length.value() != candidateLength.value())) {
                continue;
            }
            if (!matchedStart.has_value()) {
                matchedStart = candidateStart;
                matchedLength = candidateLength;
                matchedStartValue = recorded.start;
                matchedLengthValue = recorded.length;
                continue;
            }
            if (matchedStart.value() != candidateStart.value() ||
                matchedLength.value() != candidateLength.value()) {
                conflictingMetadata = true;
                break;
            }
        }
        if (!conflictingMetadata && matchedStart.has_value() && matchedLength.has_value()) {
            start = matchedStart;
            length = matchedLength;
            startValue = matchedStartValue;
            lengthValue = matchedLengthValue;
        }
    }
    if (!start.has_value() || !length.has_value()) {
        if (traceArrayLookup) {
            std::cerr << "[RangeAnalysisArrayState] recovery=metadata raw=1 start="
                      << (start.has_value() ? std::to_string(*start) : "unknown")
                      << " length=" << (length.has_value() ? std::to_string(*length) : "unknown")
                      << '\n';
        }
        return std::nullopt;
    }
    if (start.value() > elements.size() ||
        length.value() > elements.size() - start.value()) {
        if (traceArrayLookup) {
            std::cerr << "[RangeAnalysisArrayState] recovery=element-count elements="
                      << elements.size() << " start=" << *start << " length=" << *length << '\n';
        }
        return std::nullopt;
    }
    if (traceArrayLookup) {
        std::cerr << "[RangeAnalysisArrayState] recovery=ready elements="
                  << elements.size() << " start=" << *start << " length=" << *length << '\n';
    }
    return StructArrayLiteralInfo{fields[0], startValue, lengthValue};
}

AbstractObject* LookupRawArrayLiteralElement(RangeDomain& state, Value* rawArray, size_t index)
{
    auto rawObj = state.CheckAbstractObjectRefBy(rawArray);
    if (rawObj == nullptr) {
        return nullptr;
    }
    return state.GetChild(rawObj, index);
}

std::optional<size_t> GetEffectiveArrayLiteralIndex(const RangeDomain& state, Value* indexValue, Value* startValue)
{
    auto index = GetSingleNonNegativeIndex(state, indexValue);
    if (!index.has_value()) {
        return std::nullopt;
    }
    size_t start = 0;
    if (startValue != nullptr) {
        auto startIndex = GetSingleNonNegativeIndex(state, startValue);
        if (!startIndex.has_value()) {
            return std::nullopt;
        }
        start = startIndex.value();
    }
    if (index.value() > std::numeric_limits<size_t>::max() - start) {
        return std::nullopt;
    }
    return start + index.value();
}

bool TryCopyRawArrayLiteralElement(
    RangeDomain& state, Value* dest, Value* rawArray, Value* indexValue, Value* startValue = nullptr)
{
    auto index = GetEffectiveArrayLiteralIndex(state, indexValue, startValue);
    if (!index.has_value()) {
        return false;
    }
    auto element = LookupRawArrayLiteralElement(state, rawArray, index.value());
    if (element == nullptr) {
        return false;
    }
    if (dest != nullptr && dest->GetType() != nullptr && dest->GetType()->IsRef()) {
        state.SetRefToObject(dest, element);
        return true;
    }
    return CopyKnownScalarRange(state, dest, element);
}

Function* GetStructArrayApplyCallee(const Apply* apply)
{
    auto callee = apply == nullptr ? nullptr : DynamicCast<Function*>(apply->GetCallee());
    auto ownerType = callee == nullptr ? nullptr : callee->GetParentCustomTypeOrExtendedType();
    if (callee == nullptr || callee->GetBody() == nullptr || ownerType == nullptr ||
        !ownerType->StripAllRefs()->IsStructArray()) {
        return nullptr;
    }
    return callee;
}

const Expression* GetArrayDefiningExpr(Value* value)
{
    if (value == nullptr || !value->IsLocalVar()) {
        return nullptr;
    }
    return StaticCast<LocalVar*>(value)->GetExpr();
}

std::optional<uint64_t> ResolveDirectCustomFieldIndex(
    Value* base, const std::vector<std::string>& names)
{
    if (base == nullptr || base->GetType() == nullptr || names.size() != 1) {
        return std::nullopt;
    }
    auto customType = DynamicCast<CustomType*>(base->GetType()->StripAllRefs());
    if (customType == nullptr || customType->GetCustomTypeDef() == nullptr) {
        return std::nullopt;
    }
    const auto& fields = customType->GetCustomTypeDef()->GetAllInstanceVars();
    uint64_t index = fields.size();
    for (auto it = fields.crbegin(); it != fields.crend(); ++it) {
        --index;
        if (it->name == names.front()) {
            return index;
        }
    }
    return std::nullopt;
}

bool IsStructArrayFieldValue(Value* value, Value* receiver, uint64_t fieldIndex)
{
    auto expression = GetArrayDefiningExpr(value);
    if (expression == nullptr) {
        return false;
    }
    if (expression->GetExprKind() == ExprKind::FIELD) {
        auto field = StaticCast<const Field*>(expression);
        auto path = field->GetPath();
        return field->GetBase() == receiver && path.size() == 1 && path.front() == fieldIndex;
    }
    if (expression->GetExprKind() == ExprKind::FIELD_BY_NAME) {
        auto field = StaticCast<const FieldByName*>(expression);
        auto index = ResolveDirectCustomFieldIndex(field->GetBase(), field->GetNames());
        return field->GetBase() == receiver && index.has_value() && index.value() == fieldIndex;
    }
    return false;
}

bool IsStructArrayIndexOffset(Value* value, Value* receiver, Value* index)
{
    auto expression = GetArrayDefiningExpr(value);
    if (expression == nullptr || expression->GetExprKind() != ExprKind::ADD) {
        return false;
    }
    auto add = StaticCast<const BinaryExpression*>(expression);
    auto lhs = add->GetLHSOperand();
    auto rhs = add->GetRHSOperand();
    return (IsStructArrayFieldValue(lhs, receiver, 1) && rhs == index) ||
        (IsStructArrayFieldValue(rhs, receiver, 1) && lhs == index);
}

bool IsProvenStructArrayLiteralConstructor(const Apply* apply)
{
    const auto traceReject = [](const char* reason) {
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisArrayLiteral] constructor-reject=" << reason << '\n';
        }
    };
    auto callee = GetStructArrayApplyCallee(apply);
    if (callee == nullptr || !callee->IsConstructor() || callee->GetReturnType() == nullptr ||
        (!callee->GetReturnType()->IsUnit() && !callee->GetReturnType()->IsVoid())) {
        if (apply != nullptr) {
            auto args = apply->GetArgs();
            if (args.size() == 4 && IsStructArrayValue(args[0])) {
                auto directCallee = DynamicCast<Function*>(apply->GetCallee());
                if (directCallee == nullptr) {
                    traceReject("callee-not-function");
                } else if (directCallee->GetBody() == nullptr) {
                    traceReject("callee-without-body");
                } else if (directCallee->GetParentCustomTypeOrExtendedType() == nullptr) {
                    traceReject("callee-without-owner");
                } else if (!directCallee->GetParentCustomTypeOrExtendedType()->StripAllRefs()->IsStructArray()) {
                    traceReject("callee-owner-not-struct-array");
                } else if (!directCallee->IsConstructor()) {
                    traceReject("callee-not-constructor");
                } else if (directCallee->GetReturnType() == nullptr ||
                    (!directCallee->GetReturnType()->IsUnit() &&
                        !directCallee->GetReturnType()->IsVoid())) {
                    traceReject("callee-nonunit");
                    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                        std::cerr << "[RangeAnalysisArrayLiteral] callee-return="
                                  << (directCallee->GetReturnType() == nullptr
                                          ? "<null>"
                                          : directCallee->GetReturnType()->ToString())
                                  << " apply-result="
                                  << (apply->GetResult() == nullptr || apply->GetResult()->GetType() == nullptr
                                          ? "<null>"
                                          : apply->GetResult()->GetType()->ToString())
                                  << '\n';
                    }
                } else {
                    traceReject("callee-shape");
                }
            }
        }
        return false;
    }
    auto args = apply->GetArgs();
    auto params = callee->GetParams();
    if (args.size() != 4 || params.size() != args.size() || !IsStructArrayValue(args[0]) ||
        !IsRawArrayRefValue(args[1]) || args[2] == nullptr || args[3] == nullptr ||
        args[2]->GetType() == nullptr || args[3]->GetType() == nullptr || !args[2]->GetType()->IsInteger() ||
        !args[3]->GetType()->IsInteger()) {
        traceReject("argument-shape");
        return false;
    }

    auto blocks = callee->GetBody()->GetBlocks();
    std::array<bool, 3> storedFields{false, false, false};
    size_t normalExitCount = 0;
    for (auto block : blocks) {
        auto terminator = block->GetTerminator();
        if (terminator == nullptr) {
            if (!block->GetNonTerminatorExpressions().empty()) {
                traceReject("unterminated-nonempty-block");
                return false;
            }
            continue;
        }
        if (terminator->GetExprKind() == ExprKind::EXIT) {
            ++normalExitCount;
        } else if (terminator->GetExprKind() != ExprKind::GOTO) {
            traceReject("unexpected-terminator");
            return false;
        }
        for (auto expression : block->GetNonTerminatorExpressions()) {
            switch (expression->GetExprKind()) {
                case ExprKind::DEBUGEXPR:
                case ExprKind::ALLOCATE:
                case ExprKind::CONSTANT:
                    break;
                case ExprKind::STORE: {
                    auto store = StaticCast<Store*>(expression);
                    if (store->GetLocation() != callee->GetReturnValue()) {
                        traceReject("nonreturn-store");
                        return false;
                    }
                    break;
                }
                case ExprKind::STORE_ELEMENT_REF: {
                    auto store = StaticCast<StoreElementRef*>(expression);
                    auto path = store->GetPath();
                    if (store->GetLocation() != params[0] || path.size() != 1 ||
                        path.front() >= storedFields.size() ||
                        store->GetValue() != params[path.front() + 1] || storedFields[path.front()]) {
                        traceReject("invalid-indexed-field-store");
                        return false;
                    }
                    storedFields[path.front()] = true;
                    break;
                }
                case ExprKind::STORE_ELEMENT_BY_NAME: {
                    auto store = StaticCast<StoreElementByName*>(expression);
                    auto index = ResolveDirectCustomFieldIndex(store->GetLocation(), store->GetNames());
                    if (store->GetLocation() != params[0] || !index.has_value() ||
                        index.value() >= storedFields.size() ||
                        store->GetValue() != params[index.value() + 1] || storedFields[index.value()]) {
                        traceReject("invalid-named-field-store");
                        return false;
                    }
                    storedFields[index.value()] = true;
                    break;
                }
                default:
                    traceReject("unexpected-expression");
                    return false;
            }
        }
    }
    if (normalExitCount != 1) {
        traceReject("normal-exit-count");
        return false;
    }
    if (!std::all_of(storedFields.begin(), storedFields.end(), [](bool stored) { return stored; })) {
        traceReject("missing-field-store");
        return false;
    }
    return true;
}

bool IsExpectedStructArrayIndexIntrinsic(
    const Intrinsic* intrinsic, const std::vector<Parameter*>& params, bool isSetter)
{
    if (intrinsic == nullptr || params.size() != (isSetter ? 3 : 2)) {
        return false;
    }
    auto kind = intrinsic->GetIntrinsicKind();
    if (isSetter) {
        if (kind != IntrinsicKind::ARRAY_SET && kind != IntrinsicKind::ARRAY_SET_UNCHECKED) {
            return false;
        }
    } else if (kind != IntrinsicKind::ARRAY_GET && kind != IntrinsicKind::ARRAY_GET_UNCHECKED) {
        return false;
    }
    const auto& intrinsicArgs = intrinsic->GetArgs();
    if (intrinsicArgs.size() != (isSetter ? 3 : 2) ||
        !IsStructArrayFieldValue(intrinsicArgs[0], params[0], 0) ||
        !IsStructArrayIndexOffset(intrinsicArgs[1], params[0], params[1])) {
        return false;
    }
    return !isSetter || intrinsicArgs[2] == params[2];
}

bool IsProvenStructArrayIndexBody(const Apply* apply, bool isSetter)
{
    auto callee = GetStructArrayApplyCallee(apply);
    auto hasUnitLikeReturn = callee != nullptr && callee->GetReturnType() != nullptr &&
        (callee->GetReturnType()->IsUnit() || callee->GetReturnType()->IsVoid());
    if (callee == nullptr || !callee->TestAttr(Attribute::OPERATOR) || callee->GetReturnType() == nullptr ||
        hasUnitLikeReturn != isSetter) {
        return false;
    }
    auto args = apply->GetArgs();
    auto params = callee->GetParams();
    auto expectedSize = isSetter ? 3U : 2U;
    if (args.size() != expectedSize || params.size() != expectedSize || !IsStructArrayValue(args[0]) ||
        args[1] == nullptr || args[1]->GetType() == nullptr || !args[1]->GetType()->IsInteger()) {
        return false;
    }

    auto blocks = callee->GetBody()->GetBlocks();
    const Intrinsic* arrayIntrinsic = nullptr;
    size_t normalExitCount = 0;
    bool returnsIntrinsicResult = isSetter;
    for (auto block : blocks) {
        auto terminator = block->GetTerminator();
        if (terminator == nullptr) {
            if (!block->GetNonTerminatorExpressions().empty()) {
                return false;
            }
            continue;
        }
        if (terminator->GetExprKind() == ExprKind::RAISE_EXCEPTION) {
            continue;
        }
        if (terminator->GetExprKind() == ExprKind::EXIT) {
            ++normalExitCount;
        } else if (terminator->GetExprKind() != ExprKind::BRANCH &&
            terminator->GetExprKind() != ExprKind::GOTO) {
            return false;
        }
        for (auto expression : block->GetNonTerminatorExpressions()) {
            switch (expression->GetExprKind()) {
                case ExprKind::DEBUGEXPR:
                case ExprKind::ALLOCATE:
                case ExprKind::CONSTANT:
                case ExprKind::TYPECAST:
                case ExprKind::FIELD:
                case ExprKind::FIELD_BY_NAME:
                case ExprKind::ADD:
                case ExprKind::GE:
                    break;
                case ExprKind::INTRINSIC: {
                    auto intrinsic = StaticCast<Intrinsic*>(expression);
                    if (arrayIntrinsic != nullptr || !IsExpectedStructArrayIndexIntrinsic(intrinsic, params, isSetter)) {
                        return false;
                    }
                    arrayIntrinsic = intrinsic;
                    break;
                }
                case ExprKind::STORE: {
                    auto store = StaticCast<Store*>(expression);
                    if (store->GetLocation() != callee->GetReturnValue()) {
                        return false;
                    }
                    if (!isSetter && arrayIntrinsic != nullptr && store->GetValue() == arrayIntrinsic->GetResult()) {
                        returnsIntrinsicResult = true;
                    }
                    break;
                }
                default:
                    return false;
            }
        }
    }
    return normalExitCount == 1 && arrayIntrinsic != nullptr && returnsIntrinsicResult;
}

bool IsKnownStructArrayIndexInBounds(
    const RangeDomain& state, Value* indexValue, const StructArrayLiteralInfo& info)
{
    auto index = GetSingleNonNegativeIndex(state, indexValue);
    auto length = GetSingleNonNegativeIndex(state, info.length);
    return index.has_value() && length.has_value() && index.value() < length.value();
}

bool TryHandleStructArrayLiteralConstructor(RangeDomain& state, const Apply* apply)
{
    if (!IsProvenStructArrayLiteralConstructor(apply)) {
        return false;
    }
    auto args = apply->GetArgs();
    auto receiver = state.CheckAbstractObjectRefBy(args[0]);
    if (receiver == nullptr || receiver->IsTopObjInstance()) {
        return false;
    }
    auto fields = state.GetChildren(receiver);
    if (fields.size() != 3) {
        return false;
    }
    for (size_t i = 0; i < fields.size(); ++i) {
        state.Propagate(args[i + 1], fields[i]);
    }
    {
        std::lock_guard<std::mutex> lock(aggregateLiteralMtx);
        structArrayLiteralInfos[args[0]] = StructArrayLiteralInfo{args[1], args[2], args[3]};
    }
    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
        std::cerr << "[RangeAnalysisArrayLiteral] constructor=handled\n";
    }
    return true;
}

bool TryUpdateRawArrayLiteralElement(RangeDomain& state, Value* rawArray, size_t index, Value* value)
{
    auto element = LookupRawArrayLiteralElement(state, rawArray, index);
    if (element == nullptr || value == nullptr) {
        return false;
    }
    CopyScalarStateOrTop(state, value, element);
    return true;
}

bool TryHandleStructArrayLiteralMutation(RangeDomain& state, const Apply* apply)
{
    if (!IsProvenStructArrayIndexBody(apply, /* isSetter = */ true)) {
        return false;
    }
    auto args = apply->GetArgs();
    auto info = LookupStructArrayLiteralInfo(state, args[0]);
    if (!info.has_value()) {
        return false;
    }
    auto index = IsKnownStructArrayIndexInBounds(state, args[1], *info)
        ? GetEffectiveArrayLiteralIndex(state, args[1], info->start)
        : std::nullopt;
    if (index.has_value() && TryUpdateRawArrayLiteralElement(state, info->rawArray, index.value(), args[2])) {
        return true;
    }
    InvalidateRawArrayLiteral(state, info->rawArray);
    state.ForgetValueAndChildren(args[0]);
    InvalidateStructArrayLiteral(args[0]);
    return false;
}

void PropagateArrayLiteralInfoOnLoad(const Load* load)
{
    if (load == nullptr || !IsStructArrayValue(load->GetResult())) {
        return;
    }
    std::lock_guard<std::mutex> lock(aggregateLiteralMtx);
    auto it = structArrayLiteralInfos.find(load->GetLocation());
    if (it != structArrayLiteralInfos.end()) {
        structArrayLiteralInfos[load->GetResult()] = it->second;
    }
}

void PropagateArrayLiteralInfoOnStore(const Store* store)
{
    if (store == nullptr || !IsStructArrayValue(store->GetLocation())) {
        return;
    }
    auto info = LookupStructArrayLiteralInfo(store->GetValue());
    std::lock_guard<std::mutex> lock(aggregateLiteralMtx);
    if (info.has_value()) {
        structArrayLiteralInfos[store->GetLocation()] = *info;
    } else {
        structArrayLiteralInfos.erase(store->GetLocation());
    }
}

bool TryHandleArrayLiteralIndexApply(RangeDomain& state, const Apply* apply)
{
    if (!IsProvenStructArrayIndexBody(apply, /* isSetter = */ false)) {
        return false;
    }
    auto args = apply->GetArgs();
    auto info = LookupStructArrayLiteralInfo(state, args[0]);
    if (!info.has_value()) {
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisArrayLiteral] index=no-literal-info\n";
        }
        return false;
    }
    if (!IsKnownStructArrayIndexInBounds(state, args[1], *info)) {
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisArrayLiteral] index=unproven-bounds\n";
        }
        return false;
    }
    auto copied =
        TryCopyRawArrayLiteralElement(state, apply->GetResult(), info->rawArray, args[1], info->start);
    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
        std::cerr << "[RangeAnalysisArrayLiteral] index=" << (copied ? "copied" : "missing-element") << '\n';
    }
    return copied;
}

bool TryHandleRawArrayGetFromLiteral(RangeDomain& state, const Intrinsic* intrinsic)
{
    auto kind = intrinsic->GetIntrinsicKind();
    if (kind != CHIR::IntrinsicKind::ARRAY_GET && kind != CHIR::IntrinsicKind::ARRAY_GET_UNCHECKED) {
        return false;
    }
    const auto& args = intrinsic->GetArgs();
    if (args.size() < 2 || !IsRawArrayRefValue(args[0]) || !args[1]->GetType()->IsInteger()) {
        return false;
    }
    return TryCopyRawArrayLiteralElement(state, intrinsic->GetResult(), args[0], args[1]);
}

bool TryInvalidateRawArrayLiteralWrite(RangeDomain& state, const Intrinsic* intrinsic)
{
    auto kind = intrinsic->GetIntrinsicKind();
    if (kind != CHIR::IntrinsicKind::ARRAY_SET && kind != CHIR::IntrinsicKind::ARRAY_SET_UNCHECKED) {
        return false;
    }
    const auto& args = intrinsic->GetArgs();
    if (args.size() >= 3 && IsRawArrayRefValue(args[0]) && args[1]->GetType()->IsInteger()) {
        auto index = GetSingleNonNegativeIndex(state, args[1]);
        if (index.has_value() && TryUpdateRawArrayLiteralElement(state, args[0], index.value(), args[2])) {
            return true;
        }
    }
    if (!args.empty()) {
        InvalidateRawArrayLiteral(state, args[0]);
    }
    return true;
}

bool TryHandleVArrayGetFromLiteral(RangeDomain& state, const Intrinsic* intrinsic)
{
    if (intrinsic->GetIntrinsicKind() != CHIR::IntrinsicKind::VARRAY_GET) {
        return false;
    }
    const auto& args = intrinsic->GetArgs();
    if (args.size() < 2) {
        return false;
    }
    auto index = GetSingleNonNegativeIndex(state, args[1]);
    if (!index.has_value()) {
        return false;
    }
    auto element = GetAggregateLiteralElement(args[0], index.value());
    return CopyKnownScalarRange(state, intrinsic->GetResult(), element);
}

ClassType* ResolveExactAllocatedClass(Value* value, std::unordered_set<Value*>& visited, unsigned depth)
{
    constexpr unsigned MAX_CLASS_DEF_DEPTH = 16;
    if (value == nullptr || depth >= MAX_CLASS_DEF_DEPTH || !visited.emplace(value).second || !value->IsLocalVar()) {
        return nullptr;
    }
    auto expression = StaticCast<LocalVar*>(value)->GetExpr();
    if (expression == nullptr) {
        return nullptr;
    }
    switch (expression->GetExprKind()) {
        case ExprKind::ALLOCATE: {
            auto allocatedType = StaticCast<Allocate*>(expression)->GetType();
            return allocatedType != nullptr && allocatedType->IsClass()
                ? StaticCast<ClassType*>(allocatedType)
                : nullptr;
        }
        case ExprKind::TYPECAST:
            return ResolveExactAllocatedClass(
                StaticCast<TypeCast*>(expression)->GetSourceValue(), visited, depth + 1);
        case ExprKind::LOAD:
            return ResolveExactAllocatedClass(StaticCast<Load*>(expression)->GetLocation(), visited, depth + 1);
        default:
            return nullptr;
    }
}

template <typename TInvoke>
Function* ResolveExactInvokeTargetImpl(const TInvoke* invoke, ClassType* exactClass, CHIRBuilder& builder)
{
    if (invoke == nullptr || exactClass == nullptr) {
        return nullptr;
    }
    auto sourceParent = invoke->GetInstSrcParentCustomTypeOfMethod(builder);
    if (sourceParent == nullptr) {
        return nullptr;
    }
    const auto& vtable =
        exactClass->GetClassDef()->GetDefVTable().GetExpectedTypeVTable(*sourceParent);
    const auto& methods = vtable.GetVirtualMethods();
    auto offset = invoke->GetVirtualMethodOffset(&builder);
    if (offset < methods.size() &&
        methods[offset].GetMethodName() == invoke->GetMethodName()) {
        auto target = methods[offset].GetVirtualMethod();
        if (target != nullptr && target->IsFuncWithBody() &&
            !target->IsPureAbstract()) {
            return target;
        }
    }

    // Instantiated generic parent types are not always pointer-identical to
    // the source-parent key stored in the concrete class vtable. Reuse CHIR's
    // signature-aware lookup so generic substitutions and inherited slots are
    // resolved with the same rules as normal devirtualization.
    std::vector<Type*> parameterTypes;
    for (auto argument : invoke->GetArgs()) {
        if (argument == nullptr || argument->GetType() == nullptr) {
            return nullptr;
        }
        parameterTypes.emplace_back(argument->GetType());
    }
    if (!invoke->IsInvokeStaticBase()) {
        if (parameterTypes.empty()) {
            return nullptr;
        }
        parameterTypes.erase(parameterTypes.begin());
    }
    auto callType = builder.GetType<FuncType>(parameterTypes, builder.GetUnitTy());
    FuncCallType signature{
        invoke->GetMethodName(), callType, invoke->GetInstantiatedTypeArgs()};
    auto candidates = exactClass->GetFuncIndexInVTable(signature, builder);
    if (candidates.empty()) {
        return nullptr;
    }
    auto target = candidates.front().instance;
    return target != nullptr && target->IsFuncWithBody() &&
            !target->IsPureAbstract()
        ? target
        : nullptr;
}

Function* ResolveExactInvokeTarget(const Invoke* invoke, ClassType* exactClass, CHIRBuilder& builder)
{
    return ResolveExactInvokeTargetImpl(invoke, exactClass, builder);
}

Function* ResolveExactInvokeTarget(
    const InvokeWithException* invoke, ClassType* exactClass, CHIRBuilder& builder)
{
    return ResolveExactInvokeTargetImpl(invoke, exactClass, builder);
}

void RangeAnalysis::PreHandleFieldExpr(RangeDomain& state, const Field* field)
{
    if (field != nullptr && field->GetBase() != nullptr && field->GetPath().size() == 1) {
        auto base = field->GetBase();
        std::vector<AbstractObject*> children;
        if (base->GetType() != nullptr && base->GetType()->IsRef()) {
            if (auto object = state.CheckAbstractObjectRefBy(base); object != nullptr) {
                children = state.GetChildren(object);
            }
        } else {
            children = state.GetChildren(base);
        }
        auto index = static_cast<size_t>(field->GetPath().front());
        if (index < children.size()) {
            state.Propagate(children[index], field->GetResult());
            return;
        }
    }
    ValueAnalysis<RangeValueDomain>::PreHandleFieldExpr(state, field);
}

void RangeAnalysis::HandleOthersExpr(RangeDomain& state, const Expression* expression)
{
    switch (expression->GetExprKind()) {
        case ExprKind::TYPECAST: {
            auto cast = StaticCast<const TypeCast*>(expression);
            if (!TryHandleEnumTupleTypeCast(state, cast)) {
                HandleTypeCast(state, cast);
            }
            break;
        }
        case ExprKind::FIELD:
            if (!TryHandleFieldFromAggregateLiteral(state, StaticCast<const Field*>(expression))) {
                return;
            }
            break;
        case ExprKind::INTRINSIC:
            if (!TryHandleVArrayGetFromLiteral(state, StaticCast<const Intrinsic*>(expression)) &&
                !TryHandleRawArrayGetFromLiteral(state, StaticCast<const Intrinsic*>(expression))) {
                if (TryInvalidateRawArrayLiteralWrite(state, StaticCast<const Intrinsic*>(expression))) {
                    return;
                }
                auto dest = expression->GetResult();
                if (dest == nullptr) {
                    return;
                }
                return state.SetToTopOrTopRef(dest, dest->GetType()->IsRef());
            }
            break;
        case ExprKind::RAW_ARRAY_ALLOCATE:
            return;
        case ExprKind::RAW_ARRAY_LITERAL_INIT:
            RecordRawArrayLiteralInit(state, StaticCast<const RawArrayLiteralInit*>(expression));
            return;
        case ExprKind::INVOKESTATIC: {
            auto invoke = StaticCast<const InvokeStatic*>(expression);
            RecordUnmodeledCallContext(analysisContextKey, expression);
            HavocCallEffects(state, invoke->GetArgs(), invoke->GetResult());
            return;
        }
        case ExprKind::INVOKE: {
            auto invoke = StaticCast<const Invoke*>(expression);
            if (!HandleFiniteInvokeTargets(state, expression, invoke)) {
                RecordUnmodeledCallContext(analysisContextKey, expression);
                HavocCallEffects(state, invoke->GetArgs(), invoke->GetResult());
            }
            return;
        }
        case ExprKind::CONSTANT:
        case ExprKind::APPLY:
            return;
        case ExprKind::TUPLE:
        default: {
            auto dest = expression->GetResult();
            if (dest == nullptr) {
                return;
            }
            return state.SetToTopOrTopRef(dest, dest->GetType()->IsRef());
        }
    }
}

namespace {
// 判断是否是能产生分支约束的比较表达式。
bool IsRelationalExprKind(ExprKind kind)
{
    switch (kind) {
        case ExprKind::LT:
        case ExprKind::LE:
        case ExprKind::GT:
        case ExprKind::GE:
        case ExprKind::EQUAL:
        case ExprKind::NOTEQUAL:
            return true;
        default:
            return false;
    }
}

// 将 CHIR 比较表达式类型转换为值域关系枚举。
RelationalOperation ToRelationalOperation(ExprKind kind)
{
    switch (kind) {
        case ExprKind::LT:
            return RelationalOperation::LT;
        case ExprKind::LE:
            return RelationalOperation::LE;
        case ExprKind::GT:
            return RelationalOperation::GT;
        case ExprKind::GE:
            return RelationalOperation::GE;
        case ExprKind::EQUAL:
            return RelationalOperation::EQ;
        case ExprKind::NOTEQUAL:
            return RelationalOperation::NE;
        default:
            CJC_ABORT();
            return RelationalOperation::NE;
    }
}

// 获取关系的逻辑取反形式，用于 false 边。
RelationalOperation NegateRelation(RelationalOperation rel)
{
    switch (rel) {
        case RelationalOperation::LT:
            return RelationalOperation::GE;
        case RelationalOperation::LE:
            return RelationalOperation::GT;
        case RelationalOperation::GT:
            return RelationalOperation::LE;
        case RelationalOperation::GE:
            return RelationalOperation::LT;
        case RelationalOperation::EQ:
            return RelationalOperation::NE;
        case RelationalOperation::NE:
            return RelationalOperation::EQ;
    }
}

// 交换左右操作数时同步转换关系方向。
RelationalOperation SwapRelation(RelationalOperation rel)
{
    switch (rel) {
        case RelationalOperation::LT:
            return RelationalOperation::GT;
        case RelationalOperation::LE:
            return RelationalOperation::GE;
        case RelationalOperation::GT:
            return RelationalOperation::LT;
        case RelationalOperation::GE:
            return RelationalOperation::LE;
        case RelationalOperation::EQ:
        case RelationalOperation::NE:
            return rel;
    }
}

bool CanReachBlock(const Block* start, const Block* target, std::unordered_set<const Block*>& visited);
bool IsLoopBranch(const Branch* branch);
bool NarrowSIntByRelationToConstant(RangeDomain& state, Value* value, RelationalOperation rel, const SInt& constant);
std::optional<int64_t> GetUpdateStepFromLocation(Value* value, Value* location);
std::optional<int64_t> FindIncomingSignedStoreConstantThroughPredecessors(const Block* header, Value* location);
std::unordered_set<const Block*> CollectLoopBackPathBlockSet(
    const Branch* branch, const Block* exitSuccessor, const Block* header);
std::optional<int64_t> FindIncomingSignedStoreValueBeforeLoop(const RangeDomain& state,
    const Block* header, Value* location, const std::unordered_set<const Block*>& loopBlocks);
std::optional<SIntRange> BuildSignedExactRange(Type* type, const std::vector<int64_t>& values);
bool ApplyConditionConstraint(RangeDomain& state, Value* condition, bool branchCondition);
Type* GetIntegerRefRootType(Value* location);

constexpr size_t MAX_BOUNDED_LOOP_STEPS = 16384;

// 获取局部 SSA 值的定义表达式。
const Expression* GetDefiningExpr(Value* value)
{
    if (value == nullptr || !value->IsLocalVar()) {
        return nullptr;
    }
    return StaticCast<LocalVar*>(value)->GetExpr();
}

// 判断值是否可被 RangeDomain 状态跟踪。
bool IsStateTrackedValue(Value* value)
{
    return value != nullptr && (value->IsLocalVar() || value->IsParameter());
}

// 判断值是否是可跟踪的整数值。
bool IsIntegerValue(Value* value)
{
    return IsStateTrackedValue(value) && value->GetType()->IsInteger();
}

// 判断值是否是可跟踪的布尔值。
bool IsBooleanValue(Value* value)
{
    return IsStateTrackedValue(value) && value->GetType()->IsBoolean();
}

// 将布尔值收窄到分支期望的真假值。
bool NarrowBoolValue(RangeDomain& state, Value* value, bool expected)
{
    if (!IsBooleanValue(value)) {
        return true;
    }
    auto current = RangeAnalysis::GetBoolDomainFromState(state, value);
    if (current.IsSingleValue() && current.GetSingleValue() != expected) {
        state.Update(value, std::make_unique<BoolRange>(BoolDomain::Bottom()));
        return true;
    }
    state.Update(value, std::make_unique<BoolRange>(BoolDomain::FromBool(expected)));
    return true;
}

// 对两个整数域求交，必要时用 unsigned 语义重试。
SIntDomain IntersectForNarrowing(const SIntDomain& current, const SIntDomain& constraint)
{
    auto narrowed = SIntDomain::Intersects(current, constraint);
    if (!narrowed.IsBottom() || !current.IsUnsigned() || !constraint.IsUnsigned() ||
        current.Width() != constraint.Width()) {
        return narrowed;
    }

    auto unsignedNumeric =
        current.NumericBound().IntersectWith(constraint.NumericBound(), PreferredRangeType::Unsigned);
    if (unsignedNumeric.IsEmptySet()) {
        return SIntDomain::Bottom(current.Width(), current.IsUnsigned());
    }
    return SIntDomain{unsignedNumeric, current.IsUnsigned()};
}

std::optional<std::vector<SInt>> FilterExactValuesByConstraint(
    const std::optional<std::vector<SInt>>& exactValues, const SIntDomain& constraint)
{
    if (!exactValues.has_value()) {
        return std::nullopt;
    }
    std::vector<SInt> filtered;
    for (const auto& value : *exactValues) {
        auto single = SIntDomain{ConstantRange{value}, constraint.IsUnsigned()};
        if (!SIntDomain::Intersects(single, constraint).IsBottom()) {
            filtered.emplace_back(value);
        }
    }
    return NormalizeExactIntSet(std::move(filtered));
}

// 将可跟踪整数值与约束域求交以完成收窄。
bool NarrowSIntValue(RangeDomain& state, Value* value, const SIntDomain& constraint)
{
    if (!IsIntegerValue(value)) {
        return true;
    }
    const auto* currentRange = GetSIntRangeFromState(state, value);
    const auto& current = RangeAnalysis::GetSIntDomainFromState(state, value);
    auto narrowed = IntersectForNarrowing(current, constraint);
    if (narrowed.IsBottom()) {
        auto type = value->GetType();
        state.Update(value,
            std::make_unique<SIntRange>(SIntDomain::Bottom(ToWidth(*type), type->IsUnsignedInteger())));
        return true;
    }
    auto exactValues =
        currentRange == nullptr ? std::nullopt : FilterExactValuesByConstraint(currentRange->GetExactValues(), constraint);
    state.Update(value, std::make_unique<SIntRange>(std::move(narrowed), std::move(exactValues),
        currentRange == nullptr ? std::nullopt : currentRange->GetCongruence(),
        currentRange == nullptr ? std::nullopt : currentRange->GetKnownBits(),
        currentRange == nullptr ? std::nullopt : currentRange->GetExcludedValues(),
        currentRange == nullptr ? std::nullopt : currentRange->GetIntervalFragments()));
    return true;
}

// 将 load 的收窄结果反向传播到其引用的整数对象。
bool NarrowLoadedSIntLocation(RangeDomain& state, Value* value, const SIntDomain& constraint)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::LOAD) {
        return true;
    }
    auto location = StaticCast<const Load*>(expr)->GetLocation();
    if (location == nullptr || !location->GetType()->IsRef()) {
        return true;
    }
    auto refType = StaticCast<RefType*>(location->GetType());
    auto rootType = refType->GetRootBaseType();
    if (rootType == nullptr || !rootType->IsInteger() || ToWidth(*rootType) != constraint.Width()) {
        return true;
    }
    auto object = state.CheckAbstractObjectRefBy(location);
    if (object == nullptr) {
        return true;
    }
    const auto& current = GetSIntDomainFromState(state, object, rootType);
    const auto* currentRange = GetSIntRangeFromState(state, object, rootType);
    auto narrowed = IntersectForNarrowing(current, constraint);
    if (narrowed.IsBottom()) {
        state.Update(object,
            std::make_unique<SIntRange>(SIntDomain::Bottom(ToWidth(*rootType), rootType->IsUnsignedInteger())));
        return true;
    }
    state.Update(object, std::make_unique<SIntRange>(std::move(narrowed),
        currentRange == nullptr ? std::nullopt
                                : FilterExactValuesByConstraint(
                                      currentRange->GetExactValues(), constraint),
        currentRange == nullptr ? std::nullopt : currentRange->GetCongruence(),
        currentRange == nullptr ? std::nullopt : currentRange->GetKnownBits(),
        currentRange == nullptr ? std::nullopt : currentRange->GetExcludedValues(),
        currentRange == nullptr ? std::nullopt : currentRange->GetIntervalFragments()));
    return true;
}

std::unique_ptr<SIntRange> ExcludeSIntConstant(
    const SIntDomain& current, const SIntRange* currentRange, const SInt& constant)
{
    if (!ExactValueSatisfiesDomain(constant, current)) {
        if (currentRange != nullptr) {
            auto clone = currentRange->Clone();
            return std::unique_ptr<SIntRange>(static_cast<SIntRange*>(clone.release()));
        }
        return std::make_unique<SIntRange>(current);
    }
    if (current.IsSingleValue()) {
        return std::make_unique<SIntRange>(SIntDomain::Bottom(current.Width(), current.IsUnsigned()));
    }
    std::optional<std::vector<SInt>> exactValues =
        currentRange == nullptr ? std::nullopt : currentRange->GetExactValues();
    if (exactValues.has_value()) {
        exactValues->erase(std::remove(exactValues->begin(), exactValues->end(), constant), exactValues->end());
        if (exactValues->empty()) {
            return std::make_unique<SIntRange>(SIntDomain::Bottom(current.Width(), current.IsUnsigned()));
        }
        exactValues = NormalizeExactIntSet(std::move(*exactValues));
    }
    std::vector<SInt> excluded;
    if (currentRange != nullptr && currentRange->GetExcludedValues().has_value()) {
        excluded = *currentRange->GetExcludedValues();
    }
    excluded.emplace_back(constant);
    auto excludedValues = NormalizeExcludedSIntValues(std::move(excluded), current);
    return std::make_unique<SIntRange>(current, std::move(exactValues),
        currentRange == nullptr ? std::nullopt : currentRange->GetCongruence(),
        currentRange == nullptr ? std::nullopt : currentRange->GetKnownBits(),
        std::move(excludedValues));
}

bool ExcludeLoadedSIntConstant(RangeDomain& state, Value* value, const SInt& constant)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::LOAD) {
        return true;
    }
    auto location = StaticCast<const Load*>(expr)->GetLocation();
    if (location == nullptr || !location->GetType()->IsRef()) {
        return true;
    }
    auto rootType = StaticCast<RefType*>(location->GetType())->GetRootBaseType();
    if (rootType == nullptr || !rootType->IsInteger() || ToWidth(*rootType) != constant.Width()) {
        return true;
    }
    auto object = state.CheckAbstractObjectRefBy(location);
    if (object == nullptr) {
        return true;
    }
    const auto& current = GetSIntDomainFromState(state, object, rootType);
    const auto* currentRange = GetSIntRangeFromState(state, object, rootType);
    state.Update(object, ExcludeSIntConstant(current, currentRange, constant));
    return true;
}

// 按照与常量的关系收窄整数值。
bool NarrowSIntByRelationToConstant(RangeDomain& state, Value* value, RelationalOperation rel, const SInt& constant)
{
    auto type = value->GetType();
    if (rel == RelationalOperation::NE) {
        const auto& current = RangeAnalysis::GetSIntDomainFromState(state, value);
        const auto* currentRange = GetSIntRangeFromState(state, value);
        state.Update(value, ExcludeSIntConstant(current, currentRange, constant));
        return ExcludeLoadedSIntConstant(state, value, constant);
    }
    auto constraint = SIntDomain::FromNumeric(rel, constant, type->IsUnsignedInteger());
    if (!NarrowSIntValue(state, value, constraint)) {
        return false;
    }
    if (!NarrowLoadedSIntLocation(state, value, constraint)) {
        return false;
    }
    auto expr = GetDefiningExpr(value);
    if (expr != nullptr && expr->GetExprKind() == ExprKind::TYPECAST) {
        auto source = StaticCast<const TypeCast*>(expr)->GetSourceValue();
        auto sourceType = source == nullptr ? nullptr : source->GetType();
        if (sourceType != nullptr && sourceType->IsInteger() && type->IsInteger() &&
            sourceType->IsUnsignedInteger() == type->IsUnsignedInteger() &&
            ToWidth(*sourceType) < ToWidth(*type)) {
            auto sourceWidth = ToWidth(*sourceType);
            if (sourceType->IsUnsignedInteger()) {
                if (constant.UVal() <= SInt::UMaxValue(sourceWidth).UVal()) {
                    return NarrowSIntByRelationToConstant(
                        state, source, rel, SInt{sourceWidth, constant.UVal()});
                }
            } else if (constant.SVal() >= SInt::SMinValue(sourceWidth).SVal() &&
                constant.SVal() <= SInt::SMaxValue(sourceWidth).SVal()) {
                return NarrowSIntByRelationToConstant(
                    state, source, rel, SInt{sourceWidth, static_cast<uint64_t>(constant.SVal())});
            }
        }
    }
    return true;
}

// 从定义常量中读取单点整数值。
std::optional<SInt> GetSingleIntFromDefiningConstant(Value* value)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::CONSTANT) {
        return std::nullopt;
    }
    auto literal = StaticCast<const Constant*>(expr)->GetValue();
    if (literal == nullptr || !literal->IsIntLiteral()) {
        return std::nullopt;
    }
    auto domain = SIntDomain::From(*literal);
    if (!domain.IsSingleValue()) {
        return std::nullopt;
    }
    return domain.NumericBound().GetSingleElement();
}

// 从定义常量中读取单点布尔值。
std::optional<bool> GetSingleBoolFromDefiningConstant(Value* value)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::CONSTANT) {
        return std::nullopt;
    }
    auto literal = StaticCast<const Constant*>(expr)->GetValue();
    if (literal == nullptr || !literal->IsBoolLiteral()) {
        return std::nullopt;
    }
    return StaticCast<BoolLiteral*>(literal)->GetVal();
}

// 优先从状态读取单点布尔值，失败时回退到定义常量。
std::optional<bool> GetSingleBoolFromStateOrConstant(const RangeDomain& state, Value* value)
{
    auto domain = RangeAnalysis::GetBoolDomainFromState(state, value);
    if (domain.IsSingleValue()) {
        return domain.GetSingleValue();
    }
    return GetSingleBoolFromDefiningConstant(value);
}

// 判断某个值是否是从指定 ref 位置 load 出来的。
bool IsLoadFromLocation(Value* value, Value* location)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr) {
        return false;
    }
    if (expr->GetExprKind() == ExprKind::TYPECAST) {
        auto source = StaticCast<const TypeCast*>(expr)->GetSourceValue();
        return source != nullptr && source->GetType()->IsInteger() && value->GetType()->IsInteger() &&
            source->GetType()->IsUnsignedInteger() == value->GetType()->IsUnsignedInteger() &&
            ToWidth(*source->GetType()) <= ToWidth(*value->GetType()) && IsLoadFromLocation(source, location);
    }
    return expr->GetExprKind() == ExprKind::LOAD && StaticCast<const Load*>(expr)->GetLocation() == location;
}

// 读取可用于单调更新判断的非负整数常量。
std::optional<SInt> GetNonNegativeDefiningConstant(Value* value)
{
    auto constant = GetSingleIntFromDefiningConstant(value);
    if (!constant.has_value()) {
        return std::nullopt;
    }
    if (value->GetType()->IsUnsignedInteger() || constant->IsNonNeg()) {
        return constant;
    }
    return std::nullopt;
}

// 判断存储值是否来自同一位置加非负常量。
bool IsNonDecreasingValueFromLocation(Value* value, Value* location)
{
    if (IsLoadFromLocation(value, location)) {
        return true;
    }
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::ADD) {
        return false;
    }
    auto binary = StaticCast<const BinaryExpression*>(expr);
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    return (IsLoadFromLocation(lhs, location) && GetNonNegativeDefiningConstant(rhs).has_value()) ||
        (IsLoadFromLocation(rhs, location) && GetNonNegativeDefiningConstant(lhs).has_value());
}

// 判断前驱是否能回到 header，从而构成回边。
// 在不穿过 header 的前提下检查可达性，用于区分内层循环 preheader 和真正回边。
bool CanReachBlockAvoidingHeader(
    const Block* start, const Block* target, const Block* header, std::unordered_set<const Block*>& visited)
{
    if (start == nullptr || target == nullptr || start == header || !visited.emplace(start).second) {
        return false;
    }
    if (start == target) {
        return true;
    }
    for (auto successor : start->GetSuccessors()) {
        if (CanReachBlockAvoidingHeader(successor, target, header, visited)) {
            return true;
        }
    }
    return false;
}

// 判断 header 的某个后继是否是能回到 header 的循环体入口。
bool IsLoopBodySuccessor(const Branch* branch, const Block* successor)
{
    auto header = branch == nullptr ? nullptr : branch->GetParentBlock();
    if (auto forest = GetLoopForest(header);
        forest != nullptr && forest->FindByHeader(header) != nullptr) {
        return forest->IsLoopBodySuccessor(header, successor);
    }
    std::unordered_set<const Block*> visited;
    return CanReachBlock(successor, header, visited);
}

bool IsBackedgePredecessor(const Block* header, const Block* pred)
{
    if (auto forest = GetLoopForest(header);
        forest != nullptr && forest->FindByHeader(header) != nullptr) {
        return forest->IsBackedge(pred, header);
    }
    if (header != nullptr && pred == header) {
        const auto successors = header->GetSuccessors();
        return std::find(successors.begin(), successors.end(), header) != successors.end();
    }
    auto terminator = header == nullptr ? nullptr : header->GetTerminator();
    if (terminator != nullptr && terminator->GetExprKind() == ExprKind::BRANCH) {
        auto branch = StaticCast<const Branch*>(terminator);
        bool hasLoopBodySuccessor = false;
        std::vector<const Block*> successors{branch->GetTrueBlock(), branch->GetFalseBlock()};
        for (auto successor : successors) {
            if (!IsLoopBodySuccessor(branch, successor)) {
                continue;
            }
            hasLoopBodySuccessor = true;
            std::unordered_set<const Block*> visited;
            if (CanReachBlockAvoidingHeader(successor, pred, header, visited)) {
                return true;
            }
        }
        if (hasLoopBodySuccessor) {
            return false;
        }
    }
    std::unordered_set<const Block*> visited;
    return CanReachBlock(header, pred, visited);
}

// 检查所有回边 store 是否都是非递减更新。
bool HasNonDecreasingBackedgeStore(const Block* header, Value* location)
{
    if (header == nullptr || location == nullptr) {
        return false;
    }

    std::vector<const Block*> worklist;
    for (auto pred : header->GetPredecessors()) {
        if (IsBackedgePredecessor(header, pred)) {
            worklist.emplace_back(pred);
        }
    }
    if (worklist.empty()) {
        return false;
    }

    std::unordered_set<const Block*> visited;
    for (size_t index = 0; index < worklist.size(); ++index) {
        auto block = worklist[index];
        if (block == nullptr || block == header || !visited.emplace(block).second) {
            continue;
        }
        for (auto expr : block->GetExpressions()) {
            if (expr->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(expr);
            if (store->GetLocation() == location && !IsNonDecreasingValueFromLocation(store->GetValue(), location)) {
                return false;
            }
        }
        for (auto pred : block->GetPredecessors()) {
            if (pred != header) {
                worklist.emplace_back(pred);
            }
        }
    }
    return true;
}

// 查找循环入口边对循环携带位置写入的唯一常量。
std::optional<SInt> FindIncomingStoreConstant(const Block* header, Value* location)
{
    if (!HasNonDecreasingBackedgeStore(header, location)) {
        return std::nullopt;
    }
    std::optional<SInt> incoming;
    for (auto pred : header->GetPredecessors()) {
        if (IsBackedgePredecessor(header, pred)) {
            continue;
        }
        auto exprs = pred->GetExpressions();
        for (auto it = exprs.rbegin(); it != exprs.rend(); ++it) {
            if ((*it)->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(*it);
            if (store->GetLocation() != location) {
                continue;
            }
            auto constant = GetSingleIntFromDefiningConstant(store->GetValue());
            if (!constant.has_value()) {
                return std::nullopt;
            }
            if (incoming.has_value() && incoming.value() != constant.value()) {
                return std::nullopt;
            }
            incoming = constant.value();
            break;
        }
    }
    return incoming;
}

// 将循环入口常量恢复为递增归纳 load 的下界。
bool RestoreLoopIncomingLowerBound(RangeDomain& state, Value* value)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::LOAD) {
        return true;
    }
    auto load = StaticCast<const Load*>(expr);
    auto header = load->GetParentBlock();
    auto incoming = FindIncomingStoreConstant(header, load->GetLocation());
    if (!incoming.has_value()) {
        return true;
    }
    return NarrowSIntByRelationToConstant(state, value, RelationalOperation::GE, incoming.value());
}

// 判断关系是否为左操作数提供上界。
bool HasUpperBoundRelation(RelationalOperation rel)
{
    return rel == RelationalOperation::LT || rel == RelationalOperation::LE || rel == RelationalOperation::EQ;
}

// 判断关系是否为左操作数提供下界。
bool HasLowerBoundRelation(RelationalOperation rel)
{
    return rel == RelationalOperation::GT || rel == RelationalOperation::GE || rel == RelationalOperation::EQ;
}

// 从定义常量中读取有符号整数值。
std::optional<int64_t> GetSignedConstantFromDefiningConstant(Value* value)
{
    if (value == nullptr || !value->GetType()->IsInteger()) {
        return std::nullopt;
    }
    auto constant = GetSingleIntFromDefiningConstant(value);
    if (!constant.has_value()) {
        return std::nullopt;
    }
    if (value->GetType()->IsUnsignedInteger()) {
        auto unsignedValue = constant->UVal();
        if (unsignedValue > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<int64_t>(unsignedValue);
    }
    return constant->SVal();
}

// 如果值是 load 表达式，返回其 ref 位置。
Value* GetLoadLocation(Value* value)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr) {
        return nullptr;
    }
    if (expr->GetExprKind() == ExprKind::TYPECAST) {
        auto source = StaticCast<const TypeCast*>(expr)->GetSourceValue();
        if (source != nullptr && source->GetType()->IsInteger() && value->GetType()->IsInteger() &&
            source->GetType()->IsUnsignedInteger() == value->GetType()->IsUnsignedInteger() &&
            ToWidth(*source->GetType()) <= ToWidth(*value->GetType())) {
            return GetLoadLocation(source);
        }
        return nullptr;
    }
    if (expr->GetExprKind() != ExprKind::LOAD) {
        return nullptr;
    }
    return StaticCast<const Load*>(expr)->GetLocation();
}

// 对有符号 step 取负，并排除 int64 最小值溢出。
std::optional<int64_t> NegateSignedStep(int64_t step)
{
    if (step == std::numeric_limits<int64_t>::min()) {
        return std::nullopt;
    }
    return -step;
}

// 提取循环携带位置更新时使用的常量步长。
std::optional<int64_t> GetUpdateStepFromLocation(Value* value, Value* location)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr) {
        return std::nullopt;
    }
    if (expr->GetExprKind() == ExprKind::TYPECAST) {
        auto source = StaticCast<const TypeCast*>(expr)->GetSourceValue();
        if (source != nullptr && source->GetType()->IsInteger() && value->GetType()->IsInteger() &&
            ToWidth(*source->GetType()) == ToWidth(*value->GetType())) {
            return GetUpdateStepFromLocation(source, location);
        }
        return std::nullopt;
    }
    if (IsLoadFromLocation(value, location)) {
        return 0;
    }
    if (expr->GetExprKind() != ExprKind::ADD && expr->GetExprKind() != ExprKind::SUB) {
        return std::nullopt;
    }

    auto binary = StaticCast<const BinaryExpression*>(expr);
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    if (expr->GetExprKind() == ExprKind::ADD) {
        if (IsLoadFromLocation(lhs, location)) {
            return GetSignedConstantFromDefiningConstant(rhs);
        }
        if (IsLoadFromLocation(rhs, location)) {
            return GetSignedConstantFromDefiningConstant(lhs);
        }
        return std::nullopt;
    }
    if (!IsLoadFromLocation(lhs, location)) {
        return std::nullopt;
    }
    auto rhsStep = GetSignedConstantFromDefiningConstant(rhs);
    if (!rhsStep.has_value()) {
        return std::nullopt;
    }
    return NegateSignedStep(rhsStep.value());
}

// 查找循环回边上唯一的常量更新步长。
std::optional<int64_t> FindSingleBackedgeStep(const Block* header, Value* location)
{
    std::optional<int64_t> step;
    bool hasBackedge = false;
    for (auto pred : header->GetPredecessors()) {
        if (!IsBackedgePredecessor(header, pred)) {
            continue;
        }
        hasBackedge = true;
        std::optional<int64_t> predStep;
        for (auto expr : pred->GetExpressions()) {
            if (expr->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(expr);
            if (store->GetLocation() != location) {
                continue;
            }
            if (predStep.has_value()) {
                return std::nullopt;
            }
            predStep = GetUpdateStepFromLocation(store->GetValue(), location);
            if (!predStep.has_value()) {
                return std::nullopt;
            }
        }
        if (!predStep.has_value()) {
            return std::nullopt;
        }
        if (step.has_value() && step.value() != predStep.value()) {
            return std::nullopt;
        }
        step = predStep.value();
    }
    if (!hasBackedge) {
        return std::nullopt;
    }
    return step;
}

// Collect one constant update for every backedge. Unlike
// FindSingleBackedgeStep, different backedges may use different steps. The
// result is available only when every backedge writes the same induction
// location exactly once using load(location) +/- constant.
std::optional<std::vector<int64_t>> FindAllBackedgeSteps(const Block* header, Value* location)
{
    std::vector<int64_t> steps;
    for (auto pred : header->GetPredecessors()) {
        if (!IsBackedgePredecessor(header, pred)) {
            continue;
        }
        std::optional<int64_t> predStep;
        for (auto expr : pred->GetExpressions()) {
            if (expr->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(expr);
            if (store->GetLocation() != location) {
                continue;
            }
            if (predStep.has_value()) {
                return std::nullopt;
            }
            predStep = GetUpdateStepFromLocation(store->GetValue(), location);
            if (!predStep.has_value() || predStep.value() == 0) {
                return std::nullopt;
            }
        }
        if (!predStep.has_value()) {
            return std::nullopt;
        }
        steps.emplace_back(predStep.value());
    }
    return steps.empty() ? std::nullopt
                         : std::optional<std::vector<int64_t>>{std::move(steps)};
}

// 查找循环入口边写入的唯一有符号常量。
std::optional<int64_t> FindIncomingSignedStoreConstant(const Block* header, Value* location)
{
    std::optional<int64_t> incoming;
    bool hasIncoming = false;
    for (auto pred : header->GetPredecessors()) {
        if (IsBackedgePredecessor(header, pred)) {
            continue;
        }
        hasIncoming = true;
        std::optional<int64_t> predIncoming;
        auto exprs = pred->GetExpressions();
        for (auto it = exprs.rbegin(); it != exprs.rend(); ++it) {
            if ((*it)->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(*it);
            if (store->GetLocation() != location) {
                continue;
            }
            predIncoming = GetSignedConstantFromDefiningConstant(store->GetValue());
            break;
        }
        if (!predIncoming.has_value()) {
            return std::nullopt;
        }
        if (incoming.has_value() && incoming.value() != predIncoming.value()) {
            return std::nullopt;
        }
        incoming = predIncoming.value();
    }
    if (!hasIncoming) {
        return std::nullopt;
    }
    return incoming;
}

// 将循环入口常量恢复为递减归纳 load 的上界。
bool RestoreLoopIncomingUpperBound(RangeDomain& state, Value* value)
{
    auto location = GetLoadLocation(value);
    if (location == nullptr) {
        return true;
    }
    auto header = StaticCast<const Load*>(GetDefiningExpr(value))->GetParentBlock();
    auto init = FindIncomingSignedStoreConstant(header, location);
    if (!init.has_value()) {
        init = FindIncomingSignedStoreConstantThroughPredecessors(header, location);
    }
    auto step = FindSingleBackedgeStep(header, location);
    if (!init.has_value() || !step.has_value() || step.value() >= 0) {
        return true;
    }
    auto width = ToWidth(*value->GetType());
    return NarrowSIntByRelationToConstant(
        state, value, RelationalOperation::LE, SInt{width, static_cast<uint64_t>(init.value())});
}

struct SimpleInductionCondition {
    Value* loadValue;
    Value* location;
    RelationalOperation relation;
    int64_t bound;
};

struct ReplayInductionCondition {
    Value* loadValue;
    Value* location;
    RelationalOperation relation;
    int64_t bound;
    bool isUnsigned;
};

// 识别 load(location) 与有符号常量比较形成的循环 guard。
std::optional<SimpleInductionCondition> GetSimpleInductionCondition(Value* condition)
{
    auto expr = GetDefiningExpr(condition);
    if (expr == nullptr || !IsRelationalExprKind(expr->GetExprKind())) {
        return std::nullopt;
    }
    auto binary = StaticCast<const BinaryExpression*>(expr);
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    if (!lhs->GetType()->IsInteger() || !rhs->GetType()->IsInteger() || lhs->GetType()->IsUnsignedInteger() ||
        rhs->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }

    auto rel = ToRelationalOperation(expr->GetExprKind());
    if (auto lhsLocation = GetLoadLocation(lhs); lhsLocation != nullptr) {
        auto bound = GetSignedConstantFromDefiningConstant(rhs);
        if (!bound.has_value()) {
            return std::nullopt;
        }
        return SimpleInductionCondition{lhs, lhsLocation, rel, bound.value()};
    }
    if (auto rhsLocation = GetLoadLocation(rhs); rhsLocation != nullptr) {
        auto bound = GetSignedConstantFromDefiningConstant(lhs);
        if (!bound.has_value()) {
            return std::nullopt;
        }
        return SimpleInductionCondition{rhs, rhsLocation, SwapRelation(rel), bound.value()};
    }
    return std::nullopt;
}

// Extract the same constant-guard shape for replay selection. Unsigned values
// are accepted only while their constants fit the signed host representation;
// this keeps the trip-count proof overflow-free without changing transfers.
std::optional<ReplayInductionCondition> GetReplayInductionCondition(Value* condition)
{
    auto expr = GetDefiningExpr(condition);
    if (expr == nullptr || !IsRelationalExprKind(expr->GetExprKind())) {
        return std::nullopt;
    }
    auto binary = StaticCast<const BinaryExpression*>(expr);
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    if (!lhs->GetType()->IsInteger() || !rhs->GetType()->IsInteger() ||
        lhs->GetType()->IsUnsignedInteger() != rhs->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }
    auto relation = ToRelationalOperation(expr->GetExprKind());
    if (auto location = GetLoadLocation(lhs); location != nullptr) {
        auto bound = GetSignedConstantFromDefiningConstant(rhs);
        if (bound.has_value()) {
            return ReplayInductionCondition{
                lhs, location, relation, bound.value(), lhs->GetType()->IsUnsignedInteger()};
        }
    }
    if (auto location = GetLoadLocation(rhs); location != nullptr) {
        auto bound = GetSignedConstantFromDefiningConstant(lhs);
        if (bound.has_value()) {
            return ReplayInductionCondition{
                rhs, location, SwapRelation(relation), bound.value(), rhs->GetType()->IsUnsignedInteger()};
        }
    }
    return std::nullopt;
}

// 返回指定 CHIR 整数宽度下的有符号最小值和最大值。
std::pair<int64_t, int64_t> SignedLimits(IntWidth width)
{
    if (width == IntWidth::I64) {
        return {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()};
    }
    auto bits = static_cast<unsigned>(width);
    int64_t max = static_cast<int64_t>((1ULL << (bits - 1U)) - 1ULL);
    int64_t min = -static_cast<int64_t>(1ULL << (bits - 1U));
    return {min, max};
}

std::optional<int64_t> InferLoopInitFromCurrentState(
    const RangeDomain& state, Value* location, Type* type, int64_t step)
{
    if (location == nullptr || type == nullptr || !type->IsInteger() || type->IsUnsignedInteger() || step == 0) {
        return std::nullopt;
    }
    auto object = state.CheckAbstractObjectRefBy(location);
    if (object == nullptr) {
        return std::nullopt;
    }
    const auto& domain = GetSIntDomainFromState(state, object, type);
    if (domain.IsTop() || domain.IsBottom() || !domain.SymbolicBounds().Empty()) {
        return std::nullopt;
    }
    const auto& numeric = domain.NumericBound();
    if (numeric.IsFullSet() || numeric.IsEmptySet() || numeric.IsWrappedSet() || numeric.IsSignWrappedSet()) {
        return std::nullopt;
    }
    // A one-sided interval at a loop header is commonly a widening result.
    // Its finite or type-limit endpoint is not evidence of the concrete entry
    // value and must never seed exact induction enumeration.
    return domain.IsSingleValue()
        ? std::optional<int64_t>{numeric.GetSingleElement().SVal()}
        : std::nullopt;
}

// 判断扩展精度计算结果是否仍适配目标有符号宽度。
bool FitsSignedWidth(__int128 value, IntWidth width)
{
    auto [min, max] = SignedLimits(width);
    return value >= static_cast<__int128>(min) && value <= static_cast<__int128>(max);
}

bool FitsModeledIntegerWidth(__int128 value, Type* type)
{
    if (type == nullptr || !type->IsInteger()) {
        return false;
    }
    if (!type->IsUnsignedInteger()) {
        return FitsSignedWidth(value, ToWidth(*type));
    }
    auto bits = static_cast<unsigned>(ToWidth(*type));
    auto max = (static_cast<__int128>(1) << bits) - 1;
    return value >= 0 && value <= max;
}

// 计算正数归纳变量推导中使用的向上整除。
__int128 CeilDivPositive(__int128 numerator, __int128 denominator)
{
    CJC_ASSERT(numerator > 0 && denominator > 0);
    return (numerator + denominator - 1) / denominator;
}

// 计算可证明常量步长归纳变量的精确循环退出值。
std::optional<SInt> ComputeExactInductionExit(
    int64_t init, int64_t step, RelationalOperation relation, int64_t bound, IntWidth width)
{
    if (step == 0) {
        return std::nullopt;
    }
    __int128 exact = init;
    if (step > 0) {
        __int128 threshold = bound;
        if (relation == RelationalOperation::LE) {
            if (bound == std::numeric_limits<int64_t>::max()) {
                return std::nullopt;
            }
            threshold = static_cast<__int128>(bound) + 1;
        } else if (relation != RelationalOperation::LT) {
            return std::nullopt;
        }
        if (static_cast<__int128>(init) < threshold) {
            exact = static_cast<__int128>(init) +
                CeilDivPositive(threshold - static_cast<__int128>(init), step) * step;
        }
    } else {
        if (step == std::numeric_limits<int64_t>::min()) {
            return std::nullopt;
        }
        auto absStep = -static_cast<__int128>(step);
        __int128 threshold = bound;
        if (relation == RelationalOperation::GE) {
            if (bound == std::numeric_limits<int64_t>::min()) {
                return std::nullopt;
            }
            threshold = static_cast<__int128>(bound) - 1;
        } else if (relation != RelationalOperation::GT) {
            return std::nullopt;
        }
        if (static_cast<__int128>(init) > threshold) {
            exact = static_cast<__int128>(init) -
                CeilDivPositive(static_cast<__int128>(init) - threshold, absStep) * absStep;
        }
    }
    if (!FitsSignedWidth(exact, width)) {
        return std::nullopt;
    }
    return SInt{width, static_cast<uint64_t>(static_cast<int64_t>(exact))};
}

// 判断分支后继是否会离开该分支块所在循环。
bool SatisfiesSignedRelation(__int128 lhs, RelationalOperation relation, __int128 rhs)
{
    switch (relation) {
        case RelationalOperation::LT:
            return lhs < rhs;
        case RelationalOperation::LE:
            return lhs <= rhs;
        case RelationalOperation::GT:
            return lhs > rhs;
        case RelationalOperation::GE:
            return lhs >= rhs;
        case RelationalOperation::EQ:
            return lhs == rhs;
        case RelationalOperation::NE:
            return lhs != rhs;
    }
    return false;
}

std::optional<std::vector<int64_t>> TryEnumerateInductionValues(
    int64_t init, int64_t step, RelationalOperation relation, int64_t bound, IntWidth width)
{
    if (step == 0) {
        return std::nullopt;
    }
    auto exit = ComputeExactInductionExit(init, step, relation, bound, width);
    if (!exit.has_value()) {
        return std::nullopt;
    }
    std::vector<int64_t> values;
    __int128 current = init;
    const __int128 rhs = bound;
    while (SatisfiesSignedRelation(current, relation, rhs)) {
        if (!FitsSignedWidth(current, width) || ReachedExactIntSetLimit(values.size())) {
            return std::nullopt;
        }
        values.emplace_back(static_cast<int64_t>(current));
        current += step;
    }
    if (current != static_cast<__int128>(exit->SVal())) {
        return std::nullopt;
    }
    return values.empty() ? std::nullopt : std::optional<std::vector<int64_t>>{std::move(values)};
}

std::optional<std::vector<int64_t>> TryEnumerateInclusiveUntilBound(
    int64_t init, int64_t step, int64_t bound, IntWidth width)
{
    if (step == 0) {
        return std::nullopt;
    }
    auto distance = static_cast<__int128>(bound) - static_cast<__int128>(init);
    if ((distance > 0 && step < 0) || (distance < 0 && step > 0)) {
        return std::nullopt;
    }
    auto absStep = step > 0 ? static_cast<__int128>(step) : -static_cast<__int128>(step);
    auto absDistance = distance >= 0 ? distance : -distance;
    if (absDistance % absStep != 0) {
        return std::nullopt;
    }
    auto count = absDistance / absStep + 1;
    if (count <= 0 || ExceedsExactIntSetLimit(count)) {
        return std::nullopt;
    }
    std::vector<int64_t> values;
    values.reserve(static_cast<size_t>(count));
    __int128 current = init;
    for (__int128 index = 0; index < count; ++index) {
        if (!FitsSignedWidth(current, width)) {
            return std::nullopt;
        }
        values.emplace_back(static_cast<int64_t>(current));
        current += step;
    }
    return values;
}

std::optional<std::vector<int64_t>> AddLoopConditionExitValue(
    std::vector<int64_t> values, Value* value, int64_t init, int64_t step,
    RelationalOperation relation, int64_t bound, IntWidth width)
{
    if (!IsLoopConditionLoadForWidening(value)) {
        return values;
    }
    auto exit = ComputeExactInductionExit(init, step, relation, bound, width);
    if (!exit.has_value()) {
        return std::nullopt;
    }
    auto exitValue = exit->SVal();
    if (std::find(values.begin(), values.end(), exitValue) != values.end()) {
        return values;
    }
    if (ReachedExactIntSetLimit(values.size())) {
        return std::nullopt;
    }
    values.emplace_back(exitValue);
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

bool IsLoopExitSuccessor(const Branch* branch, const Block* successor)
{
    if (branch != nullptr && successor != nullptr) {
        auto header = branch->GetParentBlock();
        if (auto forest = GetLoopForest(header);
            forest != nullptr && forest->FindByHeader(header) != nullptr) {
            return forest->IsExitEdge(header, successor);
        }
    }
    if (!IsLoopBranch(branch)) {
        if (branch == nullptr || successor == nullptr) {
            return false;
        }
        auto header = branch->GetParentBlock();
        std::unordered_set<const Block*> trueVisited;
        std::unordered_set<const Block*> falseVisited;
        auto trueReachesHeader = CanReachBlock(branch->GetTrueBlock(), header, trueVisited);
        auto falseReachesHeader = CanReachBlock(branch->GetFalseBlock(), header, falseVisited);
        if (trueReachesHeader == falseReachesHeader) {
            return false;
        }
        return successor == (trueReachesHeader ? branch->GetFalseBlock() : branch->GetTrueBlock());
    }
    std::unordered_set<const Block*> visited;
    return !CanReachBlock(successor, branch->GetParentBlock(), visited);
}

// 在识别出简单归纳模式时将循环退出状态收窄为精确值。
bool HasOnlyGuardControlledLoopExit(const Branch* branch, const Block* exitSuccessor)
{
    if (branch == nullptr || exitSuccessor == nullptr || !IsLoopExitSuccessor(branch, exitSuccessor)) {
        return false;
    }
    auto header = branch->GetParentBlock();
    auto loopSuccessor =
        branch->GetTrueBlock() == exitSuccessor ? branch->GetFalseBlock() : branch->GetTrueBlock();
    if (header == nullptr || loopSuccessor == nullptr) {
        return false;
    }

    constexpr size_t MAX_PROOF_BLOCKS = 4096;
    std::vector<const Block*> worklist{loopSuccessor};
    std::unordered_set<const Block*> visited;
    while (!worklist.empty()) {
        auto block = worklist.back();
        worklist.pop_back();
        if (block == nullptr || block == header || !visited.emplace(block).second) {
            continue;
        }
        if (visited.size() > MAX_PROOF_BLOCKS) {
            return false;
        }
        for (auto successor : block->GetSuccessors()) {
            if (successor == header) {
                continue;
            }
            std::unordered_set<const Block*> reachesHeader;
            if (!CanReachBlock(successor, header, reachesHeader)) {
                return false;
            }
            worklist.emplace_back(successor);
        }
    }
    return true;
}

std::optional<int64_t> FindSimpleInductionInitialValue(const RangeDomain& state,
    const Branch* branch, const Block* exitSuccessor, Value* location, Type* storageType, int64_t step)
{
    auto header = branch == nullptr ? nullptr : branch->GetParentBlock();
    if (header == nullptr || exitSuccessor == nullptr) {
        return std::nullopt;
    }
    auto init = FindIncomingSignedStoreConstant(header, location);
    if (!init.has_value()) {
        init = FindIncomingSignedStoreConstantThroughPredecessors(header, location);
    }
    if (!init.has_value()) {
        auto loopBlocks = CollectLoopBackPathBlockSet(branch, exitSuccessor, header);
        init = FindIncomingSignedStoreValueBeforeLoop(state, header, location, loopBlocks);
    }
    if (!init.has_value()) {
        init = InferLoopInitFromCurrentState(state, location, storageType, step);
    }
    return init;
}

RelationalOperation GetLoopContinuationRelation(
    const Branch* branch, const Block* exitSuccessor, RelationalOperation conditionRelation)
{
    return branch->GetTrueBlock() == exitSuccessor
        ? NegateRelation(conditionRelation)
        : conditionRelation;
}

std::optional<int64_t> FindProvenInductionInitialValue(const RangeDomain& state,
    const Branch* branch, const Block* exitSuccessor, Value* location)
{
    auto header = branch == nullptr ? nullptr : branch->GetParentBlock();
    if (header == nullptr || exitSuccessor == nullptr) {
        return std::nullopt;
    }
    auto init = FindIncomingSignedStoreConstant(header, location);
    if (!init.has_value()) {
        init = FindIncomingSignedStoreConstantThroughPredecessors(header, location);
    }
    if (!init.has_value()) {
        auto loopBlocks = CollectLoopBackPathBlockSet(branch, exitSuccessor, header);
        init = FindIncomingSignedStoreValueBeforeLoop(state, header, location, loopBlocks);
    }
    return init;
}

std::optional<__int128> ComputeProvenInductionTripCount(int64_t init, int64_t step,
    RelationalOperation relation, int64_t bound, IntWidth width, bool isUnsigned)
{
    auto exactExit = ComputeExactInductionExit(init, step, relation, bound, width);
    if (!exactExit.has_value()) {
        return std::nullopt;
    }
    auto exactExitValue = static_cast<__int128>(exactExit->SVal());
    if (isUnsigned && exactExitValue < 0) {
        return std::nullopt;
    }
    auto distance = exactExitValue - static_cast<__int128>(init);
    if (distance == 0 || distance % static_cast<__int128>(step) != 0) {
        return std::nullopt;
    }
    auto tripCount = distance / static_cast<__int128>(step);
    return tripCount > 0 ? std::optional<__int128>{tripCount} : std::nullopt;
}

// Large scalar induction loops are represented more cheaply by the regular
// worklist plus guard/exit narrowing than by replaying every concrete step.
// Keep replay for small loops and for every loop with additional memory or
// call effects, where concrete observations still add precision.
bool ShouldPreferAnalyticInductionOverBoundedReplay(
    const RangeDomain& state, const Branch* branch)
{
    const Block* exitSuccessor = nullptr;
    for (auto candidate : {branch->GetTrueBlock(), branch->GetFalseBlock()}) {
        if (!IsLoopExitSuccessor(branch, candidate)) {
            continue;
        }
        if (exitSuccessor != nullptr && exitSuccessor != candidate) {
            return false;
        }
        exitSuccessor = candidate;
    }
    if (exitSuccessor == nullptr) {
        return false;
    }
    auto condition = GetReplayInductionCondition(branch->GetCondition());
    if (!condition.has_value()) {
        return false;
    }
    auto storageType = GetIntegerRefRootType(condition->location);
    if (storageType == nullptr) {
        storageType = condition->loadValue->GetType();
    }
    if (storageType == nullptr || !storageType->IsInteger() ||
        storageType->IsUnsignedInteger() != condition->isUnsigned) {
        return false;
    }
    auto relation =
        GetLoopContinuationRelation(branch, exitSuccessor, condition->relation);
    const auto singleStep = FindSingleBackedgeStep(branch->GetParentBlock(), condition->location);
    const bool hasDifferentBackedgeSteps = !singleStep.has_value();
    std::optional<int64_t> analyticStep = singleStep;
    std::optional<int64_t> init;
    __int128 replayThreshold = rangeAnalysisConfig.maxExactIntSetSize;
    if (hasDifferentBackedgeSteps) {
        auto backedgeSteps = FindAllBackedgeSteps(
            branch->GetParentBlock(), condition->location);
        if (!backedgeSteps.has_value() || backedgeSteps->size() < 2) {
            return false;
        }
        if (relation == RelationalOperation::LT || relation == RelationalOperation::LE) {
            if (std::any_of(backedgeSteps->begin(), backedgeSteps->end(),
                    [](int64_t candidate) { return candidate <= 0; })) {
                return false;
            }
            analyticStep = *std::max_element(backedgeSteps->begin(), backedgeSteps->end());
        } else if (relation == RelationalOperation::GT || relation == RelationalOperation::GE) {
            if (std::any_of(backedgeSteps->begin(), backedgeSteps->end(),
                    [](int64_t candidate) { return candidate >= 0; })) {
                return false;
            }
            analyticStep = *std::min_element(backedgeSteps->begin(), backedgeSteps->end());
        } else {
            return false;
        }
        init = FindProvenInductionInitialValue(
            state, branch, exitSuccessor, condition->location);
        // Replay counts basic-block transitions, so proving more loop
        // iterations than the entire transition budget is deliberately
        // conservative even for a multi-block body.
        replayThreshold = MAX_BOUNDED_LOOP_STEPS;
    } else {
        if (singleStep.value() == 0) {
            return false;
        }
        init = FindSimpleInductionInitialValue(
            state, branch, exitSuccessor, condition->location, storageType, singleStep.value());
    }
    if (!analyticStep.has_value() || !init.has_value() ||
        (condition->isUnsigned && init.value() < 0)) {
        return false;
    }
    auto tripCount = ComputeProvenInductionTripCount(init.value(), analyticStep.value(),
        relation, condition->bound, ToWidth(*storageType), condition->isUnsigned);
    if (!tripCount.has_value() || tripCount.value() <= replayThreshold) {
        return false;
    }

    auto header = branch->GetParentBlock();
    auto loopBlocks = CollectLoopBackPathBlockSet(branch, exitSuccessor, header);
    if (header == nullptr || loopBlocks.empty()) {
        return false;
    }
    for (auto block : loopBlocks) {
        auto terminator = block->GetTerminator();
        if (terminator == nullptr || (terminator->GetExprKind() != ExprKind::GOTO &&
            terminator->GetExprKind() != ExprKind::BRANCH)) {
            return false;
        }
        if (hasDifferentBackedgeSteps) {
            for (auto successor : block->GetSuccessors()) {
                if (loopBlocks.find(successor) != loopBlocks.end()) {
                    continue;
                }
                if (block != header || successor != exitSuccessor) {
                    return false;
                }
            }
        }
        for (auto expression : block->GetNonTerminatorExpressions()) {
            if (expression->GetExprMajorKind() == ExprMajorKind::UNARY_EXPR ||
                expression->GetExprMajorKind() == ExprMajorKind::BINARY_EXPR) {
                continue;
            }
            switch (expression->GetExprKind()) {
                case ExprKind::CONSTANT:
                case ExprKind::DEBUGEXPR:
                case ExprKind::TYPECAST:
                    break;
                case ExprKind::LOAD:
                    if (StaticCast<const Load*>(expression)->GetLocation() != condition->location) {
                        return false;
                    }
                    break;
                case ExprKind::STORE:
                    if (StaticCast<const Store*>(expression)->GetLocation() != condition->location) {
                        return false;
                    }
                    if (hasDifferentBackedgeSteps && !IsBackedgePredecessor(header, block)) {
                        return false;
                    }
                    break;
                default:
                    return false;
            }
        }
    }
    return true;
}

bool TryNarrowSimpleInductionBody(RangeDomain& state, const Branch* branch, const Block* successor)
{
    const Block* exitSuccessor = nullptr;
    for (auto candidate : {branch->GetTrueBlock(), branch->GetFalseBlock()}) {
        if (!IsLoopExitSuccessor(branch, candidate)) {
            continue;
        }
        if (exitSuccessor != nullptr && exitSuccessor != candidate) {
            return true;
        }
        exitSuccessor = candidate;
    }
    if (exitSuccessor == nullptr || successor == exitSuccessor) {
        return true;
    }
    auto bodySuccessor =
        branch->GetTrueBlock() == exitSuccessor ? branch->GetFalseBlock() : branch->GetTrueBlock();
    if (successor != bodySuccessor) {
        return true;
    }

    auto condition = GetSimpleInductionCondition(branch->GetCondition());
    if (!condition.has_value()) {
        return true;
    }
    auto loadType = condition->loadValue->GetType();
    auto storageType = GetIntegerRefRootType(condition->location);
    if (storageType == nullptr || storageType->IsUnsignedInteger()) {
        storageType = loadType;
    }
    if (loadType->IsUnsignedInteger() || storageType->IsUnsignedInteger()) {
        return true;
    }
    auto step = FindSingleBackedgeStep(branch->GetParentBlock(), condition->location);
    if (!step.has_value()) {
        return true;
    }
    auto init = FindSimpleInductionInitialValue(
        state, branch, exitSuccessor, condition->location, storageType, step.value());
    if (!init.has_value()) {
        return true;
    }
    auto relation =
        GetLoopContinuationRelation(branch, exitSuccessor, condition->relation);
    auto values = TryEnumerateInductionValues(
        init.value(), step.value(), relation, condition->bound, ToWidth(*storageType));
    if (!values.has_value()) {
        return true;
    }

    if (auto loadRange = BuildSignedExactRange(loadType, values.value()); loadRange.has_value()) {
        state.Update(condition->loadValue, std::make_unique<SIntRange>(std::move(loadRange.value())));
    }
    if (auto object = state.CheckAbstractObjectRefBy(condition->location); object != nullptr) {
        if (auto objectRange = BuildSignedExactRange(storageType, values.value()); objectRange.has_value()) {
            state.Update(object, std::make_unique<SIntRange>(std::move(objectRange.value())));
        }
    }
    return true;
}

bool TryNarrowSimpleInductionExit(RangeDomain& state, const Branch* branch, const Block* successor)
{
    if (!IsLoopExitSuccessor(branch, successor) ||
        !HasOnlyGuardControlledLoopExit(branch, successor)) {
        return true;
    }
    auto condition = GetSimpleInductionCondition(branch->GetCondition());
    if (!condition.has_value()) {
        return true;
    }
    auto loadType = condition->loadValue->GetType();
    if (loadType->IsUnsignedInteger()) {
        return true;
    }
    auto storageType = GetIntegerRefRootType(condition->location);
    if (storageType == nullptr || storageType->IsUnsignedInteger()) {
        storageType = loadType;
    }
    auto step = FindSingleBackedgeStep(branch->GetParentBlock(), condition->location);
    if (!step.has_value()) {
        return true;
    }
    auto init = FindSimpleInductionInitialValue(
        state, branch, successor, condition->location, storageType, step.value());
    if (!init.has_value()) {
        return true;
    }
    auto relation =
        GetLoopContinuationRelation(branch, successor, condition->relation);
    auto exact = ComputeExactInductionExit(
        init.value(), step.value(), relation, condition->bound, ToWidth(*storageType));
    if (!exact.has_value()) {
        return true;
    }
    auto observedExact = ToWidth(*loadType) == exact->Width()
        ? exact.value()
        : exact->SExt(ToWidth(*loadType));
    NarrowSIntByRelationToConstant(
        state, condition->loadValue, RelationalOperation::EQ, observedExact);
    if (auto object = state.CheckAbstractObjectRefBy(condition->location); object != nullptr) {
        state.Update(object,
            std::make_unique<SIntRange>(SIntDomain::FromNumeric(
                RelationalOperation::EQ, exact.value(), storageType->IsUnsignedInteger())));
    }
    return true;
}

bool CanComputeSimpleInductionExitFromState(const RangeDomain& state, const Branch* branch, const Block* successor)
{
    if (!IsLoopExitSuccessor(branch, successor) ||
        !HasOnlyGuardControlledLoopExit(branch, successor)) {
        return false;
    }
    auto condition = GetSimpleInductionCondition(branch->GetCondition());
    if (!condition.has_value()) {
        return false;
    }
    auto loadType = condition->loadValue->GetType();
    if (loadType->IsUnsignedInteger()) {
        return false;
    }
    auto storageType = GetIntegerRefRootType(condition->location);
    if (storageType == nullptr || storageType->IsUnsignedInteger()) {
        storageType = loadType;
    }
    auto step = FindSingleBackedgeStep(branch->GetParentBlock(), condition->location);
    if (!step.has_value()) {
        return false;
    }
    auto init = FindSimpleInductionInitialValue(
        state, branch, successor, condition->location, storageType, step.value());
    if (!init.has_value()) {
        return false;
    }
    auto relation =
        GetLoopContinuationRelation(branch, successor, condition->relation);
    return ComputeExactInductionExit(
        init.value(), step.value(), relation, condition->bound, ToWidth(*storageType)).has_value();
}

// 获取同宽整数 typecast 的源值，用于回推 case 约束。
struct VariableBoundInductionExit {
    Value* loadValue;
    Value* location;
    Value* boundValue;
    const Block* header;
    int64_t init;
    int64_t step;
    RelationalOperation relation{RelationalOperation::LE};
};

struct CountedAccumulatorUpdate {
    Value* location;
    Type* type;
    int64_t init;
    int64_t step;
};

struct AffineAccumulatorUpdate {
    Value* location;
    Type* type;
    int64_t init;
    int64_t inductionCoefficient;
    int64_t constant;
};

struct AffineAccumulatorForm {
    __int128 accumulatorCoefficient{0};
    __int128 inductionCoefficient{0};
    __int128 constant{0};
};

std::optional<AffineAccumulatorForm> GetAffineAccumulatorForm(
    Value* value, Value* accumulatorLocation, Value* inductionLocation, size_t depth = 0)
{
    constexpr size_t MAX_AFFINE_EXPRESSION_DEPTH = 12;
    if (value == nullptr || accumulatorLocation == nullptr || inductionLocation == nullptr ||
        depth > MAX_AFFINE_EXPRESSION_DEPTH) {
        return std::nullopt;
    }
    if (auto constant = GetSignedConstantFromDefiningConstant(value); constant.has_value()) {
        return AffineAccumulatorForm{0, 0, constant.value()};
    }
    if (auto location = GetLoadLocation(value); location != nullptr) {
        if (location == accumulatorLocation) {
            return AffineAccumulatorForm{1, 0, 0};
        }
        if (location == inductionLocation) {
            return AffineAccumulatorForm{0, 1, 0};
        }
        return std::nullopt;
    }
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr) {
        return std::nullopt;
    }
    if (expr->GetExprKind() == ExprKind::TYPECAST) {
        auto source = StaticCast<const TypeCast*>(expr)->GetSourceValue();
        if (source == nullptr || !source->GetType()->IsInteger() || source->GetType()->IsUnsignedInteger() ||
            !value->GetType()->IsInteger() || value->GetType()->IsUnsignedInteger() ||
            static_cast<unsigned>(ToWidth(*source->GetType())) > static_cast<unsigned>(ToWidth(*value->GetType()))) {
            return std::nullopt;
        }
        return GetAffineAccumulatorForm(source, accumulatorLocation, inductionLocation, depth + 1);
    }
    if (expr->GetExprKind() != ExprKind::ADD && expr->GetExprKind() != ExprKind::SUB &&
        expr->GetExprKind() != ExprKind::MUL) {
        return std::nullopt;
    }
    auto binary = StaticCast<const BinaryExpression*>(expr);
    auto lhs = GetAffineAccumulatorForm(
        binary->GetLHSOperand(), accumulatorLocation, inductionLocation, depth + 1);
    auto rhs = GetAffineAccumulatorForm(
        binary->GetRHSOperand(), accumulatorLocation, inductionLocation, depth + 1);
    if (!lhs.has_value() || !rhs.has_value()) {
        return std::nullopt;
    }
    if (expr->GetExprKind() == ExprKind::ADD) {
        return AffineAccumulatorForm{lhs->accumulatorCoefficient + rhs->accumulatorCoefficient,
            lhs->inductionCoefficient + rhs->inductionCoefficient, lhs->constant + rhs->constant};
    }
    if (expr->GetExprKind() == ExprKind::SUB) {
        return AffineAccumulatorForm{lhs->accumulatorCoefficient - rhs->accumulatorCoefficient,
            lhs->inductionCoefficient - rhs->inductionCoefficient, lhs->constant - rhs->constant};
    }
    const bool lhsConstant = lhs->accumulatorCoefficient == 0 && lhs->inductionCoefficient == 0;
    const bool rhsConstant = rhs->accumulatorCoefficient == 0 && rhs->inductionCoefficient == 0;
    if (lhsConstant == rhsConstant) {
        return std::nullopt;
    }
    auto factor = lhsConstant ? lhs->constant : rhs->constant;
    const auto& form = lhsConstant ? rhs.value() : lhs.value();
    return AffineAccumulatorForm{form.accumulatorCoefficient * factor,
        form.inductionCoefficient * factor, form.constant * factor};
}

struct SignedInterval {
    int64_t min;
    int64_t max;
};

struct AccumulatorDeltaInterval {
    size_t loadCount{0};
    SignedInterval delta{0, 0};
};

std::optional<size_t> ShortestBlockDistance(const Block* start, const Block* target)
{
    if (start == nullptr || target == nullptr) {
        return std::nullopt;
    }
    std::vector<std::pair<const Block*, size_t>> worklist{{start, 0}};
    std::unordered_set<const Block*> visited;
    for (size_t index = 0; index < worklist.size(); ++index) {
        auto [block, distance] = worklist[index];
        if (!visited.emplace(block).second) {
            continue;
        }
        if (block == target) {
            return distance;
        }
        for (auto successorBlock : block->GetSuccessors()) {
            worklist.emplace_back(successorBlock, distance + 1);
        }
    }
    return std::nullopt;
}

std::optional<int64_t> FindSingleStepOnLoopBackPath(
    const Branch* exitBranch, const Block* exitSuccessor, const Block* header, Value* location)
{
    auto loopSuccessor = exitBranch->GetTrueBlock() == exitSuccessor ? exitBranch->GetFalseBlock() :
        exitBranch->GetTrueBlock();
    if (loopSuccessor == nullptr || header == nullptr || location == nullptr) {
        return std::nullopt;
    }

    std::optional<int64_t> step;
    std::vector<const Block*> worklist{loopSuccessor};
    std::unordered_set<const Block*> visited;
    while (!worklist.empty()) {
        auto block = worklist.back();
        worklist.pop_back();
        if (block == nullptr || block == header || !visited.emplace(block).second) {
            continue;
        }
        for (auto expr : block->GetExpressions()) {
            if (expr->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(expr);
            if (store->GetLocation() != location) {
                continue;
            }
            auto currentStep = GetUpdateStepFromLocation(store->GetValue(), location);
            if (!currentStep.has_value()) {
                return std::nullopt;
            }
            if (step.has_value()) {
                return std::nullopt;
            }
            step = currentStep.value();
        }
        for (auto successorBlock : block->GetSuccessors()) {
            std::unordered_set<const Block*> reachHeader;
            if (CanReachBlockAvoidingHeader(successorBlock, header, exitSuccessor, reachHeader)) {
                worklist.emplace_back(successorBlock);
            }
        }
    }
    return step;
}

std::optional<int64_t> FindSingleReachableLoopUpdateStep(const Block* header, Value* location)
{
    if (header == nullptr || location == nullptr) {
        return std::nullopt;
    }
    std::optional<int64_t> step;
    std::vector<const Block*> worklist{header};
    std::unordered_set<const Block*> visited;
    while (!worklist.empty()) {
        auto block = worklist.back();
        worklist.pop_back();
        if (block == nullptr || !visited.emplace(block).second) {
            continue;
        }
        if (block != header) {
            std::unordered_set<const Block*> reachHeader;
            if (!CanReachBlock(block, header, reachHeader)) {
                continue;
            }
            for (auto expr : block->GetExpressions()) {
                if (expr->GetExprKind() != ExprKind::STORE) {
                    continue;
                }
                auto store = StaticCast<const Store*>(expr);
                if (store->GetLocation() != location) {
                    continue;
                }
                auto currentStep = GetUpdateStepFromLocation(store->GetValue(), location);
                if (!currentStep.has_value()) {
                    return std::nullopt;
                }
                if (step.has_value()) {
                    return std::nullopt;
                }
                step = currentStep.value();
            }
        }
        for (auto successorBlock : block->GetSuccessors()) {
            worklist.emplace_back(successorBlock);
        }
    }
    return step;
}

std::optional<int64_t> GetLoopExitConstantBound(Value* lhs, Value* rhs, Value* location, RelationalOperation rel)
{
    if (auto lhsLocation = GetLoadLocation(lhs); lhsLocation == location) {
        auto bound = GetSignedConstantFromDefiningConstant(rhs);
        if (bound.has_value()) {
            return bound.value();
        }
    }
    if (auto rhsLocation = GetLoadLocation(rhs); rhsLocation == location) {
        auto bound = GetSignedConstantFromDefiningConstant(lhs);
        if (bound.has_value()) {
            (void)rel;
            return bound.value();
        }
    }
    return std::nullopt;
}

std::optional<int64_t> FindEqualityExitConstantBound(const Block* header, Value* location)
{
    if (header == nullptr || location == nullptr) {
        return std::nullopt;
    }
    std::optional<int64_t> bound;
    std::vector<const Block*> worklist{header};
    std::unordered_set<const Block*> visited;
    while (!worklist.empty()) {
        auto block = worklist.back();
        worklist.pop_back();
        if (block == nullptr || !visited.emplace(block).second) {
            continue;
        }
        auto terminator = block->GetTerminator();
        if (terminator != nullptr && terminator->GetExprKind() == ExprKind::BRANCH) {
            auto branch = StaticCast<const Branch*>(terminator);
            auto expr = GetDefiningExpr(branch->GetCondition());
            if (expr != nullptr && IsRelationalExprKind(expr->GetExprKind())) {
                auto rel = ToRelationalOperation(expr->GetExprKind());
                auto binary = StaticCast<const BinaryExpression*>(expr);
                std::vector<std::pair<const Block*, bool>> successors{
                    {branch->GetTrueBlock(), true}, {branch->GetFalseBlock(), false}};
                for (auto [successorBlock, branchCondition] : successors) {
                    if (!IsLoopExitSuccessor(branch, successorBlock)) {
                        continue;
                    }
                    auto exitRel = branchCondition ? rel : NegateRelation(rel);
                    if (exitRel != RelationalOperation::EQ) {
                        continue;
                    }
                    auto currentBound = GetLoopExitConstantBound(
                        binary->GetLHSOperand(), binary->GetRHSOperand(), location, exitRel);
                    if (!currentBound.has_value()) {
                        continue;
                    }
                    if (bound.has_value() && bound.value() != currentBound.value()) {
                        return std::nullopt;
                    }
                    bound = currentBound.value();
                }
            }
        }
        for (auto successorBlock : block->GetSuccessors()) {
            worklist.emplace_back(successorBlock);
        }
    }
    return bound;
}

std::optional<std::vector<int64_t>> TryEnumerateSimpleLoopLoadValues(const RangeDomain& state, Value* value)
{
    auto location = GetLoadLocation(value);
    auto loadExpr = GetDefiningExpr(value);
    if (location == nullptr || loadExpr == nullptr || loadExpr->GetParentBlock() == nullptr ||
        value->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }
    auto header = loadExpr->GetParentBlock();
    auto init = FindIncomingSignedStoreConstant(header, location);
    if (!init.has_value()) {
        init = FindIncomingSignedStoreConstantThroughPredecessors(header, location);
    }
    auto step = FindSingleReachableLoopUpdateStep(header, location);
    if (!init.has_value() && step.has_value()) {
        init = InferLoopInitFromCurrentState(state, location, value->GetType(), step.value());
    }
    auto terminator = header->GetTerminator();
    if (init.has_value() && step.has_value() && terminator != nullptr && terminator->GetExprKind() == ExprKind::BRANCH) {
        auto condition = GetSimpleInductionCondition(StaticCast<const Branch*>(terminator)->GetCondition());
        if (condition.has_value() && condition->location == location) {
            if (condition->relation == RelationalOperation::NE) {
                auto values = TryEnumerateInclusiveUntilBound(
                    init.value(), step.value(), condition->bound, ToWidth(*value->GetType()));
                if (values.has_value()) {
                    return values;
                }
            }
            auto values = TryEnumerateInductionValues(
                init.value(), step.value(), condition->relation, condition->bound, ToWidth(*value->GetType()));
            if (values.has_value()) {
                return AddLoopConditionExitValue(std::move(values.value()), value, init.value(), step.value(),
                    condition->relation, condition->bound, ToWidth(*value->GetType()));
            }
        }
    }
    auto bound = FindEqualityExitConstantBound(header, location);
    if (!init.has_value() || !step.has_value() || !bound.has_value() || step.value() != 1 ||
        bound.value() < init.value()) {
        return std::nullopt;
    }
    auto count = static_cast<__int128>(bound.value()) - static_cast<__int128>(init.value()) + 1;
    if (count <= 0 || ExceedsExactIntSetLimit(count)) {
        return std::nullopt;
    }
    std::vector<int64_t> values;
    values.reserve(static_cast<size_t>(count));
    for (int64_t current = init.value();; ++current) {
        values.emplace_back(current);
        if (current == bound.value()) {
            break;
        }
    }
    return values;
}

std::optional<SIntRange> TryComputeSimpleLoopLoadRange(const RangeDomain& state, Value* value)
{
    if (value == nullptr || !value->GetType()->IsInteger() || value->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }
    auto values = TryEnumerateSimpleLoopLoadValues(state, value);
    if (!values.has_value()) {
        return std::nullopt;
    }
    auto width = ToWidth(*value->GetType());
    std::vector<SInt> exact;
    exact.reserve(values->size());
    for (auto item : *values) {
        exact.emplace_back(width, static_cast<uint64_t>(item));
    }
    auto exactValues = NormalizeExactIntSet(std::move(exact));
    if (!exactValues.has_value()) {
        return std::nullopt;
    }
    auto domain = DomainFromExactIntValues(*exactValues, value->GetType()->IsUnsignedInteger());
    return SIntRange{std::move(domain), std::move(exactValues)};
}

std::unordered_set<const Block*> CollectLoopBackPathBlockSet(
    const Branch* branch, const Block* exitSuccessor, const Block* header)
{
    if (auto forest = GetLoopForest(header); forest != nullptr) {
        if (auto loop = forest->FindByHeader(header);
            loop != nullptr && loop->IsExitEdge(branch->GetParentBlock(), exitSuccessor)) {
            return loop->blocks;
        }
    }
    std::unordered_set<const Block*> blocks;
    auto loopSuccessor = branch->GetTrueBlock() == exitSuccessor ? branch->GetFalseBlock() : branch->GetTrueBlock();
    std::vector<const Block*> worklist{loopSuccessor};
    std::unordered_set<const Block*> visited;
    while (!worklist.empty()) {
        auto block = worklist.back();
        worklist.pop_back();
        if (block == nullptr || !visited.emplace(block).second) {
            continue;
        }
        if (block != header) {
            std::unordered_set<const Block*> reachHeader;
            if (!CanReachBlockAvoidingHeader(block, header, exitSuccessor, reachHeader)) {
                continue;
            }
        }
        blocks.emplace(block);
        if (block == header) {
            continue;
        }
        for (auto successorBlock : block->GetSuccessors()) {
            std::unordered_set<const Block*> reachHeader;
            if (CanReachBlockAvoidingHeader(successorBlock, header, exitSuccessor, reachHeader)) {
                worklist.emplace_back(successorBlock);
            }
        }
    }
    return blocks;
}

enum class LocalStoreLookupKind : uint8_t { NOT_FOUND, FOUND, UNKNOWN };

struct LocalStoreLookupResult {
    LocalStoreLookupKind kind{LocalStoreLookupKind::NOT_FOUND};
    int64_t value{0};
};

LocalStoreLookupResult MergeLocalStoreLookup(LocalStoreLookupResult lhs, LocalStoreLookupResult rhs)
{
    if (lhs.kind == LocalStoreLookupKind::UNKNOWN || rhs.kind == LocalStoreLookupKind::UNKNOWN) {
        return {LocalStoreLookupKind::UNKNOWN, 0};
    }
    if (lhs.kind == LocalStoreLookupKind::NOT_FOUND) {
        return rhs;
    }
    if (rhs.kind == LocalStoreLookupKind::NOT_FOUND) {
        return lhs;
    }
    if (lhs.value != rhs.value) {
        return {LocalStoreLookupKind::UNKNOWN, 0};
    }
    return lhs;
}

LocalStoreLookupResult FindLatestSignedStoreConstantAvoidingBlocks(const Block* block, Value* location,
    const std::unordered_set<const Block*>& blocked, std::unordered_set<const Block*>& visited, size_t depth)
{
    constexpr size_t MAX_BACKWARD_STORE_LOOKUP_DEPTH = 32;
    if (block == nullptr || depth > MAX_BACKWARD_STORE_LOOKUP_DEPTH) {
        return {LocalStoreLookupKind::UNKNOWN, 0};
    }
    if (blocked.find(block) != blocked.end() || !visited.emplace(block).second) {
        return {LocalStoreLookupKind::NOT_FOUND, 0};
    }
    auto exprs = block->GetExpressions();
    for (auto it = exprs.rbegin(); it != exprs.rend(); ++it) {
        if ((*it)->GetExprKind() != ExprKind::STORE) {
            continue;
        }
        auto store = StaticCast<const Store*>(*it);
        if (store->GetLocation() != location) {
            continue;
        }
        auto constant = GetSignedConstantFromDefiningConstant(store->GetValue());
        if (!constant.has_value()) {
            return {LocalStoreLookupKind::UNKNOWN, 0};
        }
        return {LocalStoreLookupKind::FOUND, constant.value()};
    }

    LocalStoreLookupResult result{LocalStoreLookupKind::NOT_FOUND, 0};
    for (auto pred : block->GetPredecessors()) {
        result = MergeLocalStoreLookup(
            result, FindLatestSignedStoreConstantAvoidingBlocks(pred, location, blocked, visited, depth + 1));
        if (result.kind == LocalStoreLookupKind::UNKNOWN) {
            return result;
        }
    }
    return result;
}

std::optional<int64_t> FindIncomingSignedStoreConstantBeforeLoop(
    const Block* header, Value* location, const std::unordered_set<const Block*>& loopBlocks)
{
    LocalStoreLookupResult result{LocalStoreLookupKind::NOT_FOUND, 0};
    for (auto pred : header->GetPredecessors()) {
        std::unordered_set<const Block*> visited;
        result = MergeLocalStoreLookup(
            result, FindLatestSignedStoreConstantAvoidingBlocks(pred, location, loopBlocks, visited, 0));
        if (result.kind == LocalStoreLookupKind::UNKNOWN) {
            return std::nullopt;
        }
    }
    return result.kind == LocalStoreLookupKind::FOUND ? std::optional<int64_t>{result.value} : std::nullopt;
}

std::optional<int64_t> GetSingleSignedValueFromState(const RangeDomain& state, Value* value)
{
    if (value == nullptr || !value->GetType()->IsInteger()) {
        return std::nullopt;
    }
    if (auto range = GetSIntRangeFromState(state, value);
        range != nullptr && range->GetExactValues().has_value() && range->GetExactValues()->size() == 1) {
        auto exact = range->GetExactValues()->front();
        if (value->GetType()->IsUnsignedInteger()) {
            return exact.UVal() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                ? std::optional<int64_t>{static_cast<int64_t>(exact.UVal())}
                : std::nullopt;
        }
        return exact.SVal();
    }
    const auto& domain = RangeAnalysis::GetSIntDomainFromState(state, value);
    if (!domain.IsSingleValue()) {
        return std::nullopt;
    }
    auto exact = domain.NumericBound().GetSingleElement();
    if (value->GetType()->IsUnsignedInteger()) {
        return exact.UVal() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
            ? std::optional<int64_t>{static_cast<int64_t>(exact.UVal())}
            : std::nullopt;
    }
    return exact.SVal();
}

LocalStoreLookupResult FindLatestSignedStoreValueAvoidingBlocks(const RangeDomain& state, const Block* block,
    Value* location, const std::unordered_set<const Block*>& blocked, std::unordered_set<const Block*>& visited,
    size_t depth)
{
    constexpr size_t MAX_BACKWARD_STORE_LOOKUP_DEPTH = 32;
    if (block == nullptr || depth > MAX_BACKWARD_STORE_LOOKUP_DEPTH) {
        return {LocalStoreLookupKind::UNKNOWN, 0};
    }
    if (blocked.find(block) != blocked.end() || !visited.emplace(block).second) {
        return {LocalStoreLookupKind::NOT_FOUND, 0};
    }
    auto expressions = block->GetExpressions();
    for (auto it = expressions.rbegin(); it != expressions.rend(); ++it) {
        if ((*it)->GetExprKind() != ExprKind::STORE) {
            continue;
        }
        auto store = StaticCast<const Store*>(*it);
        if (store->GetLocation() != location) {
            continue;
        }
        auto storedValue = GetSignedConstantFromDefiningConstant(store->GetValue());
        if (!storedValue.has_value()) {
            storedValue = GetSingleSignedValueFromState(state, store->GetValue());
        }
        return storedValue.has_value()
            ? LocalStoreLookupResult{LocalStoreLookupKind::FOUND, storedValue.value()}
            : LocalStoreLookupResult{LocalStoreLookupKind::UNKNOWN, 0};
    }

    LocalStoreLookupResult result{LocalStoreLookupKind::NOT_FOUND, 0};
    for (auto predecessor : block->GetPredecessors()) {
        result = MergeLocalStoreLookup(result,
            FindLatestSignedStoreValueAvoidingBlocks(
                state, predecessor, location, blocked, visited, depth + 1));
        if (result.kind == LocalStoreLookupKind::UNKNOWN) {
            return result;
        }
    }
    return result;
}

std::optional<int64_t> FindIncomingSignedStoreValueBeforeLoop(const RangeDomain& state,
    const Block* header, Value* location, const std::unordered_set<const Block*>& loopBlocks)
{
    LocalStoreLookupResult result{LocalStoreLookupKind::NOT_FOUND, 0};
    for (auto predecessor : header->GetPredecessors()) {
        std::unordered_set<const Block*> visited;
        result = MergeLocalStoreLookup(result,
            FindLatestSignedStoreValueAvoidingBlocks(
                state, predecessor, location, loopBlocks, visited, 0));
        if (result.kind == LocalStoreLookupKind::UNKNOWN) {
            return std::nullopt;
        }
    }
    return result.kind == LocalStoreLookupKind::FOUND ? std::optional<int64_t>{result.value} : std::nullopt;
}

std::optional<VariableBoundInductionExit> TryBuildVariableBoundInductionCandidate(
    const RangeDomain& state, Value* inductionValue, Value* boundValue, const Branch* branch, const Block* successor,
    RelationalOperation loopRelation)
{
    auto location = GetLoadLocation(inductionValue);
    auto loadExpr = GetDefiningExpr(inductionValue);
    if (location == nullptr || loadExpr == nullptr || loadExpr->GetParentBlock() == nullptr ||
        !inductionValue->GetType()->IsInteger() || boundValue == nullptr ||
        !boundValue->GetType()->IsInteger() ||
        inductionValue->GetType()->IsUnsignedInteger() != boundValue->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }
    auto header = loadExpr->GetParentBlock();
    auto loopBlocks = CollectLoopBackPathBlockSet(branch, successor, header);
    auto init = FindIncomingSignedStoreConstant(header, location);
    if (!init.has_value()) {
        init = FindIncomingSignedStoreConstantThroughPredecessors(header, location);
    }
    if (!init.has_value()) {
        init = FindIncomingSignedStoreConstantBeforeLoop(header, location, loopBlocks);
    }
    if (!init.has_value()) {
        init = FindIncomingSignedStoreValueBeforeLoop(state, header, location, loopBlocks);
    }
    auto step = FindSingleStepOnLoopBackPath(branch, successor, header, location);
    auto distance = ShortestBlockDistance(header, branch->GetParentBlock());
    if (!init.has_value() || !step.has_value() || step.value() != 1) {
        return std::nullopt;
    }
    if (auto boundLocation = GetLoadLocation(boundValue); boundLocation != nullptr) {
        auto boundStep = FindSingleStepOnLoopBackPath(branch, successor, header, boundLocation);
        if (boundStep.has_value() && boundStep.value() != 0) {
            return std::nullopt;
        }
    }
    if (!distance.has_value()) {
        return std::nullopt;
    }
    return VariableBoundInductionExit{inductionValue, location, boundValue, header, init.value(), step.value(),
        loopRelation};
}

Value* ResolveLocallyForwardedBooleanCondition(Value* condition)
{
    constexpr size_t MAX_LOCAL_CONDITION_FORWARD_DEPTH = 4;
    auto current = condition;
    std::unordered_set<Value*> visitedLocations;
    for (size_t depth = 0; depth < MAX_LOCAL_CONDITION_FORWARD_DEPTH; ++depth) {
        auto loadExpression = GetDefiningExpr(current);
        if (loadExpression == nullptr || loadExpression->GetExprKind() != ExprKind::LOAD ||
            current->GetType() == nullptr || !current->GetType()->IsBoolean()) {
            return current;
        }
        auto location = StaticCast<const Load*>(loadExpression)->GetLocation();
        auto locationDefinition = GetDefiningExpr(location);
        if (locationDefinition == nullptr || locationDefinition->GetExprKind() != ExprKind::ALLOCATE ||
            !visitedLocations.emplace(location).second) {
            return condition;
        }
        for (auto user : location->GetUsers()) {
            if (user == nullptr || user->GetExprKind() == ExprKind::DEBUGEXPR ||
                (user->GetExprKind() == ExprKind::LOAD &&
                    StaticCast<const Load*>(user)->GetLocation() == location) ||
                (user->GetExprKind() == ExprKind::STORE &&
                    StaticCast<const Store*>(user)->GetLocation() == location)) {
                continue;
            }
            return condition;
        }
        auto block = loadExpression->GetParentBlock();
        if (block == nullptr) {
            return condition;
        }
        auto expressions = block->GetExpressions();
        auto loadPosition = std::find(expressions.begin(), expressions.end(), loadExpression);
        if (loadPosition == expressions.end()) {
            return condition;
        }
        Value* storedCondition = nullptr;
        for (auto it = std::make_reverse_iterator(loadPosition); it != expressions.rend(); ++it) {
            auto expression = *it;
            if (expression->GetExprKind() == ExprKind::APPLY ||
                expression->GetExprKind() == ExprKind::INVOKE ||
                expression->GetExprKind() == ExprKind::APPLY_WITH_EXCEPTION ||
                expression->GetExprKind() == ExprKind::INVOKE_WITH_EXCEPTION) {
                return condition;
            }
            if (expression->GetExprKind() == ExprKind::STORE &&
                StaticCast<const Store*>(expression)->GetLocation() == location) {
                storedCondition = StaticCast<const Store*>(expression)->GetValue();
                break;
            }
        }
        if (storedCondition == nullptr || storedCondition->GetType() == nullptr ||
            !storedCondition->GetType()->IsBoolean()) {
            return condition;
        }
        current = storedCondition;
    }
    return current;
}

std::optional<VariableBoundInductionExit> GetVariableBoundInductionExit(
    const RangeDomain& state, const Branch* branch, const Block* successor,
    bool resolveForwardedCondition = false)
{
    auto condition = resolveForwardedCondition
        ? ResolveLocallyForwardedBooleanCondition(branch->GetCondition())
        : branch->GetCondition();
    auto expr = GetDefiningExpr(condition);
    if (expr == nullptr || !IsRelationalExprKind(expr->GetExprKind())) {
        return std::nullopt;
    }
    auto rel = ToRelationalOperation(expr->GetExprKind());
    bool branchCondition = successor == branch->GetTrueBlock();
    auto exitRel = branchCondition ? rel : NegateRelation(rel);
    auto loopRelation = branchCondition ? NegateRelation(rel) : rel;
    if (exitRel == RelationalOperation::EQ) {
        loopRelation = RelationalOperation::LE;
    } else if (loopRelation != RelationalOperation::LT && loopRelation != RelationalOperation::LE &&
        loopRelation != RelationalOperation::GT && loopRelation != RelationalOperation::GE) {
        return std::nullopt;
    }

    auto binary = StaticCast<const BinaryExpression*>(expr);
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    std::vector<std::pair<size_t, VariableBoundInductionExit>> candidates;
    auto lhsCandidate = TryBuildVariableBoundInductionCandidate(state, lhs, rhs, branch, successor, loopRelation);
    if (lhsCandidate.has_value()) {
        auto candidate = lhsCandidate.value();
        auto distance = ShortestBlockDistance(candidate.header, branch->GetParentBlock());
        candidates.emplace_back(distance.value(), candidate);
    }
    auto rhsCandidate =
        TryBuildVariableBoundInductionCandidate(state, rhs, lhs, branch, successor, SwapRelation(loopRelation));
    if (rhsCandidate.has_value()) {
        auto candidate = rhsCandidate.value();
        auto distance = ShortestBlockDistance(candidate.header, branch->GetParentBlock());
        candidates.emplace_back(distance.value(), candidate);
    }
    if (candidates.empty()) {
        return std::nullopt;
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& lhsCandidate, const auto& rhsCandidate) {
        return lhsCandidate.first < rhsCandidate.first;
    });
    if (candidates.size() > 1 && candidates[0].first == candidates[1].first) {
        return std::nullopt;
    }
    auto candidate = candidates.front().second;
    if (successor == candidate.header) {
        return std::nullopt;
    }
    std::unordered_set<const Block*> visited;
    if (CanReachBlockAvoidingHeader(successor, branch->GetParentBlock(), candidate.header, visited)) {
        return std::nullopt;
    }
    return candidate;
}

Type* GetIntegerRefRootType(Value* location)
{
    if (location == nullptr || !location->GetType()->IsRef()) {
        return nullptr;
    }
    auto refType = StaticCast<RefType*>(location->GetType());
    auto rootType = refType->GetRootBaseType();
    return rootType != nullptr && rootType->IsInteger() ? rootType : nullptr;
}

Type* GetBoundedLoopRefRootType(Value* location)
{
    if (location == nullptr || !location->GetType()->IsRef()) {
        return nullptr;
    }
    auto rootType = StaticCast<RefType*>(location->GetType())->GetRootBaseType();
    return rootType != nullptr &&
        (rootType->IsInteger() || rootType->IsBoolean() || rootType->IsEnum() || rootType->IsTuple())
        ? rootType
        : nullptr;
}

enum class StoreLookupKind : uint8_t { NOT_FOUND, FOUND, UNKNOWN };

struct StoreLookupResult {
    StoreLookupKind kind{StoreLookupKind::NOT_FOUND};
    int64_t value{0};
};

StoreLookupResult MergeStoreLookup(StoreLookupResult lhs, StoreLookupResult rhs)
{
    if (lhs.kind == StoreLookupKind::UNKNOWN || rhs.kind == StoreLookupKind::UNKNOWN) {
        return {StoreLookupKind::UNKNOWN, 0};
    }
    if (lhs.kind == StoreLookupKind::NOT_FOUND) {
        return rhs;
    }
    if (rhs.kind == StoreLookupKind::NOT_FOUND) {
        return lhs;
    }
    if (lhs.value != rhs.value) {
        return {StoreLookupKind::UNKNOWN, 0};
    }
    return lhs;
}

StoreLookupResult FindLatestSignedStoreConstantAtOrBeforeBlock(
    const Block* block, Value* location, std::unordered_set<const Block*>& visited, size_t depth)
{
    constexpr size_t MAX_BACKWARD_STORE_LOOKUP_DEPTH = 32;
    if (block == nullptr || depth > MAX_BACKWARD_STORE_LOOKUP_DEPTH) {
        return {StoreLookupKind::UNKNOWN, 0};
    }
    if (!visited.emplace(block).second) {
        return {StoreLookupKind::NOT_FOUND, 0};
    }
    auto exprs = block->GetExpressions();
    for (auto it = exprs.rbegin(); it != exprs.rend(); ++it) {
        if ((*it)->GetExprKind() != ExprKind::STORE) {
            continue;
        }
        auto store = StaticCast<const Store*>(*it);
        if (store->GetLocation() != location) {
            continue;
        }
        auto constant = GetSignedConstantFromDefiningConstant(store->GetValue());
        if (!constant.has_value()) {
            return {StoreLookupKind::UNKNOWN, 0};
        }
        return {StoreLookupKind::FOUND, constant.value()};
    }

    StoreLookupResult result{StoreLookupKind::NOT_FOUND, 0};
    for (auto pred : block->GetPredecessors()) {
        result = MergeStoreLookup(
            result, FindLatestSignedStoreConstantAtOrBeforeBlock(pred, location, visited, depth + 1));
        if (result.kind == StoreLookupKind::UNKNOWN) {
            return result;
        }
    }
    return result;
}

std::optional<int64_t> FindIncomingSignedStoreConstantThroughPredecessors(const Block* header, Value* location)
{
    StoreLookupResult result{StoreLookupKind::NOT_FOUND, 0};
    for (auto pred : header->GetPredecessors()) {
        if (IsBackedgePredecessor(header, pred)) {
            continue;
        }
        std::unordered_set<const Block*> visited;
        result = MergeStoreLookup(result, FindLatestSignedStoreConstantAtOrBeforeBlock(pred, location, visited, 0));
        if (result.kind == StoreLookupKind::UNKNOWN) {
            return std::nullopt;
        }
    }
    return result.kind == StoreLookupKind::FOUND ? std::optional<int64_t>{result.value} : std::nullopt;
}

std::vector<const Block*> CollectLoopBackPathBlocks(
    const VariableBoundInductionExit& induction, const Branch* branch, const Block* exitSuccessor)
{
    auto blockSet = CollectLoopBackPathBlockSet(branch, exitSuccessor, induction.header);
    std::vector<const Block*> blocks{blockSet.begin(), blockSet.end()};
    return blocks;
}

bool CanReachLoopHeaderAvoidingUpdate(const Block* start, const Block* header, const Block* updateBlock,
    const std::unordered_set<const Block*>& loopBlocks)
{
    std::vector<const Block*> worklist{start};
    std::unordered_set<const Block*> visited;
    while (!worklist.empty()) {
        auto block = worklist.back();
        worklist.pop_back();
        if (block == nullptr || block == updateBlock || !visited.emplace(block).second) {
            continue;
        }
        if (block == header) {
            return true;
        }
        if (loopBlocks.find(block) == loopBlocks.end()) {
            continue;
        }
        for (auto successor : block->GetSuccessors()) {
            worklist.emplace_back(successor);
        }
    }
    return false;
}

bool IsProvenSingleExecutionPerLoopIteration(const VariableBoundInductionExit& induction,
    const Branch* branch, const Block* exitSuccessor, const Block* updateBlock,
    const std::unordered_set<const Block*>& loopBlocks)
{
    if (branch == nullptr || induction.header == nullptr || updateBlock == nullptr ||
        updateBlock == induction.header || loopBlocks.find(updateBlock) == loopBlocks.end()) {
        return false;
    }

    for (auto successor : updateBlock->GetSuccessors()) {
        std::unordered_set<const Block*> visited;
        if (CanReachBlockAvoidingHeader(successor, updateBlock, induction.header, visited)) {
            return false;
        }
    }

    for (auto block : loopBlocks) {
        if (block == induction.header) {
            continue;
        }
        for (auto successor : block->GetSuccessors()) {
            if (successor != induction.header && loopBlocks.find(successor) == loopBlocks.end()) {
                return false;
            }
        }
    }

    auto loopEntry = branch->GetTrueBlock() == exitSuccessor
        ? branch->GetFalseBlock()
        : branch->GetTrueBlock();
    return !CanReachLoopHeaderAvoidingUpdate(
        loopEntry, induction.header, updateBlock, loopBlocks);
}

std::vector<CountedAccumulatorUpdate> CollectLinearCountedAccumulatorUpdates(
    const VariableBoundInductionExit& induction, const Branch* branch, const Block* exitSuccessor)
{
    std::vector<CountedAccumulatorUpdate> updates;
    auto blocks = CollectLoopBackPathBlocks(induction, branch, exitSuccessor);
    std::unordered_set<const Block*> loopBlockSet{blocks.begin(), blocks.end()};

    std::unordered_map<Value*, size_t> updateIndexes;
    std::unordered_set<Value*> invalidLocations;
    for (auto block : blocks) {
        for (auto expr : block->GetExpressions()) {
            if (expr->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(expr);
            auto location = store->GetLocation();
            if (location == induction.location || invalidLocations.find(location) != invalidLocations.end()) {
                continue;
            }
            auto rootType = GetIntegerRefRootType(location);
            if (rootType == nullptr) {
                continue;
            }
            if (!IsProvenSingleExecutionPerLoopIteration(
                    induction, branch, exitSuccessor, block, loopBlockSet)) {
                invalidLocations.emplace(location);
                updateIndexes.erase(location);
                continue;
            }
            auto step = GetUpdateStepFromLocation(store->GetValue(), location);
            if (!step.has_value() || step.value() == 0) {
                invalidLocations.emplace(location);
                updateIndexes.erase(location);
                continue;
            }
            if (updateIndexes.find(location) != updateIndexes.end()) {
                invalidLocations.emplace(location);
                updateIndexes.erase(location);
                continue;
            }
            auto init = FindIncomingSignedStoreConstantBeforeLoop(induction.header, location, loopBlockSet);
            if (!init.has_value()) {
                init = FindIncomingSignedStoreConstantThroughPredecessors(induction.header, location);
            }
            if (!init.has_value()) {
                invalidLocations.emplace(location);
                continue;
            }
            updateIndexes.emplace(location, updates.size());
            updates.emplace_back(CountedAccumulatorUpdate{location, rootType, init.value(), step.value()});
        }
    }

    updates.erase(std::remove_if(updates.begin(), updates.end(),
                      [&invalidLocations](const auto& update) {
                          return invalidLocations.find(update.location) != invalidLocations.end();
                      }),
        updates.end());
    return updates;
}

std::optional<std::vector<int64_t>> EnumerateSmallSignedValues(const SIntDomain& domain)
{
    if (domain.IsTop() || domain.IsBottom() || !domain.SymbolicBounds().Empty()) {
        return std::nullopt;
    }
    const auto& numeric = domain.NumericBound();
    if (numeric.IsFullSet() || numeric.IsEmptySet() || numeric.IsWrappedSet() || numeric.IsSignWrappedSet()) {
        return std::nullopt;
    }
    int64_t min = 0;
    int64_t max = 0;
    if (domain.IsUnsigned()) {
        auto unsignedMin = numeric.UMinValue().UVal();
        auto unsignedMax = numeric.UMaxValue().UVal();
        if (unsignedMax > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return std::nullopt;
        }
        min = static_cast<int64_t>(unsignedMin);
        max = static_cast<int64_t>(unsignedMax);
    } else {
        min = numeric.SMinValue().SVal();
        max = numeric.SMaxValue().SVal();
    }
    if (max < min) {
        return std::nullopt;
    }
    auto count = static_cast<__int128>(max) - static_cast<__int128>(min) + 1;
    if (count <= 0 || ExceedsExactIntSetLimit(count)) {
        return std::nullopt;
    }
    std::vector<int64_t> values;
    values.reserve(static_cast<size_t>(count));
    for (int64_t value = min;; ++value) {
        values.emplace_back(value);
        if (value == max) {
            break;
        }
    }
    return values;
}

std::optional<std::vector<int64_t>> GetSmallSignedValuesFromState(
    const RangeDomain& state, Value* value, Type* explicitType = nullptr)
{
    if (value == nullptr) {
        return std::nullopt;
    }
    auto type = explicitType != nullptr ? explicitType : value->GetType();
    if (type == nullptr || !type->IsInteger()) {
        return std::nullopt;
    }
    if (auto range = GetSIntRangeFromState(state, value, type);
        range != nullptr && range->GetExactValues().has_value()) {
        std::vector<int64_t> values;
        values.reserve(range->GetExactValues()->size());
        for (const auto& exactValue : *range->GetExactValues()) {
            if (type->IsUnsignedInteger()) {
                if (exactValue.UVal() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                    return std::nullopt;
                }
                values.emplace_back(static_cast<int64_t>(exactValue.UVal()));
            } else {
                values.emplace_back(exactValue.SVal());
            }
        }
        return values;
    }
    if (auto values = EnumerateSmallSignedValues(
            ::Cangjie::CHIR::GetSIntDomainFromState(state, value, type));
        values.has_value()) {
        return values;
    }
    return value->GetType() == nullptr ? std::nullopt : TryEnumerateSimpleLoopLoadValues(state, value);
}


std::optional<int64_t> NarrowToInt64(__int128 value)
{
    if (value < static_cast<__int128>(std::numeric_limits<int64_t>::min()) ||
        value > static_cast<__int128>(std::numeric_limits<int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<int64_t>(value);
}

std::vector<AffineAccumulatorUpdate> CollectAffineCountedAccumulatorUpdates(
    const VariableBoundInductionExit& induction, const Branch* branch, const Block* exitSuccessor)
{
    std::vector<AffineAccumulatorUpdate> updates;
    auto blocks = CollectLoopBackPathBlocks(induction, branch, exitSuccessor);
    std::unordered_set<const Block*> loopBlockSet{blocks.begin(), blocks.end()};
    std::unordered_map<Value*, size_t> updateIndexes;
    std::unordered_set<Value*> invalidLocations;
    for (auto block : blocks) {
        for (auto expr : block->GetExpressions()) {
            if (expr->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(expr);
            auto location = store->GetLocation();
            if (location == induction.location || invalidLocations.find(location) != invalidLocations.end()) {
                continue;
            }
            auto rootType = GetIntegerRefRootType(location);
            if (rootType == nullptr) {
                continue;
            }
            if (!IsProvenSingleExecutionPerLoopIteration(
                    induction, branch, exitSuccessor, block, loopBlockSet)) {
                invalidLocations.emplace(location);
                updateIndexes.erase(location);
                continue;
            }
            auto form = GetAffineAccumulatorForm(store->GetValue(), location, induction.location);
            auto inductionCoefficient = form.has_value()
                ? NarrowToInt64(form->inductionCoefficient)
                : std::nullopt;
            auto constant = form.has_value() ? NarrowToInt64(form->constant) : std::nullopt;
            if (!form.has_value() || form->accumulatorCoefficient != 1 ||
                !inductionCoefficient.has_value() || !constant.has_value() ||
                (inductionCoefficient.value() == 0 && constant.value() == 0) ||
                updateIndexes.find(location) != updateIndexes.end()) {
                invalidLocations.emplace(location);
                updateIndexes.erase(location);
                continue;
            }
            auto init = FindIncomingSignedStoreConstantBeforeLoop(induction.header, location, loopBlockSet);
            if (!init.has_value()) {
                init = FindIncomingSignedStoreConstantThroughPredecessors(induction.header, location);
            }
            if (!init.has_value()) {
                invalidLocations.emplace(location);
                continue;
            }
            updateIndexes.emplace(location, updates.size());
            updates.emplace_back(AffineAccumulatorUpdate{location, rootType, init.value(),
                inductionCoefficient.value(), constant.value()});
        }
    }
    updates.erase(std::remove_if(updates.begin(), updates.end(), [&invalidLocations](const auto& update) {
        return invalidLocations.find(update.location) != invalidLocations.end();
    }), updates.end());
    return updates;
}

struct LockstepInductionProof {
    int64_t inductionAnchor;
    int64_t offset;
};

std::unordered_set<const Block*> CollectPredecessorClosure(const Block* header)
{
    std::unordered_set<const Block*> blocks;
    std::vector<const Block*> worklist{header};
    while (!worklist.empty()) {
        auto block = worklist.back();
        worklist.pop_back();
        if (block == nullptr || !blocks.emplace(block).second) {
            continue;
        }
        for (auto predecessor : block->GetPredecessors()) {
            worklist.emplace_back(predecessor);
        }
    }
    return blocks;
}

bool CanReachTargetAvoidingBlock(const Block* start, const Block* target, const Block* forbidden,
    const std::unordered_set<const Block*>& allowed)
{
    std::unordered_set<const Block*> visited;
    std::vector<const Block*> worklist{start};
    while (!worklist.empty()) {
        auto block = worklist.back();
        worklist.pop_back();
        if (block == nullptr || block == forbidden || !visited.emplace(block).second) {
            continue;
        }
        if (block == target) {
            return true;
        }
        if (allowed.find(block) == allowed.end()) {
            continue;
        }
        for (auto successor : block->GetSuccessors()) {
            worklist.emplace_back(successor);
        }
    }
    return false;
}

std::optional<LockstepInductionProof> ProveLockstepInductionRelation(const Block* header,
    Value* inductionLocation, Value* candidateLocation,
    const std::unordered_set<const Block*>& loopBlocks)
{
    auto predecessorClosure = CollectPredecessorClosure(header);
    const Block* anchorBlock = nullptr;
    std::optional<int64_t> inductionAnchor;
    std::optional<int64_t> offset;
    bool sawLoopUpdate = false;

    for (auto block : predecessorClosure) {
        std::vector<const Store*> inductionStores;
        std::vector<const Store*> candidateStores;
        for (auto expression : block->GetExpressions()) {
            if (expression->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(expression);
            if (store->GetLocation() == inductionLocation) {
                inductionStores.emplace_back(store);
            } else if (store->GetLocation() == candidateLocation) {
                candidateStores.emplace_back(store);
            }
        }
        if (inductionStores.size() != candidateStores.size()) {
            return std::nullopt;
        }
        for (size_t index = 0; index < inductionStores.size(); ++index) {
            auto inductionConstant =
                GetSignedConstantFromDefiningConstant(inductionStores[index]->GetValue());
            auto candidateConstant =
                GetSignedConstantFromDefiningConstant(candidateStores[index]->GetValue());
            if (inductionConstant.has_value() && candidateConstant.has_value()) {
                if (loopBlocks.find(block) != loopBlocks.end() ||
                    (anchorBlock != nullptr && anchorBlock != block)) {
                    return std::nullopt;
                }
                auto currentOffset = NarrowToInt64(static_cast<__int128>(candidateConstant.value()) -
                    static_cast<__int128>(inductionConstant.value()));
                if (!currentOffset.has_value() ||
                    (inductionAnchor.has_value() && inductionAnchor.value() != inductionConstant.value()) ||
                    (offset.has_value() && offset.value() != currentOffset.value())) {
                    return std::nullopt;
                }
                anchorBlock = block;
                inductionAnchor = inductionConstant.value();
                offset = currentOffset.value();
                continue;
            }

            auto inductionUpdate =
                GetUpdateStepFromLocation(inductionStores[index]->GetValue(), inductionLocation);
            auto candidateUpdate =
                GetUpdateStepFromLocation(candidateStores[index]->GetValue(), candidateLocation);
            if (!inductionUpdate.has_value() || !candidateUpdate.has_value() ||
                inductionUpdate.value() != candidateUpdate.value() ||
                loopBlocks.find(block) == loopBlocks.end()) {
                return std::nullopt;
            }
            sawLoopUpdate = true;
        }
    }

    if (anchorBlock == nullptr || !inductionAnchor.has_value() || !offset.has_value() || !sawLoopUpdate) {
        return std::nullopt;
    }
    for (auto block : predecessorClosure) {
        if (!block->GetPredecessors().empty()) {
            continue;
        }
        if (CanReachTargetAvoidingBlock(block, header, anchorBlock, predecessorClosure)) {
            return std::nullopt;
        }
    }
    return LockstepInductionProof{inductionAnchor.value(), offset.value()};
}

void SetExactSignedRange(RangeDomain& state, Value* value, Type* type, int64_t exactValue)
{
    if (value == nullptr || type == nullptr || !type->IsInteger() || type->IsUnsignedInteger()) {
        return;
    }
    auto exact = SInt{ToWidth(*type), static_cast<uint64_t>(exactValue)};
    state.Update(value, std::make_unique<SIntRange>(
        SIntDomain::FromNumeric(RelationalOperation::EQ, exact, false), std::vector<SInt>{exact}));
}

void SetSignedIntervalRange(RangeDomain& state, Value* value, Type* type, int64_t lower, int64_t upper)
{
    if (value == nullptr || type == nullptr || !type->IsInteger() || type->IsUnsignedInteger() || lower > upper) {
        return;
    }
    auto width = ToWidth(*type);
    if (!FitsSignedWidth(lower, width) || !FitsSignedWidth(upper, width)) {
        return;
    }
    auto min = SInt{width, static_cast<uint64_t>(lower)};
    auto max = SInt{width, static_cast<uint64_t>(upper)};
    state.Update(value, std::make_unique<SIntRange>(SIntDomain{RangeFromMinMax(min, max, false), false}));
}

bool TryNarrowLockstepInductionEdge(
    RangeDomain& state, const Branch* branch, const Block* successor)
{
    const Block* exitSuccessor = nullptr;
    for (auto candidate : {branch->GetTrueBlock(), branch->GetFalseBlock()}) {
        if (IsLoopExitSuccessor(branch, candidate)) {
            if (exitSuccessor != nullptr) {
                return true;
            }
            exitSuccessor = candidate;
        }
    }
    if (exitSuccessor == nullptr ||
        (successor != exitSuccessor && successor != branch->GetTrueBlock() && successor != branch->GetFalseBlock())) {
        return true;
    }
    auto condition = GetSimpleInductionCondition(branch->GetCondition());
    if (!condition.has_value() || condition->loadValue->GetType()->IsUnsignedInteger()) {
        return true;
    }
    auto header = branch->GetParentBlock();
    auto inductionStep = FindSingleBackedgeStep(header, condition->location);
    auto loopBlocks = CollectLoopBackPathBlockSet(branch, exitSuccessor, header);

    std::unordered_set<Value*> candidateLocations;
    for (auto block : loopBlocks) {
        for (auto expression : block->GetExpressions()) {
            if (expression->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(expression);
            auto location = store->GetLocation();
            auto type = GetIntegerRefRootType(location);
            auto step = GetUpdateStepFromLocation(store->GetValue(), location);
            if (location == condition->location || type == nullptr || type->IsUnsignedInteger() ||
                !step.has_value() || step.value() == 0) {
                continue;
            }
            candidateLocations.emplace(location);
        }
    }

    const bool isExitEdge = successor == exitSuccessor;
    std::optional<int64_t> inductionExact;
    std::optional<std::pair<int64_t, int64_t>> inductionBodyInterval;
    for (auto candidateLocation : candidateLocations) {
        auto proof = ProveLockstepInductionRelation(
            header, condition->location, candidateLocation, loopBlocks);
        if (!proof.has_value()) {
            continue;
        }
        std::optional<SInt> exact;
        const auto& constrainedInduction =
            RangeAnalysis::GetSIntDomainFromState(state, condition->loadValue);
        if (constrainedInduction.IsSingleValue()) {
            exact = constrainedInduction.NumericBound().GetSingleElement();
        } else if (inductionStep.has_value() && inductionStep.value() != 0) {
            exact = ComputeExactInductionExit(proof->inductionAnchor, inductionStep.value(),
                condition->relation, condition->bound, ToWidth(*condition->loadValue->GetType()));
        }
        if (!exact.has_value()) {
            continue;
        }
        auto candidateType = GetIntegerRefRootType(candidateLocation);
        if (candidateType == nullptr) {
            continue;
        }
        auto object = state.CheckAbstractObjectRefBy(candidateLocation);
        if (object == nullptr) {
            continue;
        }

        if (isExitEdge) {
            auto candidateExact = NarrowToInt64(
                static_cast<__int128>(exact->SVal()) + static_cast<__int128>(proof->offset));
            if (!candidateExact.has_value() ||
                !FitsSignedWidth(candidateExact.value(), ToWidth(*candidateType))) {
                continue;
            }
            inductionExact = exact->SVal();
            SetExactSignedRange(state, object, candidateType, candidateExact.value());
            continue;
        }

        if (!SatisfiesSignedRelation(proof->inductionAnchor, condition->relation, condition->bound)) {
            continue;
        }
        if (!inductionStep.has_value() || inductionStep.value() == 0) {
            continue;
        }
        auto lastBodyValue = NarrowToInt64(
            static_cast<__int128>(exact->SVal()) - static_cast<__int128>(inductionStep.value()));
        if (!lastBodyValue.has_value()) {
            continue;
        }
        auto inductionLower = std::min(proof->inductionAnchor, lastBodyValue.value());
        auto inductionUpper = std::max(proof->inductionAnchor, lastBodyValue.value());
        auto candidateLower = NarrowToInt64(
            static_cast<__int128>(inductionLower) + static_cast<__int128>(proof->offset));
        auto candidateUpper = NarrowToInt64(
            static_cast<__int128>(inductionUpper) + static_cast<__int128>(proof->offset));
        if (!candidateLower.has_value() || !candidateUpper.has_value()) {
            continue;
        }
        inductionBodyInterval = std::make_pair(inductionLower, inductionUpper);
        SetSignedIntervalRange(
            state, object, candidateType, candidateLower.value(), candidateUpper.value());
    }

    if (inductionExact.has_value()) {
        auto loadType = condition->loadValue->GetType();
        SetExactSignedRange(state, condition->loadValue, loadType, inductionExact.value());
        if (auto object = state.CheckAbstractObjectRefBy(condition->location); object != nullptr) {
            SetExactSignedRange(state, object, loadType, inductionExact.value());
        }
    } else if (inductionBodyInterval.has_value()) {
        auto loadType = condition->loadValue->GetType();
        SetSignedIntervalRange(state, condition->loadValue, loadType,
            inductionBodyInterval->first, inductionBodyInterval->second);
        if (auto object = state.CheckAbstractObjectRefBy(condition->location); object != nullptr) {
            SetSignedIntervalRange(state, object, loadType,
                inductionBodyInterval->first, inductionBodyInterval->second);
        }
    }
    return true;
}

std::optional<SignedInterval> CombineIntervalAdd(SignedInterval lhs, SignedInterval rhs)
{
    auto min = NarrowToInt64(static_cast<__int128>(lhs.min) + static_cast<__int128>(rhs.min));
    auto max = NarrowToInt64(static_cast<__int128>(lhs.max) + static_cast<__int128>(rhs.max));
    if (!min.has_value() || !max.has_value()) {
        return std::nullopt;
    }
    return SignedInterval{min.value(), max.value()};
}

std::optional<SignedInterval> CombineIntervalSub(SignedInterval lhs, SignedInterval rhs)
{
    auto min = NarrowToInt64(static_cast<__int128>(lhs.min) - static_cast<__int128>(rhs.max));
    auto max = NarrowToInt64(static_cast<__int128>(lhs.max) - static_cast<__int128>(rhs.min));
    if (!min.has_value() || !max.has_value()) {
        return std::nullopt;
    }
    return SignedInterval{min.value(), max.value()};
}

std::optional<SignedInterval> GetSignedIntervalFromSIntRange(const SIntRange* range)
{
    if (range == nullptr) {
        return std::nullopt;
    }
    if (range->GetExactValues().has_value() && !range->GetExactValues()->empty()) {
        std::vector<int64_t> values;
        values.reserve(range->GetExactValues()->size());
        for (const auto& exactValue : *range->GetExactValues()) {
            values.emplace_back(exactValue.SVal());
        }
        auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
        return SignedInterval{*minIt, *maxIt};
    }
    const auto& domain = range->GetVal();
    if (domain.IsUnsigned() || domain.IsTop() || domain.IsBottom() || !domain.SymbolicBounds().Empty()) {
        return std::nullopt;
    }
    const auto& numeric = domain.NumericBound();
    if (numeric.IsFullSet() || numeric.IsEmptySet() || numeric.IsWrappedSet() || numeric.IsSignWrappedSet()) {
        return std::nullopt;
    }
    return SignedInterval{numeric.SMinValue().SVal(), numeric.SMaxValue().SVal()};
}

std::optional<SignedInterval> GetSignedIntervalFromRangeValueDomain(const RangeValueDomain* domain)
{
    if (domain == nullptr || domain->IsTop()) {
        return std::nullopt;
    }
    auto absVal = domain->CheckAbsVal();
    if (absVal == nullptr || absVal->GetRangeKind() != ValueRange::RangeKind::SINT) {
        return std::nullopt;
    }
    return GetSignedIntervalFromSIntRange(StaticCast<const SIntRange*>(absVal));
}

bool ContainsLoadFromLocation(Value* value, Value* location, size_t depth = 0)
{
    constexpr size_t MAX_LOAD_SEARCH_DEPTH = 8;
    if (value == nullptr || location == nullptr || depth > MAX_LOAD_SEARCH_DEPTH) {
        return false;
    }
    if (IsLoadFromLocation(value, location)) {
        return true;
    }
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr) {
        return false;
    }
    for (auto operand : expr->GetOperands()) {
        if (ContainsLoadFromLocation(operand, location, depth + 1)) {
            return true;
        }
    }
    return false;
}

std::optional<SignedInterval> GetDirectSignedIntervalFromState(const RangeDomain& state, Value* value)
{
    if (value == nullptr || !value->GetType()->IsInteger() || value->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }
    if (auto constant = GetSignedConstantFromDefiningConstant(value); constant.has_value()) {
        return SignedInterval{constant.value(), constant.value()};
    }
    if (auto values = GetSmallSignedValuesFromState(state, value); values.has_value() && !values->empty()) {
        auto [minIt, maxIt] = std::minmax_element(values->begin(), values->end());
        return SignedInterval{*minIt, *maxIt};
    }
    if (auto range = GetSIntRangeFromState(state, value); range != nullptr) {
        return GetSignedIntervalFromSIntRange(range);
    }
    auto expr = GetDefiningExpr(value);
    if (expr != nullptr && expr->GetExprKind() == ExprKind::LOAD) {
        auto location = StaticCast<const Load*>(expr)->GetLocation();
        if (auto object = state.CheckAbstractObjectRefBy(location); object != nullptr) {
            return GetSignedIntervalFromRangeValueDomain(state.CheckAbstractValueWithTopBottom(object));
        }
    }
    return std::nullopt;
}

std::optional<SignedInterval> GetReachableLocalStoreInterval(const RangeDomain& state, const Load* load)
{
    if (load == nullptr || load->GetParentBlock() == nullptr) {
        return std::nullopt;
    }
    auto location = load->GetLocation();
    auto rootType = GetIntegerRefRootType(location);
    if (rootType == nullptr || rootType->IsUnsignedInteger()) {
        return std::nullopt;
    }

    constexpr size_t MAX_LOCAL_STORE_SCAN_BLOCKS = 128;
    auto loadBlock = load->GetParentBlock();
    std::vector<const Block*> worklist{loadBlock};
    std::unordered_set<const Block*> scheduled{loadBlock};
    std::optional<SignedInterval> result;
    for (size_t index = 0; index < worklist.size(); ++index) {
        auto block = worklist[index];
        if (block == nullptr) {
            continue;
        }
        if (block != loadBlock) {
            std::unordered_set<const Block*> reachLoad;
            if (!CanReachBlock(block, loadBlock, reachLoad)) {
                continue;
            }
        }
        for (auto expr : block->GetExpressions()) {
            if (expr->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(expr);
            if (store->GetLocation() != location) {
                continue;
            }
            if (ContainsLoadFromLocation(store->GetValue(), location)) {
                return std::nullopt;
            }
            auto interval = GetDirectSignedIntervalFromState(state, store->GetValue());
            if (!interval.has_value()) {
                return std::nullopt;
            }
            if (!result.has_value()) {
                result = interval.value();
            } else {
                result->min = std::min(result->min, interval->min);
                result->max = std::max(result->max, interval->max);
            }
        }
        for (auto pred : block->GetPredecessors()) {
            if (pred == nullptr || scheduled.find(pred) != scheduled.end()) {
                continue;
            }
            if (scheduled.size() >= MAX_LOCAL_STORE_SCAN_BLOCKS) {
                return std::nullopt;
            }
            scheduled.emplace(pred);
            worklist.emplace_back(pred);
        }
    }
    return result;
}

std::optional<SignedInterval> GetSignedIntervalFromState(const RangeDomain& state, Value* value)
{
    if (value == nullptr || !value->GetType()->IsInteger() || value->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }
    if (auto constant = GetSignedConstantFromDefiningConstant(value); constant.has_value()) {
        return SignedInterval{constant.value(), constant.value()};
    }
    if (auto values = GetSmallSignedValuesFromState(state, value); values.has_value() && !values->empty()) {
        auto [minIt, maxIt] = std::minmax_element(values->begin(), values->end());
        return SignedInterval{*minIt, *maxIt};
    }
    if (auto range = GetSIntRangeFromState(state, value); range != nullptr) {
        if (auto interval = GetSignedIntervalFromSIntRange(range); interval.has_value()) {
            return interval;
        }
    }
    auto expr = GetDefiningExpr(value);
    if (expr != nullptr && expr->GetExprKind() == ExprKind::LOAD) {
        auto load = StaticCast<const Load*>(expr);
        auto location = load->GetLocation();
        if (auto object = state.CheckAbstractObjectRefBy(location); object != nullptr) {
            if (auto interval = GetSignedIntervalFromRangeValueDomain(state.CheckAbstractValueWithTopBottom(object));
                interval.has_value()) {
                return interval;
            }
        }
        if (auto interval = GetReachableLocalStoreInterval(state, load); interval.has_value()) {
            return interval;
        }
    }
    return std::nullopt;
}

std::optional<AccumulatorDeltaInterval> GetAccumulatorDeltaIntervalFromLocation(
    const RangeDomain& state, Value* value, Value* location, size_t depth = 0)
{
    constexpr size_t MAX_ACCUMULATOR_DELTA_DEPTH = 8;
    if (value == nullptr || location == nullptr || depth > MAX_ACCUMULATOR_DELTA_DEPTH) {
        return std::nullopt;
    }
    if (IsLoadFromLocation(value, location)) {
        return AccumulatorDeltaInterval{1, SignedInterval{0, 0}};
    }
    auto expr = GetDefiningExpr(value);
    if (expr != nullptr && expr->GetExprKind() == ExprKind::TYPECAST) {
        auto source = StaticCast<const TypeCast*>(expr)->GetSourceValue();
        if (source != nullptr && source->GetType()->IsInteger() && !source->GetType()->IsUnsignedInteger() &&
            ToWidth(*source->GetType()) == ToWidth(*value->GetType())) {
            return GetAccumulatorDeltaIntervalFromLocation(state, source, location, depth + 1);
        }
    }
    if (expr != nullptr && (expr->GetExprKind() == ExprKind::ADD || expr->GetExprKind() == ExprKind::SUB)) {
        auto binary = StaticCast<const BinaryExpression*>(expr);
        auto lhs = GetAccumulatorDeltaIntervalFromLocation(state, binary->GetLHSOperand(), location, depth + 1);
        auto rhs = GetAccumulatorDeltaIntervalFromLocation(state, binary->GetRHSOperand(), location, depth + 1);
        if (!lhs.has_value() || !rhs.has_value() || lhs->loadCount + rhs->loadCount > 1) {
            return std::nullopt;
        }
        if (expr->GetExprKind() == ExprKind::ADD) {
            auto delta = CombineIntervalAdd(lhs->delta, rhs->delta);
            if (!delta.has_value()) {
                return std::nullopt;
            }
            return AccumulatorDeltaInterval{lhs->loadCount + rhs->loadCount, delta.value()};
        }
        if (rhs->loadCount != 0) {
            return std::nullopt;
        }
        auto delta = CombineIntervalSub(lhs->delta, rhs->delta);
        if (!delta.has_value()) {
            return std::nullopt;
        }
        return AccumulatorDeltaInterval{lhs->loadCount, delta.value()};
    }
    auto interval = GetSignedIntervalFromState(state, value);
    if (!interval.has_value()) {
        return std::nullopt;
    }
    return AccumulatorDeltaInterval{0, interval.value()};
}

std::optional<SIntRange> BuildSignedIntervalRange(Type* type, SignedInterval interval)
{
    if (type == nullptr || !type->IsInteger()) {
        return std::nullopt;
    }
    auto width = ToWidth(*type);
    if (!FitsModeledIntegerWidth(interval.min, type) || !FitsModeledIntegerWidth(interval.max, type)) {
        return std::nullopt;
    }
    auto lowerValue = SInt{width, static_cast<uint64_t>(interval.min)};
    auto upperValue = SInt{width, static_cast<uint64_t>(interval.max)};
    auto isUnsigned = type->IsUnsignedInteger();
    auto domain = SIntDomain::Intersects(SIntDomain::FromNumeric(RelationalOperation::GE, lowerValue, isUnsigned),
        SIntDomain::FromNumeric(RelationalOperation::LE, upperValue, isUnsigned));
    auto count = static_cast<__int128>(interval.max) - static_cast<__int128>(interval.min) + 1;
    if (count > 0 && !ExceedsExactIntSetLimit(count)) {
        std::vector<SInt> exact;
        exact.reserve(static_cast<size_t>(count));
        for (int64_t value = interval.min;; ++value) {
            exact.emplace_back(width, static_cast<uint64_t>(value));
            if (value == interval.max) {
                break;
            }
        }
        auto exactValues = NormalizeExactIntSet(std::move(exact));
        if (exactValues.has_value()) {
            return SIntRange{std::move(domain), std::move(exactValues)};
        }
    }
    return SIntRange{std::move(domain), std::nullopt};
}

std::optional<std::vector<int64_t>> GetCountedLoopTripCounts(
    const RangeDomain& state, const VariableBoundInductionExit& induction)
{
    auto boundValues = GetSmallSignedValuesFromState(state, induction.boundValue);
    if (!boundValues.has_value()) {
        return std::nullopt;
    }
    std::vector<int64_t> tripCounts;
    for (auto bound : *boundValues) {
        auto values = TryEnumerateInductionValues(
            induction.init, induction.step, induction.relation, bound, ToWidth(*induction.loadValue->GetType()));
        if (!values.has_value()) {
            return std::nullopt;
        }
        tripCounts.emplace_back(static_cast<int64_t>(values->size()));
    }
    return tripCounts.empty() ? std::nullopt : std::optional<std::vector<int64_t>>{std::move(tripCounts)};
}

std::optional<SignedInterval> BuildCountedAccumulatorBodyInterval(
    const RangeDomain& state, const VariableBoundInductionExit& induction, int64_t init, SignedInterval step)
{
    auto tripCounts = GetCountedLoopTripCounts(state, induction);
    if (!tripCounts.has_value()) {
        return std::nullopt;
    }
    std::optional<SignedInterval> result;
    for (auto tripCountValue : *tripCounts) {
        auto tripCount = static_cast<__int128>(tripCountValue);
        for (__int128 iteration = 0; iteration < tripCount; ++iteration) {
            auto lo = NarrowToInt64(static_cast<__int128>(init) + iteration * static_cast<__int128>(step.min));
            auto hi = NarrowToInt64(static_cast<__int128>(init) + iteration * static_cast<__int128>(step.max));
            if (!lo.has_value() || !hi.has_value()) {
                return std::nullopt;
            }
            auto current = SignedInterval{std::min(lo.value(), hi.value()), std::max(lo.value(), hi.value())};
            if (!result.has_value()) {
                result = current;
            } else {
                result->min = std::min(result->min, current.min);
                result->max = std::max(result->max, current.max);
            }
        }
    }
    return result;
}

std::optional<SignedInterval> BuildCountedAccumulatorUpdateInterval(
    const RangeDomain& state, const VariableBoundInductionExit& induction, int64_t init, SignedInterval step)
{
    auto tripCounts = GetCountedLoopTripCounts(state, induction);
    if (!tripCounts.has_value()) {
        return std::nullopt;
    }
    std::optional<SignedInterval> result;
    for (auto tripCountValue : *tripCounts) {
        auto tripCount = static_cast<__int128>(tripCountValue);
        for (__int128 iteration = 1; iteration <= tripCount; ++iteration) {
            auto lo = NarrowToInt64(static_cast<__int128>(init) + iteration * static_cast<__int128>(step.min));
            auto hi = NarrowToInt64(static_cast<__int128>(init) + iteration * static_cast<__int128>(step.max));
            if (!lo.has_value() || !hi.has_value()) {
                return std::nullopt;
            }
            auto current = SignedInterval{std::min(lo.value(), hi.value()), std::max(lo.value(), hi.value())};
            if (!result.has_value()) {
                result = current;
            } else {
                result->min = std::min(result->min, current.min);
                result->max = std::max(result->max, current.max);
            }
        }
    }
    return result;
}

std::optional<SInt> ComputeCountedAccumulatorValue(
    const VariableBoundInductionExit& induction, const CountedAccumulatorUpdate& update, int64_t bound)
{
    if (induction.step != 1) {
        return std::nullopt;
    }
    auto values = TryEnumerateInductionValues(
        induction.init, induction.step, induction.relation, bound, ToWidth(*induction.loadValue->GetType()));
    if (!values.has_value()) {
        return std::nullopt;
    }
    auto tripCount = static_cast<__int128>(values->size());
    auto value = static_cast<__int128>(update.init) + tripCount * static_cast<__int128>(update.step);
    auto width = ToWidth(*update.type);
    if (!FitsModeledIntegerWidth(value, update.type)) {
        return std::nullopt;
    }
    return SInt{width, static_cast<uint64_t>(static_cast<int64_t>(value))};
}

std::optional<SIntRange> BuildCountedAccumulatorExitRange(
    const RangeDomain& state, const VariableBoundInductionExit& induction, const CountedAccumulatorUpdate& update)
{
    auto boundValues = GetSmallSignedValuesFromState(state, induction.boundValue);
    if (!boundValues.has_value()) {
        return std::nullopt;
    }
    std::vector<SInt> values;
    values.reserve(boundValues->size());
    for (auto bound : *boundValues) {
        auto value = ComputeCountedAccumulatorValue(induction, update, bound);
        if (!value.has_value()) {
            return std::nullopt;
        }
        values.emplace_back(value.value());
    }
    auto exactValues = NormalizeExactIntSet(std::move(values));
    if (!exactValues.has_value()) {
        return std::nullopt;
    }
    auto domain = DomainFromExactIntValues(*exactValues, update.type->IsUnsignedInteger());
    return SIntRange{std::move(domain), std::move(exactValues)};
}

struct CountedAccumulatorLoopContext {
    VariableBoundInductionExit induction;
    const Branch* branch;
    const Block* exitSuccessor;
    std::unordered_set<const Block*> loopBlocks;
};

bool IsLoopBodyNestedBlock(const Block* block, const Block* header, const std::unordered_set<const Block*>& loopBlocks)
{
    if (block == nullptr || header == nullptr) {
        return false;
    }
    if (loopBlocks.find(block) != loopBlocks.end()) {
        return true;
    }
    bool reachedFromLoop = false;
    for (auto loopBlock : loopBlocks) {
        std::unordered_set<const Block*> visited;
        if (CanReachBlockAvoidingHeader(loopBlock, block, header, visited)) {
            reachedFromLoop = true;
            break;
        }
    }
    if (!reachedFromLoop) {
        return false;
    }
    std::unordered_set<const Block*> reachHeader;
    if (CanReachBlock(block, header, reachHeader)) {
        return true;
    }
    for (auto loopBlock : loopBlocks) {
        std::unordered_set<const Block*> visited;
        if (CanReachBlockAvoidingHeader(block, loopBlock, header, visited)) {
            return true;
        }
    }
    return false;
}

struct PairInductionLoop {
    Value* lhsValue;
    Value* lhsLocation;
    Value* rhsValue;
    Value* rhsLocation;
    const Block* header;
    const Branch* branch;
    const Block* exitSuccessor;
    int64_t lhsStep;
    int64_t rhsStep;
    RelationalOperation relation;
};

struct PairLoopValues {
    std::vector<int64_t> lhsBodyValues;
    std::vector<int64_t> rhsBodyValues;
    std::vector<int64_t> lhsExitValues;
    std::vector<int64_t> rhsExitValues;
};

std::optional<SIntRange> BuildIntegerExactRange(Type* type, std::vector<SInt> values)
{
    if (type == nullptr || !type->IsInteger() || values.empty()) {
        return std::nullopt;
    }
    auto width = ToWidth(*type);
    if (std::any_of(values.begin(), values.end(), [width](const SInt& value) {
        return value.Width() != width;
    })) {
        return std::nullopt;
    }
    auto exactValues = NormalizeExactIntSet(std::move(values));
    if (!exactValues.has_value()) {
        return std::nullopt;
    }
    auto domain = DomainFromExactIntValues(*exactValues, type->IsUnsignedInteger());
    return SIntRange{std::move(domain), std::move(exactValues)};
}

std::optional<SIntRange> BuildSignedExactRange(Type* type, const std::vector<int64_t>& values)
{
    if (type == nullptr || !type->IsInteger() || values.empty()) {
        return std::nullopt;
    }
    auto width = ToWidth(*type);
    std::vector<SInt> exact;
    exact.reserve(values.size());
    for (auto value : values) {
        if (!FitsModeledIntegerWidth(value, type)) {
            return std::nullopt;
        }
        exact.emplace_back(width, static_cast<uint64_t>(value));
    }
    auto exactValues = NormalizeExactIntSet(std::move(exact));
    if (!exactValues.has_value()) {
        return std::nullopt;
    }
    auto domain = DomainFromExactIntValues(*exactValues, type->IsUnsignedInteger());
    return SIntRange{std::move(domain), std::move(exactValues)};
}

std::optional<int64_t> FindSingleStepOrZeroOnLoopBackPath(
    const Branch* exitBranch, const Block* exitSuccessor, const Block* header, Value* location)
{
    auto step = FindSingleStepOnLoopBackPath(exitBranch, exitSuccessor, header, location);
    if (step.has_value()) {
        return step;
    }
    auto blocks = CollectLoopBackPathBlockSet(exitBranch, exitSuccessor, header);
    for (auto block : blocks) {
        if (block == nullptr || block == header) {
            continue;
        }
        for (auto expr : block->GetExpressions()) {
            if (expr->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(expr);
            if (store->GetLocation() == location) {
                return std::nullopt;
            }
        }
    }
    return 0;
}

std::optional<PairInductionLoop> GetPairInductionLoop(const Branch* branch, const Block* successor)
{
    if (!IsLoopExitSuccessor(branch, successor)) {
        return std::nullopt;
    }
    auto expr = GetDefiningExpr(branch->GetCondition());
    if (expr == nullptr || !IsRelationalExprKind(expr->GetExprKind())) {
        return std::nullopt;
    }
    auto rel = ToRelationalOperation(expr->GetExprKind());
    bool branchCondition = successor == branch->GetTrueBlock();
    auto loopRelation = branchCondition ? NegateRelation(rel) : rel;
    if (loopRelation != RelationalOperation::LT && loopRelation != RelationalOperation::LE &&
        loopRelation != RelationalOperation::GT && loopRelation != RelationalOperation::GE) {
        return std::nullopt;
    }

    auto binary = StaticCast<const BinaryExpression*>(expr);
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    auto lhsLocation = GetLoadLocation(lhs);
    auto rhsLocation = GetLoadLocation(rhs);
    auto lhsExpr = GetDefiningExpr(lhs);
    auto rhsExpr = GetDefiningExpr(rhs);
    auto header = branch->GetParentBlock();
    if (lhsLocation == nullptr || rhsLocation == nullptr || lhsLocation == rhsLocation ||
        lhsExpr == nullptr || rhsExpr == nullptr || header == nullptr ||
        !lhs->GetType()->IsInteger() || !rhs->GetType()->IsInteger() || lhs->GetType()->IsUnsignedInteger() ||
        rhs->GetType()->IsUnsignedInteger() || ToWidth(*lhs->GetType()) != ToWidth(*rhs->GetType())) {
        return std::nullopt;
    }

    auto lhsStep = FindSingleStepOrZeroOnLoopBackPath(branch, successor, header, lhsLocation);
    auto rhsStep = FindSingleStepOrZeroOnLoopBackPath(branch, successor, header, rhsLocation);
    if (!lhsStep.has_value() || !rhsStep.has_value()) {
        return std::nullopt;
    }
    auto relativeStep = static_cast<__int128>(lhsStep.value()) - static_cast<__int128>(rhsStep.value());
    if (((loopRelation == RelationalOperation::LT || loopRelation == RelationalOperation::LE) &&
            relativeStep <= 0) ||
        ((loopRelation == RelationalOperation::GT || loopRelation == RelationalOperation::GE) &&
            relativeStep >= 0)) {
        return std::nullopt;
    }
    return PairInductionLoop{
        lhs, lhsLocation, rhs, rhsLocation, header, branch, successor, lhsStep.value(), rhsStep.value(), loopRelation};
}

std::optional<std::vector<int64_t>> ValuesFromSmallSignedValueSet(
    const RangeDomain& state, Value* value, Type* type)
{
    if (type == nullptr) {
        return std::nullopt;
    }
    auto values = GetSmallSignedValuesFromState(state, value, type);
    if (values.has_value()) {
        return values;
    }
    if (value == nullptr || value->GetType() == nullptr) {
        return std::nullopt;
    }
    if (auto interval = GetSignedIntervalFromState(state, value); interval.has_value()) {
        auto count = static_cast<__int128>(interval->max) - static_cast<__int128>(interval->min) + 1;
        if (count > 0 && !ExceedsExactIntSetLimit(count)) {
            std::vector<int64_t> result;
            result.reserve(static_cast<size_t>(count));
            for (int64_t current = interval->min;; ++current) {
                if (!FitsSignedWidth(current, ToWidth(*type))) {
                    return std::nullopt;
                }
                result.emplace_back(current);
                if (current == interval->max) {
                    break;
                }
            }
            return result;
        }
    }
    return std::nullopt;
}

enum class SmallStoreLookupKind : uint8_t { NOT_FOUND, FOUND, UNKNOWN };

struct SmallStoreLookupResult {
    SmallStoreLookupKind kind{SmallStoreLookupKind::NOT_FOUND};
    std::vector<int64_t> values;
};

SmallStoreLookupResult MergeSmallStoreLookup(SmallStoreLookupResult lhs, SmallStoreLookupResult rhs)
{
    if (lhs.kind == SmallStoreLookupKind::UNKNOWN || rhs.kind == SmallStoreLookupKind::UNKNOWN) {
        return {SmallStoreLookupKind::UNKNOWN, {}};
    }
    if (lhs.kind == SmallStoreLookupKind::NOT_FOUND) {
        return rhs;
    }
    if (rhs.kind == SmallStoreLookupKind::NOT_FOUND) {
        return lhs;
    }
    lhs.values.insert(lhs.values.end(), rhs.values.begin(), rhs.values.end());
    std::sort(lhs.values.begin(), lhs.values.end());
    lhs.values.erase(std::unique(lhs.values.begin(), lhs.values.end()), lhs.values.end());
    if (lhs.values.empty() || ExceedsExactIntSetLimit(lhs.values.size())) {
        return {SmallStoreLookupKind::UNKNOWN, {}};
    }
    return lhs;
}

bool IsFunctionCallExpression(const Expression* expression)
{
    if (expression == nullptr) {
        return false;
    }
    switch (expression->GetExprKind()) {
        case ExprKind::APPLY:
        case ExprKind::INVOKE:
        case ExprKind::INVOKESTATIC:
        case ExprKind::APPLY_WITH_EXCEPTION:
        case ExprKind::INVOKE_WITH_EXCEPTION:
        case ExprKind::INVOKESTATIC_WITH_EXCEPTION:
            return true;
        default:
            return false;
    }
}

bool IsKnownPureStandardRangeIteratorCall(const Expression* expression)
{
    if (expression == nullptr || expression->GetExprKind() != ExprKind::APPLY) {
        return false;
    }
    auto apply = StaticCast<const Apply*>(expression);
    auto callee = DynamicCast<Function*>(apply->GetCallee());
    const auto& arguments = apply->GetArgs();
    if (callee == nullptr || callee->GetPackageName() != "std.core" ||
        callee->GetSrcCodeIdentifier() != "iterator" || arguments.size() != 1 ||
        arguments.front() == nullptr || arguments.front()->GetType() == nullptr) {
        return false;
    }
    auto receiverType = arguments.front()->GetType()->StripAllRefs();
    if (receiverType == nullptr || !receiverType->IsStruct()) {
        return false;
    }
    auto definition = StaticCast<StructType*>(receiverType)->GetStructDef();
    return definition != nullptr && definition->GetPackageName() == "std.core" &&
        definition->GetSrcCodeIdentifier() == "Range";
}

SmallStoreLookupResult FindLatestSmallSignedStoreValuesAvoidingBlocks(const RangeDomain& state, const Block* block,
    Value* location, Type* type, const std::unordered_set<const Block*>& blocked,
    std::unordered_set<const Block*>& visited, size_t depth)
{
    constexpr size_t MAX_BACKWARD_SMALL_STORE_LOOKUP_DEPTH = 32;
    if (block == nullptr || depth > MAX_BACKWARD_SMALL_STORE_LOOKUP_DEPTH) {
        return {SmallStoreLookupKind::UNKNOWN, {}};
    }
    if (blocked.find(block) != blocked.end() || !visited.emplace(block).second) {
        return {SmallStoreLookupKind::NOT_FOUND, {}};
    }
    auto exprs = block->GetExpressions();
    for (auto it = exprs.rbegin(); it != exprs.rend(); ++it) {
        if (location->IsGlobalVarWithInitializer() && IsFunctionCallExpression(*it) &&
            !IsKnownPureStandardRangeIteratorCall(*it)) {
            return {SmallStoreLookupKind::UNKNOWN, {}};
        }
        if ((*it)->GetExprKind() != ExprKind::STORE) {
            continue;
        }
        auto store = StaticCast<const Store*>(*it);
        if (store->GetLocation() != location) {
            continue;
        }
        auto values = ValuesFromSmallSignedValueSet(state, store->GetValue(), type);
        if (!values.has_value()) {
            return {SmallStoreLookupKind::UNKNOWN, {}};
        }
        std::sort(values->begin(), values->end());
        values->erase(std::unique(values->begin(), values->end()), values->end());
        if (values->empty() || ExceedsExactIntSetLimit(values->size())) {
            return {SmallStoreLookupKind::UNKNOWN, {}};
        }
        return {SmallStoreLookupKind::FOUND, std::move(values.value())};
    }

    SmallStoreLookupResult result{SmallStoreLookupKind::NOT_FOUND, {}};
    for (auto pred : block->GetPredecessors()) {
        result = MergeSmallStoreLookup(result,
            FindLatestSmallSignedStoreValuesAvoidingBlocks(
                state, pred, location, type, blocked, visited, depth + 1));
        if (result.kind == SmallStoreLookupKind::UNKNOWN) {
            return result;
        }
    }
    return result;
}

std::optional<std::vector<int64_t>> FindIncomingSmallSignedStoreValues(
    const RangeDomain& state, const Block* header, Value* location, Value* fallbackValue, Type* type,
    const std::unordered_set<const Block*>& loopBlocks,
    const std::optional<std::vector<int64_t>>& entryValues,
    const std::function<const RangeDomain*(const Block*)>& incomingEdgeState = {})
{
    std::vector<int64_t> values;
    bool sawIncoming = false;
    for (auto pred : header->GetPredecessors()) {
        if (IsBackedgePredecessor(header, pred)) {
            continue;
        }
        sawIncoming = true;
        if (incomingEdgeState) {
            auto edgeState = incomingEdgeState(pred);
            auto edgeValues = edgeState == nullptr
                ? std::nullopt
                : ValuesFromSmallSignedValueSet(*edgeState, fallbackValue, type);
            if (edgeValues.has_value()) {
                values.insert(values.end(), edgeValues->begin(), edgeValues->end());
                continue;
            }
        }
        std::unordered_set<const Block*> visited;
        auto lookup = FindLatestSmallSignedStoreValuesAvoidingBlocks(
            state, pred, location, type, loopBlocks, visited, 0);
        if (lookup.kind == SmallStoreLookupKind::UNKNOWN) {
            return std::nullopt;
        }
        if (lookup.kind == SmallStoreLookupKind::NOT_FOUND) {
            auto fallbackValues = entryValues.has_value()
                ? entryValues
                : ValuesFromSmallSignedValueSet(state, fallbackValue, type);
            if (!fallbackValues.has_value()) {
                return std::nullopt;
            }
            values.insert(values.end(), fallbackValues->begin(), fallbackValues->end());
            continue;
        }
        values.insert(values.end(), lookup.values.begin(), lookup.values.end());
    }
    if (!sawIncoming) {
        return entryValues.has_value()
            ? entryValues
            : ValuesFromSmallSignedValueSet(state, fallbackValue, type);
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    if (values.empty() || ExceedsExactIntSetLimit(values.size())) {
        return std::nullopt;
    }
    return values;
}

struct IntegerStoreLookupResult {
    SmallStoreLookupKind kind{SmallStoreLookupKind::NOT_FOUND};
    std::vector<SInt> values;
};

IntegerStoreLookupResult MergeIntegerStoreLookup(
    IntegerStoreLookupResult lhs, IntegerStoreLookupResult rhs)
{
    if (lhs.kind == SmallStoreLookupKind::UNKNOWN || rhs.kind == SmallStoreLookupKind::UNKNOWN) {
        return {SmallStoreLookupKind::UNKNOWN, {}};
    }
    if (lhs.kind == SmallStoreLookupKind::NOT_FOUND) {
        return rhs;
    }
    if (rhs.kind == SmallStoreLookupKind::NOT_FOUND) {
        return lhs;
    }
    lhs.values.insert(lhs.values.end(), rhs.values.begin(), rhs.values.end());
    auto normalized = NormalizeExactIntSet(std::move(lhs.values));
    if (!normalized.has_value()) {
        return {SmallStoreLookupKind::UNKNOWN, {}};
    }
    return {SmallStoreLookupKind::FOUND, std::move(normalized.value())};
}

std::optional<std::vector<SInt>> GetSmallIntegerValuesFromState(
    const RangeDomain& state, Value* value, Type* type)
{
    if (type == nullptr || !type->IsInteger()) {
        return std::nullopt;
    }
    auto values = GetExactValuesOrSmallRange(
        GetSIntRangeFromState(state, value, type), type->IsUnsignedInteger());
    if (!values.has_value() ||
        std::any_of(values->begin(), values->end(), [type](const SInt& item) {
            return item.Width() != ToWidth(*type);
        })) {
        return std::nullopt;
    }
    return values;
}

IntegerStoreLookupResult FindLatestSmallIntegerStoreValuesAvoidingBlocks(
    const RangeDomain& state, const Block* block, Value* location, Type* type,
    const std::unordered_set<const Block*>& blocked, std::unordered_set<const Block*>& visited, size_t depth)
{
    constexpr size_t MAX_BACKWARD_INTEGER_STORE_LOOKUP_DEPTH = 32;
    if (block == nullptr || depth > MAX_BACKWARD_INTEGER_STORE_LOOKUP_DEPTH) {
        return {SmallStoreLookupKind::UNKNOWN, {}};
    }
    if (blocked.find(block) != blocked.end() || !visited.emplace(block).second) {
        return {SmallStoreLookupKind::NOT_FOUND, {}};
    }
    auto expressions = block->GetExpressions();
    for (auto it = expressions.rbegin(); it != expressions.rend(); ++it) {
        if (location->IsGlobalVarWithInitializer() && IsFunctionCallExpression(*it) &&
            !IsKnownPureStandardRangeIteratorCall(*it)) {
            return {SmallStoreLookupKind::UNKNOWN, {}};
        }
        if ((*it)->GetExprKind() != ExprKind::STORE) {
            continue;
        }
        auto store = StaticCast<const Store*>(*it);
        if (store->GetLocation() != location) {
            continue;
        }
        auto values = GetSmallIntegerValuesFromState(state, store->GetValue(), type);
        return values.has_value()
            ? IntegerStoreLookupResult{SmallStoreLookupKind::FOUND, std::move(values.value())}
            : IntegerStoreLookupResult{SmallStoreLookupKind::UNKNOWN, {}};
    }

    IntegerStoreLookupResult result;
    for (auto predecessor : block->GetPredecessors()) {
        result = MergeIntegerStoreLookup(result,
            FindLatestSmallIntegerStoreValuesAvoidingBlocks(
                state, predecessor, location, type, blocked, visited, depth + 1));
        if (result.kind == SmallStoreLookupKind::UNKNOWN) {
            return result;
        }
    }
    return result;
}

std::optional<std::vector<SInt>> FindIncomingSmallIntegerStoreValues(
    const RangeDomain& state, const Block* entry, Value* location, Value* fallbackValue, Type* type,
    const std::unordered_set<const Block*>& loopBlocks,
    const std::optional<std::vector<SInt>>& entryValues,
    const std::function<const RangeDomain*(const Block*)>& incomingEdgeState = {})
{
    IntegerStoreLookupResult result;
    bool sawIncoming = false;
    for (auto predecessor : entry->GetPredecessors()) {
        if (loopBlocks.find(predecessor) != loopBlocks.end()) {
            continue;
        }
        sawIncoming = true;
        if (incomingEdgeState) {
            auto edgeState = incomingEdgeState(predecessor);
            Value* edgeValue = fallbackValue;
            if (edgeState != nullptr && location->GetType() != nullptr &&
                location->GetType()->IsRef()) {
                edgeValue = edgeState->CheckAbstractObjectRefBy(location);
            }
            auto edgeValues = edgeState == nullptr || edgeValue == nullptr
                ? std::nullopt
                : GetSmallIntegerValuesFromState(*edgeState, edgeValue, type);
            if (edgeValues.has_value()) {
                result = MergeIntegerStoreLookup(
                    std::move(result), {SmallStoreLookupKind::FOUND, std::move(edgeValues.value())});
                if (result.kind == SmallStoreLookupKind::UNKNOWN) {
                    return std::nullopt;
                }
                continue;
            }
        }
        std::unordered_set<const Block*> visited;
        auto lookup = FindLatestSmallIntegerStoreValuesAvoidingBlocks(
            state, predecessor, location, type, loopBlocks, visited, 0);
        if (lookup.kind == SmallStoreLookupKind::UNKNOWN) {
            return std::nullopt;
        }
        if (lookup.kind == SmallStoreLookupKind::NOT_FOUND) {
            auto fallback = entryValues.has_value()
                ? entryValues
                : GetSmallIntegerValuesFromState(state, fallbackValue, type);
            if (!fallback.has_value()) {
                return std::nullopt;
            }
            lookup = {SmallStoreLookupKind::FOUND, std::move(fallback.value())};
        }
        result = MergeIntegerStoreLookup(std::move(result), std::move(lookup));
        if (result.kind == SmallStoreLookupKind::UNKNOWN) {
            return std::nullopt;
        }
    }
    if (!sawIncoming) {
        return entryValues.has_value()
            ? entryValues
            : GetSmallIntegerValuesFromState(state, fallbackValue, type);
    }
    return result.kind == SmallStoreLookupKind::FOUND
        ? std::optional<std::vector<SInt>>{std::move(result.values)}
        : std::nullopt;
}

StoreLookupResult FindLatestBoolStoreValueAvoidingBlocks(const RangeDomain& state, const Block* block,
    Value* location, const std::unordered_set<const Block*>& blocked,
    std::unordered_set<const Block*>& visited, size_t depth)
{
    constexpr size_t MAX_BACKWARD_BOOL_STORE_LOOKUP_DEPTH = 32;
    if (block == nullptr || depth > MAX_BACKWARD_BOOL_STORE_LOOKUP_DEPTH) {
        return {StoreLookupKind::UNKNOWN, 0};
    }
    if (blocked.find(block) != blocked.end() || !visited.emplace(block).second) {
        return {StoreLookupKind::NOT_FOUND, 0};
    }
    auto expressions = block->GetExpressions();
    for (auto it = expressions.rbegin(); it != expressions.rend(); ++it) {
        if (location->IsGlobalVarWithInitializer() && IsFunctionCallExpression(*it) &&
            !IsKnownPureStandardRangeIteratorCall(*it)) {
            return {StoreLookupKind::UNKNOWN, 0};
        }
        if ((*it)->GetExprKind() != ExprKind::STORE) {
            continue;
        }
        auto store = StaticCast<const Store*>(*it);
        if (store->GetLocation() != location) {
            continue;
        }
        auto value = RangeAnalysis::GetBoolDomainFromState(state, store->GetValue());
        if (!value.IsSingleValue()) {
            return {StoreLookupKind::UNKNOWN, 0};
        }
        return {StoreLookupKind::FOUND, value.IsTrue() ? 1 : 0};
    }

    StoreLookupResult result{StoreLookupKind::NOT_FOUND, 0};
    for (auto predecessor : block->GetPredecessors()) {
        result = MergeStoreLookup(result, FindLatestBoolStoreValueAvoidingBlocks(
            state, predecessor, location, blocked, visited, depth + 1));
        if (result.kind == StoreLookupKind::UNKNOWN) {
            return result;
        }
    }
    return result;
}

std::optional<bool> FindIncomingBoolStoreValue(const RangeDomain& state, const Block* entry,
    Value* location, Value* fallbackValue, const std::unordered_set<const Block*>& loopBlocks,
    std::optional<bool> entryValue,
    const std::function<const RangeDomain*(const Block*)>& incomingEdgeState = {})
{
    StoreLookupResult result{StoreLookupKind::NOT_FOUND, 0};
    bool sawIncoming = false;
    for (auto predecessor : entry->GetPredecessors()) {
        if (loopBlocks.find(predecessor) != loopBlocks.end()) {
            continue;
        }
        sawIncoming = true;
        if (incomingEdgeState) {
            auto edgeState = incomingEdgeState(predecessor);
            if (edgeState != nullptr) {
                auto rootType = location->GetType()->IsRef()
                    ? StaticCast<RefType*>(location->GetType())->GetRootBaseType()
                    : location->GetType();
                Value* edgeObject = fallbackValue;
                if (location->GetType()->IsRef()) {
                    edgeObject = edgeState->CheckAbstractObjectRefBy(location);
                }
                auto edgeValue = edgeObject == nullptr
                    ? BoolDomain::Top()
                    : GetBoolDomainFromStateWithType(*edgeState, edgeObject, rootType);
                if (edgeValue.IsSingleValue()) {
                    result = MergeStoreLookup(result,
                        {StoreLookupKind::FOUND, edgeValue.IsTrue() ? 1 : 0});
                    continue;
                }
            }
        }
        std::unordered_set<const Block*> visited;
        result = MergeStoreLookup(result, FindLatestBoolStoreValueAvoidingBlocks(
            state, predecessor, location, loopBlocks, visited, 0));
        if (result.kind == StoreLookupKind::UNKNOWN) {
            return std::nullopt;
        }
    }
    if (result.kind == StoreLookupKind::FOUND) {
        return result.value != 0;
    }
    if (!sawIncoming || result.kind == StoreLookupKind::NOT_FOUND) {
        if (entryValue.has_value()) {
            return entryValue;
        }
        auto fallback = GetBoolDomainFromStateWithType(state, fallbackValue, location->GetType()->IsRef()
            ? StaticCast<RefType*>(location->GetType())->GetRootBaseType()
            : location->GetType());
        if (fallback.IsSingleValue()) {
            return fallback.IsTrue();
        }
    }
    return std::nullopt;
}

struct IntervalStoreLookupResult {
    SmallStoreLookupKind kind{SmallStoreLookupKind::NOT_FOUND};
    SignedInterval interval{0, 0};
};

IntervalStoreLookupResult MergeIntervalStoreLookup(
    IntervalStoreLookupResult lhs, IntervalStoreLookupResult rhs)
{
    if (lhs.kind == SmallStoreLookupKind::UNKNOWN || rhs.kind == SmallStoreLookupKind::UNKNOWN) {
        return {SmallStoreLookupKind::UNKNOWN, {0, 0}};
    }
    if (lhs.kind == SmallStoreLookupKind::NOT_FOUND) {
        return rhs;
    }
    if (rhs.kind == SmallStoreLookupKind::NOT_FOUND) {
        return lhs;
    }
    lhs.interval.min = std::min(lhs.interval.min, rhs.interval.min);
    lhs.interval.max = std::max(lhs.interval.max, rhs.interval.max);
    return lhs;
}

IntervalStoreLookupResult FindLatestSignedStoreIntervalAvoidingBlocks(const RangeDomain& state, const Block* block,
    Value* location, const std::unordered_set<const Block*>& blocked, std::unordered_set<const Block*>& visited,
    size_t depth)
{
    constexpr size_t MAX_BACKWARD_INTERVAL_STORE_LOOKUP_DEPTH = 32;
    if (block == nullptr || depth > MAX_BACKWARD_INTERVAL_STORE_LOOKUP_DEPTH) {
        return {SmallStoreLookupKind::UNKNOWN, {0, 0}};
    }
    if (blocked.find(block) != blocked.end() || !visited.emplace(block).second) {
        return {SmallStoreLookupKind::NOT_FOUND, {0, 0}};
    }
    auto exprs = block->GetExpressions();
    for (auto it = exprs.rbegin(); it != exprs.rend(); ++it) {
        if ((*it)->GetExprKind() != ExprKind::STORE) {
            continue;
        }
        auto store = StaticCast<const Store*>(*it);
        if (store->GetLocation() != location) {
            continue;
        }
        auto interval = GetSignedIntervalFromState(state, store->GetValue());
        return interval.has_value()
            ? IntervalStoreLookupResult{SmallStoreLookupKind::FOUND, interval.value()}
            : IntervalStoreLookupResult{SmallStoreLookupKind::UNKNOWN, {0, 0}};
    }

    IntervalStoreLookupResult result;
    for (auto pred : block->GetPredecessors()) {
        result = MergeIntervalStoreLookup(result,
            FindLatestSignedStoreIntervalAvoidingBlocks(
                state, pred, location, blocked, visited, depth + 1));
        if (result.kind == SmallStoreLookupKind::UNKNOWN) {
            return result;
        }
    }
    return result;
}

std::optional<SignedInterval> FindIncomingSignedStoreInterval(const RangeDomain& state, const Block* header,
    Value* location, Value* fallbackValue, const std::unordered_set<const Block*>& loopBlocks)
{
    IntervalStoreLookupResult result;
    bool sawIncoming = false;
    for (auto pred : header->GetPredecessors()) {
        if (IsBackedgePredecessor(header, pred)) {
            continue;
        }
        sawIncoming = true;
        std::unordered_set<const Block*> visited;
        auto lookup = FindLatestSignedStoreIntervalAvoidingBlocks(
            state, pred, location, loopBlocks, visited, 0);
        if (lookup.kind == SmallStoreLookupKind::NOT_FOUND) {
            auto fallback = GetSignedIntervalFromState(state, fallbackValue);
            if (!fallback.has_value()) {
                return std::nullopt;
            }
            lookup = {SmallStoreLookupKind::FOUND, fallback.value()};
        }
        result = MergeIntervalStoreLookup(result, lookup);
        if (result.kind == SmallStoreLookupKind::UNKNOWN) {
            return std::nullopt;
        }
    }
    if (!sawIncoming) {
        return GetSignedIntervalFromState(state, fallbackValue);
    }
    return result.kind == SmallStoreLookupKind::FOUND
        ? std::optional<SignedInterval>{result.interval}
        : std::nullopt;
}

std::optional<SignedInterval> FindMustStoreIntervalOnLoopPath(const RangeDomain& state, const Block* block,
    const Block* header, Value* location, const std::unordered_set<const Block*>& loopBlocks,
    std::unordered_set<const Block*> path, size_t depth)
{
    constexpr size_t MAX_MUST_STORE_SEARCH_DEPTH = 64;
    if (block == nullptr || block == header || depth > MAX_MUST_STORE_SEARCH_DEPTH ||
        loopBlocks.find(block) == loopBlocks.end() || !path.emplace(block).second) {
        return std::nullopt;
    }
    auto expressions = block->GetExpressions();
    for (auto it = expressions.rbegin(); it != expressions.rend(); ++it) {
        if ((*it)->GetExprKind() != ExprKind::STORE) {
            continue;
        }
        auto store = StaticCast<const Store*>(*it);
        if (store->GetLocation() == location) {
            return GetSignedIntervalFromState(state, store->GetValue());
        }
    }

    bool sawPredecessor = false;
    std::optional<SignedInterval> merged;
    for (auto predecessor : block->GetPredecessors()) {
        if (predecessor != header && loopBlocks.find(predecessor) == loopBlocks.end()) {
            continue;
        }
        sawPredecessor = true;
        auto incoming = FindMustStoreIntervalOnLoopPath(
            state, predecessor, header, location, loopBlocks, path, depth + 1);
        if (!incoming.has_value()) {
            return std::nullopt;
        }
        if (!merged.has_value()) {
            merged = incoming;
        } else {
            merged->min = std::min(merged->min, incoming->min);
            merged->max = std::max(merged->max, incoming->max);
        }
    }
    return sawPredecessor ? merged : std::nullopt;
}

bool HasOnlyDirectLoadStoreUsers(Value* location)
{
    if (location == nullptr) {
        return false;
    }
    for (auto user : location->GetUsers()) {
        if (user->GetExprKind() == ExprKind::DEBUGEXPR) {
            continue;
        }
        if (user->GetExprKind() == ExprKind::LOAD && StaticCast<const Load*>(user)->GetLocation() == location) {
            continue;
        }
        if (user->GetExprKind() == ExprKind::STORE && StaticCast<const Store*>(user)->GetLocation() == location) {
            continue;
        }
        return false;
    }
    return true;
}

bool ContainsStoreToLocation(const Block* block, Value* location)
{
    if (block == nullptr || location == nullptr) {
        return false;
    }
    auto expressions = block->GetExpressions();
    return std::any_of(expressions.begin(), expressions.end(), [location](auto expression) {
        return expression->GetExprKind() == ExprKind::STORE &&
            StaticCast<const Store*>(expression)->GetLocation() == location;
    });
}

bool HasStoreOutsideNestedLoop(Value* location, const std::unordered_set<const Block*>& outerLoopBlocks,
    const std::unordered_set<const Block*>& nestedLoopBlocks)
{
    for (auto block : outerLoopBlocks) {
        if (nestedLoopBlocks.find(block) == nestedLoopBlocks.end() && ContainsStoreToLocation(block, location)) {
            return true;
        }
    }
    return false;
}

struct ForcedFirstLoopEntry {
    const Block* header;
    const Block* bodySuccessor;
};

std::vector<ForcedFirstLoopEntry> CollectForcedFirstLoopEntries(const Branch* outerBranch,
    const Block* outerExit, const std::unordered_set<const Block*>& outerLoopBlocks)
{
    constexpr size_t MAX_TRACKED_NESTED_LOOP_HEADERS = 63;
    std::vector<ForcedFirstLoopEntry> entries;
    for (auto block : outerLoopBlocks) {
        if (block == outerBranch->GetParentBlock() || entries.size() >= MAX_TRACKED_NESTED_LOOP_HEADERS) {
            continue;
        }
        auto terminator = block->GetTerminator();
        if (terminator == nullptr || terminator->GetExprKind() != ExprKind::BRANCH) {
            continue;
        }
        auto branch = StaticCast<const Branch*>(terminator);
        const Block* nestedExit = nullptr;
        const Block* nestedBody = nullptr;
        for (auto successor : {branch->GetTrueBlock(), branch->GetFalseBlock()}) {
            if (IsLoopExitSuccessor(branch, successor)) {
                nestedExit = successor;
            } else {
                nestedBody = successor;
            }
        }
        if (nestedExit == nullptr || nestedBody == nullptr || nestedExit == nestedBody || nestedExit == outerExit) {
            continue;
        }
        auto condition = GetSimpleInductionCondition(branch->GetCondition());
        if (!condition.has_value()) {
            continue;
        }
        auto initial = FindIncomingSignedStoreConstantBeforeLoop(
            outerBranch->GetParentBlock(), condition->location, outerLoopBlocks);
        if (!initial.has_value() || !SatisfiesSignedRelation(
                static_cast<__int128>(initial.value()), condition->relation,
                static_cast<__int128>(condition->bound))) {
            continue;
        }
        auto nestedLoopBlocks = CollectLoopBackPathBlockSet(branch, nestedExit, block);
        if (HasStoreOutsideNestedLoop(condition->location, outerLoopBlocks, nestedLoopBlocks)) {
            continue;
        }
        entries.emplace_back(ForcedFirstLoopEntry{block, nestedBody});
    }
    return entries;
}

struct FirstIterationPathState {
    const Block* block;
    uint64_t enteredLoopHeaders;

    bool operator==(const FirstIterationPathState& rhs) const
    {
        return block == rhs.block && enteredLoopHeaders == rhs.enteredLoopHeaders;
    }
};

struct FirstIterationPathStateHash {
    size_t operator()(const FirstIterationPathState& state) const
    {
        return std::hash<const Block*>{}(state.block) ^
            (std::hash<uint64_t>{}(state.enteredLoopHeaders) << 1U);
    }
};

bool HasTargetFreeFirstIterationPath(const Branch* outerBranch, const Block* outerExit, Value* location,
    const std::unordered_set<const Block*>& outerLoopBlocks)
{
    constexpr size_t MAX_FIRST_ITERATION_PATH_STATES = 4096;
    auto bodyEntry = outerBranch->GetTrueBlock() == outerExit
        ? outerBranch->GetFalseBlock()
        : outerBranch->GetTrueBlock();
    if (bodyEntry == nullptr) {
        return true;
    }
    auto forcedEntries = CollectForcedFirstLoopEntries(outerBranch, outerExit, outerLoopBlocks);
    std::unordered_map<const Block*, std::pair<size_t, const Block*>> forcedByHeader;
    for (size_t index = 0; index < forcedEntries.size(); ++index) {
        forcedByHeader.emplace(forcedEntries[index].header,
            std::make_pair(index, forcedEntries[index].bodySuccessor));
    }

    std::vector<FirstIterationPathState> worklist{{bodyEntry, 0}};
    std::unordered_set<FirstIterationPathState, FirstIterationPathStateHash> visited;
    for (size_t index = 0; index < worklist.size(); ++index) {
        if (worklist.size() > MAX_FIRST_ITERATION_PATH_STATES) {
            return true;
        }
        auto current = worklist[index];
        if (!visited.emplace(current).second || ContainsStoreToLocation(current.block, location)) {
            continue;
        }
        if (current.block == outerBranch->GetParentBlock() || current.block == outerExit ||
            outerLoopBlocks.find(current.block) == outerLoopBlocks.end()) {
            return true;
        }

        auto forced = forcedByHeader.find(current.block);
        if (forced != forcedByHeader.end()) {
            auto bit = uint64_t{1} << forced->second.first;
            if ((current.enteredLoopHeaders & bit) == 0) {
                worklist.emplace_back(
                    FirstIterationPathState{forced->second.second, current.enteredLoopHeaders | bit});
                continue;
            }
        }
        for (auto successor : current.block->GetSuccessors()) {
            worklist.emplace_back(FirstIterationPathState{successor, current.enteredLoopHeaders});
        }
    }
    return false;
}

std::optional<SignedInterval> CollectLoopStoreInterval(const RangeDomain& state,
    const std::unordered_set<const Block*>& loopBlocks, Value* location)
{
    std::optional<SignedInterval> result;
    bool sawStore = false;
    for (auto block : loopBlocks) {
        for (auto expression : block->GetExpressions()) {
            if (expression->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(expression);
            if (store->GetLocation() != location) {
                continue;
            }
            sawStore = true;
            auto interval = GetSignedIntervalFromState(state, store->GetValue());
            if (!interval.has_value()) {
                return std::nullopt;
            }
            if (!result.has_value()) {
                result = interval;
            } else {
                result->min = std::min(result->min, interval->min);
                result->max = std::max(result->max, interval->max);
            }
        }
    }
    return sawStore ? result : std::nullopt;
}

void NarrowIntegerObjectRange(RangeDomain& state, Value* object, Type* type, const SIntRange& constraint)
{
    if (object == nullptr || type == nullptr || !type->IsInteger()) {
        return;
    }
    const auto& current = GetSIntDomainFromState(state, object, type);
    auto intersection = IntersectForNarrowing(current, constraint.GetVal());
    if (intersection.IsBottom()) {
        state.Update(object,
            std::make_unique<SIntRange>(SIntDomain::Bottom(ToWidth(*type), type->IsUnsignedInteger())));
        return;
    }
    state.Update(object, std::make_unique<SIntRange>(std::move(intersection)));
}

bool TryNarrowMustExecuteLoopCarriedValues(
    RangeDomain& state, const Branch* branch, const Block* successor)
{
    if (!IsLoopExitSuccessor(branch, successor)) {
        return true;
    }
    auto condition = GetSimpleInductionCondition(branch->GetCondition());
    if (!condition.has_value()) {
        return true;
    }
    auto initial = FindIncomingSignedStoreConstant(branch->GetParentBlock(), condition->location);
    if (!initial.has_value() || !SatisfiesSignedRelation(
            static_cast<__int128>(initial.value()), condition->relation,
            static_cast<__int128>(condition->bound))) {
        return true;
    }

    auto header = branch->GetParentBlock();
    auto loopBlocks = CollectLoopBackPathBlockSet(branch, successor, header);
    std::unordered_set<Value*> candidateLocations;
    for (auto block : loopBlocks) {
        for (auto expression : block->GetExpressions()) {
            if (expression->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto location = StaticCast<const Store*>(expression)->GetLocation();
            if (location != condition->location && GetIntegerRefRootType(location) != nullptr) {
                candidateLocations.emplace(location);
            }
        }
    }

    for (auto location : candidateLocations) {
        std::optional<SignedInterval> merged;
        bool sawBackedge = false;
        bool hasMustStoreOnEveryBackedge = true;
        for (auto predecessor : header->GetPredecessors()) {
            if (!IsBackedgePredecessor(header, predecessor)) {
                continue;
            }
            sawBackedge = true;
            auto interval = FindMustStoreIntervalOnLoopPath(
                state, predecessor, header, location, loopBlocks, {}, 0);
            if (!interval.has_value()) {
                hasMustStoreOnEveryBackedge = false;
                break;
            }
            if (!merged.has_value()) {
                merged = interval;
            } else {
                merged->min = std::min(merged->min, interval->min);
                merged->max = std::max(merged->max, interval->max);
            }
        }
        auto rootType = GetIntegerRefRootType(location);
        auto object = state.CheckAbstractObjectRefBy(location);
        if (rootType == nullptr || object == nullptr) {
            continue;
        }
        if (sawBackedge && hasMustStoreOnEveryBackedge && merged.has_value()) {
            auto range = BuildSignedIntervalRange(rootType, merged.value());
            if (range.has_value()) {
                NarrowIntegerObjectRange(state, object, rootType, range.value());
            }
            continue;
        }
        if (!HasOnlyDirectLoadStoreUsers(location) ||
            HasTargetFreeFirstIterationPath(branch, successor, location, loopBlocks)) {
            continue;
        }
        auto firstAndLaterStores = CollectLoopStoreInterval(state, loopBlocks, location);
        if (!firstAndLaterStores.has_value() || firstAndLaterStores->min != firstAndLaterStores->max) {
            continue;
        }
        auto range = BuildSignedIntervalRange(rootType, firstAndLaterStores.value());
        if (range.has_value()) {
            NarrowIntegerObjectRange(state, object, rootType, range.value());
        }
    }
    return true;
}

struct PairLoopIntervals {
    SignedInterval lhsBody;
    SignedInterval rhsBody;
    SignedInterval lhsExit;
    SignedInterval rhsExit;
    bool hasBodyValues{false};
};

std::optional<__int128> ComputePairLoopTripCount(
    int64_t lhs, int64_t rhs, const PairInductionLoop& loop)
{
    auto relativeStep = static_cast<__int128>(loop.lhsStep) - static_cast<__int128>(loop.rhsStep);
    switch (loop.relation) {
        case RelationalOperation::LT: {
            if (lhs >= rhs) {
                return 0;
            }
            return CeilDivPositive(static_cast<__int128>(rhs) - lhs, relativeStep);
        }
        case RelationalOperation::LE: {
            if (lhs > rhs) {
                return 0;
            }
            return (static_cast<__int128>(rhs) - lhs) / relativeStep + 1;
        }
        case RelationalOperation::GT: {
            if (lhs <= rhs) {
                return 0;
            }
            return CeilDivPositive(static_cast<__int128>(lhs) - rhs, -relativeStep);
        }
        case RelationalOperation::GE: {
            if (lhs < rhs) {
                return 0;
            }
            return (static_cast<__int128>(lhs) - rhs) / (-relativeStep) + 1;
        }
        case RelationalOperation::EQ:
        case RelationalOperation::NE:
            return std::nullopt;
    }
    return std::nullopt;
}

void ExtendSignedInterval(SignedInterval& interval, bool& initialized, int64_t first, int64_t last)
{
    auto lower = std::min(first, last);
    auto upper = std::max(first, last);
    if (!initialized) {
        interval = {lower, upper};
        initialized = true;
        return;
    }
    interval.min = std::min(interval.min, lower);
    interval.max = std::max(interval.max, upper);
}

std::optional<PairLoopIntervals> ComputePairLoopIntervals(const RangeDomain& state, const PairInductionLoop& loop)
{
    constexpr __int128 MAX_PAIR_INITIAL_COMBINATIONS = 65536;
    auto loopBlocks = CollectLoopBackPathBlockSet(loop.branch, loop.exitSuccessor, loop.header);
    auto lhsInit = FindIncomingSignedStoreInterval(
        state, loop.header, loop.lhsLocation, loop.lhsValue, loopBlocks);
    auto rhsInit = FindIncomingSignedStoreInterval(
        state, loop.header, loop.rhsLocation, loop.rhsValue, loopBlocks);
    if (!lhsInit.has_value() || !rhsInit.has_value()) {
        return std::nullopt;
    }
    auto lhsCount = static_cast<__int128>(lhsInit->max) - lhsInit->min + 1;
    auto rhsCount = static_cast<__int128>(rhsInit->max) - rhsInit->min + 1;
    if (lhsCount <= 0 || rhsCount <= 0 || lhsCount * rhsCount > MAX_PAIR_INITIAL_COMBINATIONS) {
        return std::nullopt;
    }

    PairLoopIntervals result{{0, 0}, {0, 0}, {0, 0}, {0, 0}, false};
    bool lhsExitInitialized = false;
    bool rhsExitInitialized = false;
    bool lhsBodyInitialized = false;
    bool rhsBodyInitialized = false;
    auto width = ToWidth(*loop.lhsValue->GetType());
    for (int64_t lhs = lhsInit->min;; ++lhs) {
        for (int64_t rhs = rhsInit->min;; ++rhs) {
            auto tripCount = ComputePairLoopTripCount(lhs, rhs, loop);
            if (!tripCount.has_value()) {
                return std::nullopt;
            }
            auto lhsExit = static_cast<__int128>(lhs) + *tripCount * loop.lhsStep;
            auto rhsExit = static_cast<__int128>(rhs) + *tripCount * loop.rhsStep;
            if (!FitsSignedWidth(lhsExit, width) || !FitsSignedWidth(rhsExit, width)) {
                return std::nullopt;
            }
            ExtendSignedInterval(result.lhsExit, lhsExitInitialized,
                static_cast<int64_t>(lhsExit), static_cast<int64_t>(lhsExit));
            ExtendSignedInterval(result.rhsExit, rhsExitInitialized,
                static_cast<int64_t>(rhsExit), static_cast<int64_t>(rhsExit));
            if (*tripCount > 0) {
                auto lhsLast = static_cast<__int128>(lhs) + (*tripCount - 1) * loop.lhsStep;
                auto rhsLast = static_cast<__int128>(rhs) + (*tripCount - 1) * loop.rhsStep;
                if (!FitsSignedWidth(lhsLast, width) || !FitsSignedWidth(rhsLast, width)) {
                    return std::nullopt;
                }
                ExtendSignedInterval(result.lhsBody, lhsBodyInitialized, lhs, static_cast<int64_t>(lhsLast));
                ExtendSignedInterval(result.rhsBody, rhsBodyInitialized, rhs, static_cast<int64_t>(rhsLast));
            }
            if (rhs == rhsInit->max) {
                break;
            }
        }
        if (lhs == lhsInit->max) {
            break;
        }
    }
    result.hasBodyValues = lhsBodyInitialized && rhsBodyInitialized;
    return lhsExitInitialized && rhsExitInitialized
        ? std::optional<PairLoopIntervals>{result}
        : std::nullopt;
}

std::optional<PairLoopValues> EnumeratePairLoopValues(const RangeDomain& state, const PairInductionLoop& loop)
{
    auto loopBlocks = CollectLoopBackPathBlockSet(loop.branch, loop.exitSuccessor, loop.header);
    auto lhsInits = FindIncomingSmallSignedStoreValues(
        state, loop.header, loop.lhsLocation, loop.lhsValue, loop.lhsValue->GetType(), loopBlocks, std::nullopt);
    auto rhsInits = FindIncomingSmallSignedStoreValues(
        state, loop.header, loop.rhsLocation, loop.rhsValue, loop.rhsValue->GetType(), loopBlocks, std::nullopt);
    if (!lhsInits.has_value() || !rhsInits.has_value()) {
        return std::nullopt;
    }
    PairLoopValues result;
    auto width = ToWidth(*loop.lhsValue->GetType());
    for (auto lhsInit : *lhsInits) {
        for (auto rhsInit : *rhsInits) {
            __int128 lhs = lhsInit;
            __int128 rhs = rhsInit;
            size_t iteration = 0;
            while (SatisfiesSignedRelation(lhs, loop.relation, rhs)) {
                if (ReachedExactIntSetLimit(iteration++) ||
                    !FitsSignedWidth(lhs, width) ||
                    !FitsSignedWidth(rhs, width)) {
                    return std::nullopt;
                }
                result.lhsBodyValues.emplace_back(static_cast<int64_t>(lhs));
                result.rhsBodyValues.emplace_back(static_cast<int64_t>(rhs));
                lhs += loop.lhsStep;
                rhs += loop.rhsStep;
            }
            if (!FitsSignedWidth(lhs, width) || !FitsSignedWidth(rhs, width)) {
                return std::nullopt;
            }
            result.lhsExitValues.emplace_back(static_cast<int64_t>(lhs));
            result.rhsExitValues.emplace_back(static_cast<int64_t>(rhs));
        }
    }
    return result;
}

bool TryNarrowPairInductionExit(RangeDomain& state, const Branch* branch, const Block* successor)
{
    auto loop = GetPairInductionLoop(branch, successor);
    if (!loop.has_value()) {
        return true;
    }
    auto values = EnumeratePairLoopValues(state, loop.value());
    auto intervals = values.has_value() ? std::nullopt : ComputePairLoopIntervals(state, loop.value());
    auto lhsRange = values.has_value()
        ? BuildSignedExactRange(loop->lhsValue->GetType(), values->lhsExitValues)
        : intervals.has_value()
        ? BuildSignedIntervalRange(loop->lhsValue->GetType(), intervals->lhsExit)
        : std::nullopt;
    auto rhsRange = values.has_value()
        ? BuildSignedExactRange(loop->rhsValue->GetType(), values->rhsExitValues)
        : intervals.has_value()
        ? BuildSignedIntervalRange(loop->rhsValue->GetType(), intervals->rhsExit)
        : std::nullopt;
    if (lhsRange.has_value()) {
        if (auto object = state.CheckAbstractObjectRefBy(loop->lhsLocation); object != nullptr) {
            state.Update(object, std::make_unique<SIntRange>(std::move(lhsRange.value())));
        }
    }
    if (rhsRange.has_value()) {
        if (auto object = state.CheckAbstractObjectRefBy(loop->rhsLocation); object != nullptr) {
            state.Update(object, std::make_unique<SIntRange>(std::move(rhsRange.value())));
        }
    }
    return true;
}

bool CanComputePairInductionExitFromState(const RangeDomain& state, const Branch* branch, const Block* successor)
{
    auto loop = GetPairInductionLoop(branch, successor);
    return loop.has_value() && (EnumeratePairLoopValues(state, loop.value()).has_value() ||
        ComputePairLoopIntervals(state, loop.value()).has_value());
}

struct PairLoopContext {
    PairInductionLoop loop;
    std::unordered_set<const Block*> loopBlocks;
};

std::optional<PairLoopContext> FindPairLoopContext(const RangeDomain& state, const Block* loadBlock)
{
    (void)state;
    constexpr size_t MAX_LOOP_CONTEXT_SEARCH_BLOCKS = 256;
    std::vector<const Block*> worklist{loadBlock};
    std::unordered_set<const Block*> visited;
    for (size_t index = 0; index < worklist.size() && index < MAX_LOOP_CONTEXT_SEARCH_BLOCKS; ++index) {
        auto block = worklist[index];
        if (block == nullptr || !visited.emplace(block).second) {
            continue;
        }
        auto terminator = block->GetTerminator();
        if (terminator != nullptr && terminator->GetExprKind() == ExprKind::BRANCH) {
            auto branch = StaticCast<const Branch*>(terminator);
            for (auto successor : {branch->GetTrueBlock(), branch->GetFalseBlock()}) {
                auto loop = GetPairInductionLoop(branch, successor);
                if (!loop.has_value()) {
                    continue;
                }
                auto loopBlocks = CollectLoopBackPathBlockSet(branch, successor, loop->header);
                if (!IsLoopBodyNestedBlock(loadBlock, loop->header, loopBlocks)) {
                    continue;
                }
                return PairLoopContext{loop.value(), std::move(loopBlocks)};
            }
        }
        for (auto successor : block->GetSuccessors()) {
            worklist.emplace_back(successor);
        }
        for (auto pred : block->GetPredecessors()) {
            worklist.emplace_back(pred);
        }
    }
    return std::nullopt;
}

std::optional<SIntRange> TryComputePairLoopLoadRange(const RangeDomain& state, const Load* load)
{
    if (load == nullptr || load->GetLocation() == nullptr || load->GetParentBlock() == nullptr ||
        !load->GetResult()->GetType()->IsInteger() || load->GetResult()->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }
    auto context = FindPairLoopContext(state, load->GetParentBlock());
    if (!context.has_value()) {
        return std::nullopt;
    }
    auto values = EnumeratePairLoopValues(state, context->loop);
    if (values.has_value()) {
        if (load->GetLocation() == context->loop.lhsLocation) {
            return BuildSignedExactRange(load->GetResult()->GetType(), values->lhsBodyValues);
        }
        if (load->GetLocation() == context->loop.rhsLocation) {
            return BuildSignedExactRange(load->GetResult()->GetType(), values->rhsBodyValues);
        }
        return std::nullopt;
    }
    auto intervals = ComputePairLoopIntervals(state, context->loop);
    if (!intervals.has_value() || !intervals->hasBodyValues) {
        return std::nullopt;
    }
    if (load->GetLocation() == context->loop.lhsLocation) {
        return BuildSignedIntervalRange(load->GetResult()->GetType(), intervals->lhsBody);
    }
    if (load->GetLocation() == context->loop.rhsLocation) {
        return BuildSignedIntervalRange(load->GetResult()->GetType(), intervals->rhsBody);
    }
    return std::nullopt;
}

std::optional<CountedAccumulatorLoopContext> FindCountedAccumulatorLoopContext(
    const RangeDomain& state, const Block* updateBlock)
{
    constexpr size_t MAX_LOOP_CONTEXT_SEARCH_BLOCKS = 256;
    std::vector<const Block*> worklist{updateBlock};
    std::unordered_set<const Block*> visited;
    for (size_t index = 0; index < worklist.size() && index < MAX_LOOP_CONTEXT_SEARCH_BLOCKS; ++index) {
        auto block = worklist[index];
        if (block == nullptr || !visited.emplace(block).second) {
            continue;
        }
        auto terminator = block->GetTerminator();
        if (terminator != nullptr && terminator->GetExprKind() == ExprKind::BRANCH) {
            auto branch = StaticCast<const Branch*>(terminator);
            for (auto successor : {branch->GetTrueBlock(), branch->GetFalseBlock()}) {
                auto induction = GetVariableBoundInductionExit(state, branch, successor);
                if (!induction.has_value()) {
                    continue;
                }
                auto loopBlocks = CollectLoopBackPathBlockSet(branch, successor, induction->header);
                if (!IsLoopBodyNestedBlock(updateBlock, induction->header, loopBlocks)) {
                    continue;
                }
                return CountedAccumulatorLoopContext{induction.value(), branch, successor, std::move(loopBlocks)};
            }
        }
        for (auto successor : block->GetSuccessors()) {
            worklist.emplace_back(successor);
        }
        for (auto pred : block->GetPredecessors()) {
            worklist.emplace_back(pred);
        }
    }
    return std::nullopt;
}

const Load* GetRootIntegerLoad(Value* value)
{
    constexpr size_t MAX_TRANSPARENT_CAST_DEPTH = 4;
    for (size_t depth = 0; value != nullptr && depth <= MAX_TRANSPARENT_CAST_DEPTH; ++depth) {
        auto expression = GetDefiningExpr(value);
        if (expression == nullptr) {
            return nullptr;
        }
        if (expression->GetExprKind() == ExprKind::LOAD) {
            return StaticCast<const Load*>(expression);
        }
        if (expression->GetExprKind() != ExprKind::TYPECAST) {
            return nullptr;
        }
        auto cast = StaticCast<const TypeCast*>(expression);
        auto source = cast->GetSourceValue();
        if (source == nullptr || !source->GetType()->IsInteger() || !value->GetType()->IsInteger() ||
            source->GetType()->IsUnsignedInteger() != value->GetType()->IsUnsignedInteger() ||
            ToWidth(*source->GetType()) != ToWidth(*value->GetType())) {
            return nullptr;
        }
        value = source;
    }
    return nullptr;
}

bool HasStoreToEitherLocationBeforeExpression(
    const Expression* target, Value* lhsLocation, Value* rhsLocation)
{
    if (target == nullptr || target->GetParentBlock() == nullptr) {
        return true;
    }
    for (auto expression : target->GetParentBlock()->GetExpressions()) {
        if (expression == target) {
            return false;
        }
        if (expression->GetExprKind() != ExprKind::STORE) {
            continue;
        }
        auto location = StaticCast<const Store*>(expression)->GetLocation();
        if (location == lhsLocation || location == rhsLocation) {
            return true;
        }
    }
    return true;
}

std::optional<SIntRange> TryComputeLockstepDifferenceRangeImpl(
    const RangeDomain& state, const BinaryExpression* binaryExpr)
{
    if (binaryExpr == nullptr || binaryExpr->GetExprKind() != ExprKind::SUB ||
        binaryExpr->GetResult() == nullptr || !binaryExpr->GetResult()->GetType()->IsInteger() ||
        binaryExpr->GetResult()->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }
    auto lhsLoad = GetRootIntegerLoad(binaryExpr->GetLHSOperand());
    auto rhsLoad = GetRootIntegerLoad(binaryExpr->GetRHSOperand());
    if (lhsLoad == nullptr || rhsLoad == nullptr ||
        lhsLoad->GetParentBlock() != binaryExpr->GetParentBlock() ||
        rhsLoad->GetParentBlock() != binaryExpr->GetParentBlock()) {
        return std::nullopt;
    }
    auto lhsLocation = lhsLoad->GetLocation();
    auto rhsLocation = rhsLoad->GetLocation();
    if (lhsLocation == nullptr || rhsLocation == nullptr || lhsLocation == rhsLocation ||
        HasStoreToEitherLocationBeforeExpression(binaryExpr, lhsLocation, rhsLocation)) {
        return std::nullopt;
    }

    auto context = FindCountedAccumulatorLoopContext(state, binaryExpr->GetParentBlock());
    if (!context.has_value()) {
        return std::nullopt;
    }
    auto bodySuccessor = context->branch->GetTrueBlock() == context->exitSuccessor
        ? context->branch->GetFalseBlock()
        : context->branch->GetTrueBlock();
    if (binaryExpr->GetParentBlock() != bodySuccessor) {
        return std::nullopt;
    }

    Value* candidateLocation = nullptr;
    bool candidateOnLhs = false;
    if (rhsLocation == context->induction.location) {
        candidateLocation = lhsLocation;
        candidateOnLhs = true;
    } else if (lhsLocation == context->induction.location) {
        candidateLocation = rhsLocation;
    } else {
        return std::nullopt;
    }
    auto proof = ProveLockstepInductionRelation(context->induction.header,
        context->induction.location, candidateLocation, context->loopBlocks);
    if (!proof.has_value()) {
        return std::nullopt;
    }
    auto difference = candidateOnLhs
        ? std::optional<int64_t>{proof->offset}
        : NegateSignedStep(proof->offset);
    if (!difference.has_value() ||
        !FitsSignedWidth(difference.value(), ToWidth(*binaryExpr->GetResult()->GetType()))) {
        return std::nullopt;
    }
    return BuildSignedExactRange(binaryExpr->GetResult()->GetType(), {difference.value()});
}

std::optional<SIntRange> TryComputeCommonSymbolDifferenceRangeImpl(
    const RangeDomain& state, const BinaryExpression* binaryExpr)
{
    if (binaryExpr == nullptr || binaryExpr->GetExprKind() != ExprKind::SUB ||
        binaryExpr->GetOverflowStrategy() != OverflowStrategy::THROWING ||
        binaryExpr->GetResult() == nullptr || binaryExpr->GetResult()->GetType() == nullptr ||
        !binaryExpr->GetResult()->GetType()->IsInteger() ||
        binaryExpr->GetResult()->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }
    auto lhs = binaryExpr->GetLHSOperand();
    auto rhs = binaryExpr->GetRHSOperand();
    if (lhs == nullptr || rhs == nullptr || lhs->GetType() == nullptr || rhs->GetType() == nullptr ||
        !lhs->GetType()->IsInteger() || !rhs->GetType()->IsInteger() ||
        lhs->GetType()->IsUnsignedInteger() || rhs->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }
    auto resultWidth = ToWidth(*binaryExpr->GetResult()->GetType());
    if (ToWidth(*lhs->GetType()) != resultWidth || ToWidth(*rhs->GetType()) != resultWidth) {
        return std::nullopt;
    }

    constexpr size_t MAX_SYMBOLIC_ANCESTOR_DEPTH = 6;
    constexpr size_t MAX_SYMBOLIC_ANCESTORS = 32;
    using AncestorOffsets = std::map<Value*, int64_t>;
    struct PendingAncestor {
        Value* value;
        int64_t offset;
        size_t depth;
    };
    auto collectAncestors = [&](Value* start) -> std::optional<AncestorOffsets> {
        AncestorOffsets offsets{{start, 0}};
        std::vector<PendingAncestor> pending{{start, 0, 0}};
        for (size_t index = 0; index < pending.size(); ++index) {
            auto current = pending[index];
            if (current.depth >= MAX_SYMBOLIC_ANCESTOR_DEPTH) {
                continue;
            }
            const auto& domain = RangeAnalysis::GetSIntDomainFromState(state, current.value);
            for (auto symbolic = domain.SymbolicBounds().Begin();
                symbolic != domain.SymbolicBounds().End(); ++symbolic) {
                auto ancestor = symbolic->first.get();
                if (ancestor == nullptr || ancestor->GetType() == nullptr ||
                    !ancestor->GetType()->IsInteger() || ancestor->GetType()->IsUnsignedInteger() ||
                    ToWidth(*ancestor->GetType()) != resultWidth || !symbolic->second.IsSingleElement()) {
                    continue;
                }
                auto candidate = static_cast<__int128>(current.offset) +
                    static_cast<__int128>(symbolic->second.GetSingleElement().SVal());
                if (!FitsSignedWidth(candidate, resultWidth)) {
                    return std::nullopt;
                }
                auto exact = static_cast<int64_t>(candidate);
                if (auto found = offsets.find(ancestor); found != offsets.end()) {
                    if (found->second != exact) {
                        return std::nullopt;
                    }
                    continue;
                }
                if (offsets.size() >= MAX_SYMBOLIC_ANCESTORS) {
                    return std::nullopt;
                }
                offsets.emplace(ancestor, exact);
                pending.push_back(PendingAncestor{ancestor, exact, current.depth + 1});
            }
        }
        return offsets;
    };

    auto lhsAncestors = collectAncestors(lhs);
    auto rhsAncestors = collectAncestors(rhs);
    if (!lhsAncestors.has_value() || !rhsAncestors.has_value()) {
        return std::nullopt;
    }
    const auto& lhsDomain = RangeAnalysis::GetSIntDomainFromState(state, lhs);
    const auto& rhsDomain = RangeAnalysis::GetSIntDomainFromState(state, rhs);
    auto genericDifference = ComputeArithmeticBinop(CHIRArithmeticBinopArgs{
        lhsDomain, rhsDomain, lhs, rhs, ExprKind::SUB,
        binaryExpr->GetOverflowStrategy(), false});
    std::optional<int64_t> inferredDifference;
    for (const auto& [ancestor, lhsOffset] : lhsAncestors.value()) {
        auto rhsOffset = rhsAncestors->find(ancestor);
        if (rhsOffset == rhsAncestors->end()) {
            continue;
        }
        auto difference = static_cast<__int128>(lhsOffset) - rhsOffset->second;
        if (!FitsSignedWidth(difference, resultWidth)) {
            continue;
        }
        auto exact = static_cast<int64_t>(difference);
        if (inferredDifference.has_value() && inferredDifference.value() != exact) {
            return std::nullopt;
        }
        inferredDifference = exact;
    }
    if (!inferredDifference.has_value()) {
        return std::nullopt;
    }
    auto exactValue = SInt{resultWidth, static_cast<uint64_t>(inferredDifference.value())};
    if (!genericDifference.NumericBound().Contains(exactValue)) {
        return std::nullopt;
    }
    return BuildSignedExactRange(
        binaryExpr->GetResult()->GetType(), {inferredDifference.value()});
}

std::optional<SIntRange> BuildSignedValuesRange(Type* type, std::vector<int64_t> values)
{
    if (type == nullptr || values.empty()) {
        return std::nullopt;
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    if (!ExceedsExactIntSetLimit(values.size())) {
        return BuildSignedExactRange(type, values);
    }
    return BuildSignedIntervalRange(type, SignedInterval{values.front(), values.back()});
}

std::optional<__int128> ComputeCountedLoopTripCount(
    const VariableBoundInductionExit& induction, int64_t bound)
{
    auto exact = ComputeExactInductionExit(induction.init, induction.step, induction.relation, bound,
        ToWidth(*induction.loadValue->GetType()));
    if (!exact.has_value() || induction.step == 0) {
        return std::nullopt;
    }
    auto distance = static_cast<__int128>(exact->SVal()) - induction.init;
    if (distance % induction.step != 0) {
        return std::nullopt;
    }
    auto tripCount = distance / induction.step;
    return tripCount >= 0 ? std::optional<__int128>{tripCount} : std::nullopt;
}

std::optional<int64_t> ComputeAffineAccumulatorValue(
    const VariableBoundInductionExit& induction, const AffineAccumulatorUpdate& update, __int128 iteration)
{
    auto inductionSum = iteration * static_cast<__int128>(induction.init) +
        static_cast<__int128>(induction.step) * iteration * (iteration - 1) / 2;
    auto value = static_cast<__int128>(update.init) +
        static_cast<__int128>(update.inductionCoefficient) * inductionSum +
        static_cast<__int128>(update.constant) * iteration;
    if (!FitsModeledIntegerWidth(value, update.type)) {
        return std::nullopt;
    }
    return static_cast<int64_t>(value);
}

enum class AccumulatorRangePoint { BODY_ENTRY, UPDATE_RESULT, LOOP_EXIT };

std::optional<SIntRange> BuildAffineAccumulatorRange(const RangeDomain& state,
    const VariableBoundInductionExit& induction, const AffineAccumulatorUpdate& update, AccumulatorRangePoint point)
{
    constexpr __int128 MAX_AFFINE_ACCUMULATOR_EVALUATIONS = 65536;
    auto boundValues = GetSmallSignedValuesFromState(state, induction.boundValue);
    if (!boundValues.has_value()) {
        return std::nullopt;
    }
    std::vector<int64_t> values;
    __int128 evaluations = 0;
    for (auto bound : *boundValues) {
        auto tripCount = ComputeCountedLoopTripCount(induction, bound);
        if (!tripCount.has_value()) {
            return std::nullopt;
        }
        __int128 begin = 0;
        __int128 end = *tripCount;
        if (point == AccumulatorRangePoint::BODY_ENTRY) {
            if (*tripCount == 0) {
                continue;
            }
            end = *tripCount - 1;
        } else if (point == AccumulatorRangePoint::UPDATE_RESULT) {
            begin = 1;
            if (*tripCount == 0) {
                continue;
            }
        } else {
            begin = *tripCount;
        }
        auto count = end - begin + 1;
        if (count <= 0 || evaluations + count > MAX_AFFINE_ACCUMULATOR_EVALUATIONS) {
            return std::nullopt;
        }
        evaluations += count;
        for (auto iteration = begin; iteration <= end; ++iteration) {
            auto value = ComputeAffineAccumulatorValue(induction, update, iteration);
            if (!value.has_value()) {
                return std::nullopt;
            }
            values.emplace_back(value.value());
        }
    }
    return BuildSignedValuesRange(update.type, std::move(values));
}

std::optional<SIntRange> BuildCountedAccumulatorIterationRange(
    const RangeDomain& state, const VariableBoundInductionExit& induction, const CountedAccumulatorUpdate& update)
{
    auto boundValues = GetSmallSignedValuesFromState(state, induction.boundValue);
    if (!boundValues.has_value()) {
        return std::nullopt;
    }
    if (induction.step != 1) {
        return std::nullopt;
    }
    std::vector<SInt> values;
    for (auto bound : *boundValues) {
        auto inductionValues = TryEnumerateInductionValues(
            induction.init, induction.step, induction.relation, bound, ToWidth(*induction.loadValue->GetType()));
        if (!inductionValues.has_value()) {
            return std::nullopt;
        }
        auto tripCount = static_cast<__int128>(inductionValues->size());
        for (__int128 iteration = 1; iteration <= tripCount; ++iteration) {
            auto value = static_cast<__int128>(update.init) + iteration * static_cast<__int128>(update.step);
            auto width = ToWidth(*update.type);
            if (!FitsModeledIntegerWidth(value, update.type)) {
                return std::nullopt;
            }
            values.emplace_back(SInt{width, static_cast<uint64_t>(static_cast<int64_t>(value))});
            if (ExceedsExactIntSetLimit(values.size())) {
                return std::nullopt;
            }
        }
    }
    auto exactValues = NormalizeExactIntSet(std::move(values));
    if (!exactValues.has_value()) {
        return std::nullopt;
    }
    auto domain = DomainFromExactIntValues(*exactValues, update.type->IsUnsignedInteger());
    return SIntRange{std::move(domain), std::move(exactValues)};
}

std::optional<SIntRange> TryComputeCountedAccumulatorUpdateRangeImpl(
    const RangeDomain& state, const BinaryExpression* binaryExpr)
{
    auto location = GetLoadLocation(binaryExpr->GetLHSOperand());
    if (location == nullptr) {
        location = GetLoadLocation(binaryExpr->GetRHSOperand());
    }
    if (location == nullptr) {
        return std::nullopt;
    }
    auto resultUsers = binaryExpr->GetResult()->GetUsers();
    const bool isStoredBack = std::any_of(resultUsers.begin(),
        resultUsers.end(), [binaryExpr, location](auto user) {
            return user->GetExprKind() == ExprKind::STORE &&
                StaticCast<const Store*>(user)->GetLocation() == location &&
                StaticCast<const Store*>(user)->GetValue() == binaryExpr->GetResult();
        });
    if (!isStoredBack) {
        return std::nullopt;
    }
    auto rootType = GetIntegerRefRootType(location);
    if (rootType == nullptr) {
        return std::nullopt;
    }
    auto step = GetUpdateStepFromLocation(binaryExpr->GetResult(), location);
    auto context = FindCountedAccumulatorLoopContext(state, binaryExpr->GetParentBlock());
    if (!context.has_value() || context->induction.location == location) {
        return std::nullopt;
    }
    auto init = FindIncomingSignedStoreConstantBeforeLoop(context->induction.header, location, context->loopBlocks);
    if (!init.has_value()) {
        return std::nullopt;
    }
    auto affineUpdates = CollectAffineCountedAccumulatorUpdates(
        context->induction, context->branch, context->exitSuccessor);
    auto affine = std::find_if(affineUpdates.begin(), affineUpdates.end(),
        [location](const auto& update) { return update.location == location; });
    if (affine != affineUpdates.end()) {
        return BuildAffineAccumulatorRange(
            state, context->induction, *affine, AccumulatorRangePoint::UPDATE_RESULT);
    }
    if (step.has_value() && step.value() != 0) {
        return BuildCountedAccumulatorIterationRange(
            state, context->induction, CountedAccumulatorUpdate{location, rootType, init.value(), step.value()});
    }
    auto delta = GetAccumulatorDeltaIntervalFromLocation(state, binaryExpr->GetResult(), location);
    if (!delta.has_value() || delta->loadCount != 1 || (delta->delta.min == 0 && delta->delta.max == 0)) {
        return std::nullopt;
    }
    auto updateInterval = BuildCountedAccumulatorUpdateInterval(state, context->induction, init.value(), delta->delta);
    if (!updateInterval.has_value()) {
        return std::nullopt;
    }
    return BuildSignedIntervalRange(rootType, updateInterval.value());
}

std::optional<SIntRange> TryComputeCountedAccumulatorBodyLoadRangeImpl(const RangeDomain& state, const Load* load)
{
    auto location = load->GetLocation();
    auto rootType = GetIntegerRefRootType(location);
    if (rootType == nullptr) {
        return std::nullopt;
    }
    auto context = FindCountedAccumulatorLoopContext(state, load->GetParentBlock());
    if (!context.has_value() || context->induction.location == location) {
        return std::nullopt;
    }
    auto init = FindIncomingSignedStoreConstantBeforeLoop(context->induction.header, location, context->loopBlocks);
    if (!init.has_value()) {
        return std::nullopt;
    }
    auto affineUpdates = CollectAffineCountedAccumulatorUpdates(
        context->induction, context->branch, context->exitSuccessor);
    auto affine = std::find_if(affineUpdates.begin(), affineUpdates.end(),
        [location](const auto& update) { return update.location == location; });
    if (affine != affineUpdates.end()) {
        return BuildAffineAccumulatorRange(
            state, context->induction, *affine, AccumulatorRangePoint::BODY_ENTRY);
    }
    std::optional<SignedInterval> stepInterval;
    for (auto block : context->loopBlocks) {
        bool hasStoreInBlock = false;
        for (auto expr : block->GetExpressions()) {
            if (expr->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(expr);
            if (store->GetLocation() != location) {
                continue;
            }
            if (hasStoreInBlock) {
                return std::nullopt;
            }
            hasStoreInBlock = true;
            auto step = GetUpdateStepFromLocation(store->GetValue(), location);
            std::optional<SignedInterval> current;
            if (step.has_value()) {
                if (step.value() == 0) {
                    return std::nullopt;
                }
                current = SignedInterval{step.value(), step.value()};
            } else {
                auto delta = GetAccumulatorDeltaIntervalFromLocation(state, store->GetValue(), location);
                if (!delta.has_value() || delta->loadCount != 1 ||
                    (delta->delta.min == 0 && delta->delta.max == 0)) {
                    return std::nullopt;
                }
                current = delta->delta;
            }
            if (!stepInterval.has_value()) {
                stepInterval = current.value();
            } else {
                stepInterval->min = std::min(stepInterval->min, current->min);
                stepInterval->max = std::max(stepInterval->max, current->max);
            }
        }
    }
    if (!stepInterval.has_value()) {
        return std::nullopt;
    }
    auto bodyInterval = BuildCountedAccumulatorBodyInterval(state, context->induction, init.value(), stepInterval.value());
    if (!bodyInterval.has_value()) {
        return std::nullopt;
    }
    return BuildSignedIntervalRange(rootType, bodyInterval.value());
}

std::optional<SIntRange> TryComputeSimpleInductionLoadExitRangeImpl(const RangeDomain& state, const Load* load)
{
    (void)state;
    auto location = load->GetLocation();
    auto type = load->GetResult()->GetType();
    if (location == nullptr || type == nullptr || !type->IsInteger() || type->IsUnsignedInteger()) {
        return std::nullopt;
    }
    constexpr size_t MAX_EXIT_SEARCH_BLOCKS = 32;
    auto loadBlock = load->GetParentBlock();
    std::vector<const Block*> worklist{loadBlock};
    std::unordered_set<const Block*> visited;
    for (size_t index = 0; index < worklist.size() && index < MAX_EXIT_SEARCH_BLOCKS; ++index) {
        auto block = worklist[index];
        if (block == nullptr || !visited.emplace(block).second) {
            continue;
        }
        for (auto pred : block->GetPredecessors()) {
            auto terminator = pred->GetTerminator();
            if (terminator != nullptr && terminator->GetExprKind() == ExprKind::BRANCH) {
                auto branch = StaticCast<const Branch*>(terminator);
                for (auto successor : {branch->GetTrueBlock(), branch->GetFalseBlock()}) {
                    if (!IsLoopExitSuccessor(branch, successor) ||
                        !HasOnlyGuardControlledLoopExit(branch, successor)) {
                        continue;
                    }
                    std::unordered_set<const Block*> reachLoad;
                    if (!CanReachBlock(successor, loadBlock, reachLoad)) {
                        continue;
                    }
                    auto condition = GetSimpleInductionCondition(branch->GetCondition());
                    if (!condition.has_value() || condition->location != location) {
                        continue;
                    }
                    auto init = FindIncomingSignedStoreConstant(branch->GetParentBlock(), location);
                    if (!init.has_value()) {
                        init = FindIncomingSignedStoreConstantThroughPredecessors(branch->GetParentBlock(), location);
                    }
                    auto step = FindSingleBackedgeStep(branch->GetParentBlock(), location);
                    if (init.has_value() && step.has_value()) {
                        auto continuationRelation = GetLoopContinuationRelation(
                            branch, successor, condition->relation);
                        auto exact = ComputeExactInductionExit(
                            init.value(), step.value(), continuationRelation,
                            condition->bound, ToWidth(*type));
                        if (exact.has_value()) {
                            return BuildSignedExactRange(type, {exact->SVal()});
                        }
                    }
                    auto exitRelation = successor == branch->GetTrueBlock()
                        ? condition->relation
                        : NegateRelation(condition->relation);
                    auto constraint = SIntDomain::FromNumeric(exitRelation,
                        SInt{ToWidth(*type), static_cast<uint64_t>(condition->bound)}, false);
                    if (step.has_value() && (step.value() == 1 || step.value() == -1)) {
                        const auto* currentRange = GetSIntRangeFromState(state, load->GetResult());
                        auto narrowed = IntersectForNarrowing(
                            RangeAnalysis::GetSIntDomainFromState(state, load->GetResult()), constraint);
                        if (!narrowed.IsBottom() && !narrowed.IsTop()) {
                            auto exactValues = currentRange == nullptr
                                ? std::nullopt
                                : FilterExactValuesByConstraint(currentRange->GetExactValues(), constraint);
                            return SIntRange{std::move(narrowed), std::move(exactValues)};
                        }
                    }
                    return std::nullopt;
                }
            }
            worklist.emplace_back(pred);
        }
    }
    return std::nullopt;
}

std::optional<SIntRange> TryComputeCountedAccumulatorLoadExitRangeImpl(const RangeDomain& state, const Load* load)
{
    auto location = load->GetLocation();
    auto rootType = GetIntegerRefRootType(location);
    if (rootType == nullptr) {
        return std::nullopt;
    }
    constexpr size_t MAX_EXIT_SEARCH_BLOCKS = 32;
    auto loadBlock = load->GetParentBlock();
    std::vector<const Block*> worklist{loadBlock};
    std::unordered_set<const Block*> visited;
    for (size_t index = 0; index < worklist.size() && index < MAX_EXIT_SEARCH_BLOCKS; ++index) {
        auto block = worklist[index];
        if (block == nullptr || !visited.emplace(block).second) {
            continue;
        }
        for (auto pred : block->GetPredecessors()) {
            auto terminator = pred->GetTerminator();
            if (terminator != nullptr && terminator->GetExprKind() == ExprKind::BRANCH) {
                auto branch = StaticCast<const Branch*>(terminator);
                for (auto successor : {branch->GetTrueBlock(), branch->GetFalseBlock()}) {
                    std::unordered_set<const Block*> reachLoad;
                    if (!CanReachBlock(successor, loadBlock, reachLoad)) {
                        continue;
                    }
                    auto induction = GetVariableBoundInductionExit(state, branch, successor);
                    if (!induction.has_value() || induction->location == location) {
                        continue;
                    }
                    auto affineUpdates = CollectAffineCountedAccumulatorUpdates(
                        induction.value(), branch, successor);
                    auto affine = std::find_if(affineUpdates.begin(), affineUpdates.end(),
                        [location](const auto& update) { return update.location == location; });
                    if (affine != affineUpdates.end()) {
                        return BuildAffineAccumulatorRange(
                            state, induction.value(), *affine, AccumulatorRangePoint::LOOP_EXIT);
                    }
                    auto updates = CollectLinearCountedAccumulatorUpdates(induction.value(), branch, successor);
                    for (const auto& update : updates) {
                        if (update.location != location) {
                            continue;
                        }
                        return BuildCountedAccumulatorExitRange(state, induction.value(), update);
                    }
                }
            }
            worklist.emplace_back(pred);
        }
    }
    return std::nullopt;
}

bool TryNarrowVariableBoundAccumulatorExit(RangeDomain& state, const Branch* branch, const Block* successor)
{
    auto induction = GetVariableBoundInductionExit(state, branch, successor);
    if (!induction.has_value()) {
        return true;
    }
    auto storageType = GetIntegerRefRootType(induction->location);
    auto boundValues = GetSmallSignedValuesFromState(state, induction->boundValue);
    if (storageType != nullptr && boundValues.has_value()) {
        std::vector<int64_t> exitValues;
        exitValues.reserve(boundValues->size());
        bool allExitsProven = true;
        for (auto bound : *boundValues) {
            auto exact = ComputeExactInductionExit(induction->init, induction->step,
                induction->relation, bound, ToWidth(*storageType));
            if (!exact.has_value()) {
                allExitsProven = false;
                break;
            }
            exitValues.emplace_back(exact->SVal());
        }
        std::sort(exitValues.begin(), exitValues.end());
        exitValues.erase(std::unique(exitValues.begin(), exitValues.end()), exitValues.end());
        if (allExitsProven && !exitValues.empty() &&
            !ExceedsExactIntSetLimit(exitValues.size())) {
            if (auto range = BuildSignedExactRange(induction->loadValue->GetType(), exitValues);
                range.has_value()) {
                state.Update(induction->loadValue, std::make_unique<SIntRange>(std::move(range.value())));
            }
            if (auto object = state.CheckAbstractObjectRefBy(induction->location); object != nullptr) {
                if (auto range = BuildSignedExactRange(storageType, exitValues); range.has_value()) {
                    state.Update(object, std::make_unique<SIntRange>(std::move(range.value())));
                }
            }
        }
    }
    auto affineUpdates = CollectAffineCountedAccumulatorUpdates(induction.value(), branch, successor);
    for (const auto& update : affineUpdates) {
        auto range = BuildAffineAccumulatorRange(
            state, induction.value(), update, AccumulatorRangePoint::LOOP_EXIT);
        if (!range.has_value()) {
            continue;
        }
        if (auto object = state.CheckAbstractObjectRefBy(update.location); object != nullptr) {
            state.Update(object, std::make_unique<SIntRange>(std::move(range.value())));
        }
    }
    auto updates = CollectLinearCountedAccumulatorUpdates(induction.value(), branch, successor);
    for (const auto& update : updates) {
        auto range = BuildCountedAccumulatorExitRange(state, induction.value(), update);
        if (!range.has_value()) {
            continue;
        }
        if (auto object = state.CheckAbstractObjectRefBy(update.location); object != nullptr) {
            state.Update(object, std::make_unique<SIntRange>(std::move(range.value())));
        }
    }
    return true;
}

Value* GetSameWidthTypeCastSource(Value* value)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::TYPECAST) {
        return nullptr;
    }
    auto source = StaticCast<const TypeCast*>(expr)->GetSourceValue();
    if (!IsIntegerValue(source) || ToWidth(*source->GetType()) != ToWidth(*value->GetType())) {
        return nullptr;
    }
    return source;
}

// 使用单个 case 常量收窄 MultiBranch 目标值。
bool NarrowMultiBranchTarget(RangeDomain& state, Value* value, RelationalOperation rel, uint64_t caseVal)
{
    if (!IsIntegerValue(value)) {
        return true;
    }
    return NarrowSIntByRelationToConstant(
        state, value, rel, SInt{ToWidth(*value->GetType()), caseVal});
}

// 收窄 MultiBranch 条件，并在存在同宽 typecast 时回推源值。
bool NarrowMultiBranchCondition(RangeDomain& state, Value* cond, RelationalOperation rel, uint64_t caseVal)
{
    if (!NarrowMultiBranchTarget(state, cond, rel, caseVal)) {
        return false;
    }
    auto source = GetSameWidthTypeCastSource(cond);
    if (source != nullptr && source != cond) {
        return NarrowMultiBranchTarget(state, source, rel, caseVal);
    }
    return true;
}

std::optional<SIntCongruence> RefineCongruenceWithKnownBits(
    const std::optional<SIntCongruence>& current,
    const std::optional<SIntKnownBits>& knownBits, IntWidth width)
{
    auto fromBits = InferCongruenceFromKnownBits(knownBits, width);
    if (!current.has_value()) {
        return fromBits;
    }
    if (!fromBits.has_value() || fromBits->stride % current->stride != 0 ||
        fromBits->residue % current->stride != current->residue) {
        return current;
    }
    return fromBits;
}

std::unique_ptr<SIntRange> IntersectRangeWithKnownBits(const SIntDomain& current,
    const SIntRange* currentRange, const SIntKnownBits& constraint)
{
    bool compatible = true;
    auto currentBits = currentRange == nullptr
        ? InferKnownBitsFromInterval(current)
        : GetEffectiveKnownBits(current, currentRange->GetExactValues(),
              currentRange->GetCongruence(), currentRange->GetKnownBits());
    auto knownBits = IntersectSIntKnownBits(
        currentBits, constraint, current.Width(), compatible);
    if (!compatible) {
        return std::make_unique<SIntRange>(
            SIntDomain::Bottom(current.Width(), current.IsUnsigned()));
    }

    auto narrowed = SIntDomain::Intersects(current,
        DomainFromKnownBits(knownBits, current.Width(), current.IsUnsigned()));
    std::optional<std::vector<SInt>> exactValues;
    if (currentRange != nullptr && currentRange->GetExactValues().has_value()) {
        const auto mask = GetSIntWidthMask(current.Width());
        std::vector<SInt> filtered;
        for (const auto& value : *currentRange->GetExactValues()) {
            const auto raw = value.UVal() & mask;
            if ((raw & constraint.knownZero) == 0 &&
                (raw & constraint.knownOne) == constraint.knownOne) {
                filtered.emplace_back(value);
            }
        }
        exactValues = NormalizeExactIntSet(std::move(filtered));
        if (!exactValues.has_value()) {
            return std::make_unique<SIntRange>(
                SIntDomain::Bottom(current.Width(), current.IsUnsigned()));
        }
    }
    auto congruence = RefineCongruenceWithKnownBits(
        currentRange == nullptr ? std::nullopt : currentRange->GetCongruence(),
        knownBits, current.Width());
    return std::make_unique<SIntRange>(std::move(narrowed), std::move(exactValues),
        std::move(congruence), std::move(knownBits),
        currentRange == nullptr ? std::nullopt : currentRange->GetExcludedValues(),
        currentRange == nullptr ? std::nullopt : currentRange->GetIntervalFragments());
}

bool NarrowSIntByKnownBits(
    RangeDomain& state, Value* value, const SIntKnownBits& constraint)
{
    if (!IsIntegerValue(value)) {
        return true;
    }
    const auto& current = RangeAnalysis::GetSIntDomainFromState(state, value);
    const auto* currentRange = GetSIntRangeFromState(state, value);
    auto narrowed = IntersectRangeWithKnownBits(current, currentRange, constraint);
    const bool reachable = !narrowed->GetVal().IsBottom();
    state.Update(value, std::move(narrowed));

    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::LOAD) {
        return reachable;
    }
    auto location = StaticCast<const Load*>(expr)->GetLocation();
    auto rootType = location == nullptr || !location->GetType()->IsRef()
        ? nullptr
        : StaticCast<RefType*>(location->GetType())->GetRootBaseType();
    auto object = location == nullptr ? nullptr : state.CheckAbstractObjectRefBy(location);
    if (rootType == nullptr || !rootType->IsInteger() || object == nullptr) {
        return reachable;
    }
    const auto& objectDomain = GetSIntDomainFromState(state, object, rootType);
    const auto* objectRange = GetSIntRangeFromState(state, object, rootType);
    auto narrowedObject =
        IntersectRangeWithKnownBits(objectDomain, objectRange, constraint);
    const bool objectReachable = !narrowedObject->GetVal().IsBottom();
    state.Update(object, std::move(narrowedObject));
    return reachable && objectReachable;
}

bool ApplyBitMaskComparisonConstraint(RangeDomain& state, Value* maskedValue,
    RelationalOperation relation, const SInt& comparedValue)
{
    if (relation != RelationalOperation::EQ && relation != RelationalOperation::NE) {
        return true;
    }
    auto expr = GetDefiningExpr(maskedValue);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::BITAND ||
        expr->GetExprMajorKind() != ExprMajorKind::BINARY_EXPR) {
        return true;
    }
    auto binary = StaticCast<const BinaryExpression*>(expr);
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    auto lhsConstant = GetSingleIntFromDefiningConstant(lhs);
    auto rhsConstant = GetSingleIntFromDefiningConstant(rhs);
    Value* source = nullptr;
    std::optional<SInt> mask;
    if (lhsConstant.has_value() && !rhsConstant.has_value()) {
        source = rhs;
        mask = lhsConstant.value();
    } else if (rhsConstant.has_value() && !lhsConstant.has_value()) {
        source = lhs;
        mask = rhsConstant.value();
    } else {
        return true;
    }
    if (!IsIntegerValue(source) || source->GetType() == nullptr ||
        ToWidth(*source->GetType()) != mask->Width() || mask->Width() != comparedValue.Width()) {
        return true;
    }

    const auto widthMask = GetSIntWidthMask(mask->Width());
    const auto rawMask = mask->UVal() & widthMask;
    const auto rawCompared = comparedValue.UVal() & widthMask;
    if ((rawCompared & ~rawMask & widthMask) != 0) {
        return relation == RelationalOperation::NE;
    }
    if (rawMask == 0) {
        const bool equality = rawCompared == 0;
        return relation == RelationalOperation::EQ ? equality : !equality;
    }

    uint64_t required = rawCompared;
    if (relation == RelationalOperation::NE) {
        if ((rawMask & (rawMask - 1U)) != 0) {
            return true;
        }
        required = rawCompared == 0 ? rawMask : 0;
    }
    SIntKnownBits constraint{
        rawMask & ~required & widthMask, rawMask & required};
    return NarrowSIntByKnownBits(state, source, constraint);
}

// 将整数比较约束应用到分支边状态。
bool ApplyIntComparisonConstraint(
    RangeDomain& state, Value* lhs, Value* rhs, RelationalOperation rel, bool branchCondition)
{
    if (!IsIntegerValue(lhs) || !IsIntegerValue(rhs)) {
        return true;
    }
    if (!branchCondition) {
        rel = NegateRelation(rel);
    }

    const auto& lhsDomain = RangeAnalysis::GetSIntDomainFromState(state, lhs);
    const auto lhsValue = lhsDomain.IsSingleValue()
        ? std::optional<SInt>{lhsDomain.NumericBound().GetSingleElement()}
        : std::nullopt;
    const auto& rhsDomain = RangeAnalysis::GetSIntDomainFromState(state, rhs);
    const auto rhsValue = rhsDomain.IsSingleValue()
        ? std::optional<SInt>{rhsDomain.NumericBound().GetSingleElement()}
        : std::nullopt;
    if (rhsValue.has_value()) {
        if (!NarrowSIntByRelationToConstant(state, lhs, rel, rhsValue.value())) {
            return false;
        }
        if (!ApplyBitMaskComparisonConstraint(state, lhs, rel, rhsValue.value())) {
            return false;
        }
    } else if (auto rhsConstant = GetSingleIntFromDefiningConstant(rhs)) {
        if (!NarrowSIntByRelationToConstant(state, lhs, rel, rhsConstant.value())) {
            return false;
        }
        if (!ApplyBitMaskComparisonConstraint(state, lhs, rel, rhsConstant.value())) {
            return false;
        }
    }
    if (lhsValue.has_value()) {
        if (!NarrowSIntByRelationToConstant(state, rhs, SwapRelation(rel), lhsValue.value())) {
            return false;
        }
        if (!ApplyBitMaskComparisonConstraint(
                state, rhs, SwapRelation(rel), lhsValue.value())) {
            return false;
        }
    } else if (auto lhsConstant = GetSingleIntFromDefiningConstant(lhs)) {
        if (!NarrowSIntByRelationToConstant(state, rhs, SwapRelation(rel), lhsConstant.value())) {
            return false;
        }
        if (!ApplyBitMaskComparisonConstraint(
                state, rhs, SwapRelation(rel), lhsConstant.value())) {
            return false;
        }
    }
    if (lhs == rhs || ToWidth(*lhs->GetType()) != ToWidth(*rhs->GetType())) {
        return true;
    }
    if (!NarrowSIntValue(state, lhs,
        SIntDomain::FromSymbolic(rel, rhs, ToWidth(*lhs->GetType()), lhs->GetType()->IsUnsignedInteger()))) {
        return false;
    }
    if (!NarrowSIntValue(state, rhs,
        SIntDomain::FromSymbolic(SwapRelation(rel), lhs, ToWidth(*rhs->GetType()), rhs->GetType()->IsUnsignedInteger()))) {
        return false;
    }
    if (HasUpperBoundRelation(rel) && !RestoreLoopIncomingLowerBound(state, lhs)) {
        return false;
    }
    if (HasUpperBoundRelation(SwapRelation(rel)) && !RestoreLoopIncomingLowerBound(state, rhs)) {
        return false;
    }
    if (HasLowerBoundRelation(rel) && !RestoreLoopIncomingUpperBound(state, lhs)) {
        return false;
    }
    if (HasLowerBoundRelation(SwapRelation(rel)) && !RestoreLoopIncomingUpperBound(state, rhs)) {
        return false;
    }
    return true;
}

// 将布尔相等或不等约束应用到分支边状态。
bool ApplyBoolEqualityConstraint(RangeDomain& state, Value* lhs, Value* rhs, RelationalOperation rel, bool branchCondition)
{
    if (!IsBooleanValue(lhs) || !IsBooleanValue(rhs)) {
        return true;
    }
    if (!branchCondition) {
        rel = NegateRelation(rel);
    }
    if (rel != RelationalOperation::EQ && rel != RelationalOperation::NE) {
        return true;
    }

    auto lhsValue = GetSingleBoolFromStateOrConstant(state, lhs);
    auto rhsValue = GetSingleBoolFromStateOrConstant(state, rhs);
    if (lhsValue.has_value()) {
        auto value = lhsValue.value();
        if (!NarrowBoolValue(state, rhs, rel == RelationalOperation::EQ ? value : !value)) {
            return false;
        }
    }
    if (rhsValue.has_value()) {
        auto value = rhsValue.value();
        if (!NarrowBoolValue(state, lhs, rel == RelationalOperation::EQ ? value : !value)) {
            return false;
        }
    }
    return true;
}

// 处理 && 和 || 条件中的布尔恒等式与短路约束。
bool ApplyLogicalBoolConstraint(
    RangeDomain& state, const BinaryExpression* binary, ExprKind kind, bool branchCondition)
{
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    if (!IsBooleanValue(lhs) || !IsBooleanValue(rhs)) {
        return true;
    }

    auto lhsValue = GetSingleBoolFromStateOrConstant(state, lhs);
    auto rhsValue = GetSingleBoolFromStateOrConstant(state, rhs);
    if (kind == ExprKind::OR) {
        if (lhsValue.has_value()) {
            if (lhsValue.value()) {
                return branchCondition;
            }
            return ApplyConditionConstraint(state, rhs, branchCondition);
        }
        if (rhsValue.has_value()) {
            if (rhsValue.value()) {
                return branchCondition;
            }
            return ApplyConditionConstraint(state, lhs, branchCondition);
        }
        if (!branchCondition) {
            return ApplyConditionConstraint(state, lhs, false) && ApplyConditionConstraint(state, rhs, false);
        }
        return true;
    }

    if (kind == ExprKind::AND) {
        if (lhsValue.has_value()) {
            if (!lhsValue.value()) {
                return !branchCondition;
            }
            return ApplyConditionConstraint(state, rhs, branchCondition);
        }
        if (rhsValue.has_value()) {
            if (!rhsValue.value()) {
                return !branchCondition;
            }
            return ApplyConditionConstraint(state, lhs, branchCondition);
        }
        if (branchCondition) {
            return ApplyConditionConstraint(state, lhs, true) && ApplyConditionConstraint(state, rhs, true);
        }
    }
    return true;
}

// 在一个 block 中查找最后一次写入 ref 位置的布尔常量。
std::optional<bool> FindStoredBoolConstant(const Block* block, Value* location)
{
    auto exprs = block->GetExpressions();
    for (auto it = exprs.rbegin(); it != exprs.rend(); ++it) {
        if ((*it)->GetExprKind() != ExprKind::STORE) {
            continue;
        }
        auto store = StaticCast<const Store*>(*it);
        if (store->GetLocation() != location) {
            continue;
        }
        return GetSingleBoolFromDefiningConstant(store->GetValue());
    }
    return std::nullopt;
}

struct ShortCircuitBoolAlias {
    Value* source;
    bool inverted;
};

// 从 lowered 短路布尔临时 load 中恢复原始 flag。
std::optional<ShortCircuitBoolAlias> GetShortCircuitBoolAlias(Value* condition)
{
    auto expr = GetDefiningExpr(condition);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::LOAD) {
        return std::nullopt;
    }
    auto location = StaticCast<const Load*>(expr)->GetLocation();
    auto loadBlock = expr->GetParentBlock();
    if (location == nullptr || loadBlock == nullptr) {
        return std::nullopt;
    }
    auto preds = loadBlock->GetPredecessors();
    if (preds.size() != 2) {
        return std::nullopt;
    }

    const Block* controller = nullptr;
    for (auto pred : preds) {
        auto predPreds = pred->GetPredecessors();
        if (predPreds.size() != 1) {
            return std::nullopt;
        }
        if (controller == nullptr) {
            controller = predPreds.front();
        } else if (controller != predPreds.front()) {
            return std::nullopt;
        }
    }
    auto terminator = controller == nullptr ? nullptr : controller->GetTerminator();
    if (terminator == nullptr || terminator->GetExprKind() != ExprKind::BRANCH) {
        return std::nullopt;
    }
    auto branch = StaticCast<const Branch*>(terminator);
    auto trueBlock = branch->GetTrueBlock();
    auto falseBlock = branch->GetFalseBlock();
    if ((trueBlock != preds[0] && trueBlock != preds[1]) || (falseBlock != preds[0] && falseBlock != preds[1])) {
        return std::nullopt;
    }
    auto trueValue = FindStoredBoolConstant(trueBlock, location);
    auto falseValue = FindStoredBoolConstant(falseBlock, location);
    if (!trueValue.has_value() || !falseValue.has_value() || trueValue.value() == falseValue.value()) {
        return std::nullopt;
    }
    auto source = branch->GetCondition();
    if (source == condition || !IsBooleanValue(source)) {
        return std::nullopt;
    }
    return ShortCircuitBoolAlias{source, !trueValue.value() && falseValue.value()};
}

// 将所有支持的分支条件约束应用到出边状态。
bool ApplyConditionConstraint(RangeDomain& state, Value* condition, bool branchCondition)
{
    auto expr = GetDefiningExpr(condition);
    if (expr == nullptr) {
        return NarrowBoolValue(state, condition, branchCondition);
    }
    if (auto alias = GetShortCircuitBoolAlias(condition); alias.has_value()) {
        if (!ApplyConditionConstraint(state, alias->source, alias->inverted ? !branchCondition : branchCondition)) {
            return false;
        }
        (void)NarrowBoolValue(state, condition, branchCondition);
        return true;
    }
    if (expr->GetExprKind() == ExprKind::NOT) {
        if (!ApplyConditionConstraint(state, StaticCast<const UnaryExpression*>(expr)->GetOperand(), !branchCondition)) {
            return false;
        }
        (void)NarrowBoolValue(state, condition, branchCondition);
        return true;
    }
    if ((expr->GetExprKind() == ExprKind::AND || expr->GetExprKind() == ExprKind::OR) &&
        expr->GetExprMajorKind() == ExprMajorKind::BINARY_EXPR) {
        if (!ApplyLogicalBoolConstraint(state, StaticCast<const BinaryExpression*>(expr), expr->GetExprKind(), branchCondition)) {
            return false;
        }
        (void)NarrowBoolValue(state, condition, branchCondition);
        return true;
    }
    if (!IsRelationalExprKind(expr->GetExprKind())) {
        return NarrowBoolValue(state, condition, branchCondition);
    }

    auto binary = StaticCast<const BinaryExpression*>(expr);
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    auto rel = ToRelationalOperation(expr->GetExprKind());
    bool feasible = true;
    if (lhs->GetType()->IsInteger() && rhs->GetType()->IsInteger()) {
        feasible = ApplyIntComparisonConstraint(state, lhs, rhs, rel, branchCondition);
    } else if ((expr->GetExprKind() == ExprKind::EQUAL || expr->GetExprKind() == ExprKind::NOTEQUAL) &&
        lhs->GetType()->IsBoolean() && rhs->GetType()->IsBoolean()) {
        feasible = ApplyBoolEqualityConstraint(state, lhs, rhs, rel, branchCondition);
    }
    if (!feasible) {
        return false;
    }
    (void)NarrowBoolValue(state, condition, branchCondition);
    return true;
}

// 对 MultiBranch 的 case/default 后继应用对应约束。
bool ApplyMultiBranchConstraint(RangeDomain& state, const MultiBranch* multi, const Block* successor)
{
    auto cond = multi->GetCondition();
    if (!IsIntegerValue(cond)) {
        return true;
    }
    if (successor == multi->GetDefaultBlock()) {
        for (auto caseVal : multi->GetCaseVals()) {
            if (!NarrowMultiBranchCondition(
                    state, cond, RelationalOperation::NE, caseVal)) {
                return false;
            }
        }
        return true;
    }

    std::optional<uint64_t> matchedCase;
    for (size_t i = 0; i < multi->GetCaseVals().size(); ++i) {
        if (successor != multi->GetCaseBlockByIndex(i)) {
            continue;
        }
        if (matchedCase.has_value()) {
            return true;
        }
        matchedCase = multi->GetCaseValByIndex(i);
    }
    if (matchedCase.has_value()) {
        return NarrowMultiBranchCondition(state, cond, RelationalOperation::EQ, matchedCase.value());
    }
    return true;
}

// 带 visited 集合检查 CFG 可达性，用于循环检测。
bool CanReachBlock(const Block* start, const Block* target, std::unordered_set<const Block*>& visited)
{
    if (start == nullptr || target == nullptr || !visited.emplace(start).second) {
        return false;
    }
    if (start == target) {
        return true;
    }
    for (auto successor : start->GetSuccessors()) {
        if (CanReachBlock(successor, target, visited)) {
            return true;
        }
    }
    return false;
}

// 判断分支是否有后继路径回到自身 block。
bool IsLoopBranch(const Branch* branch)
{
    auto parent = branch->GetParentBlock();
    std::unordered_set<const Block*> visited;
    if (CanReachBlock(branch->GetTrueBlock(), parent, visited)) {
        return true;
    }
    visited.clear();
    return CanReachBlock(branch->GetFalseBlock(), parent, visited);
}

bool IsBoundedScalarCallTargetSupported(
    Value* result, const std::vector<Value*>& arguments, const Function* callee, size_t firstScalarArgument)
{
    if (result == nullptr || callee == nullptr) {
        return false;
    }
    auto resultType = result->GetType();
    if (resultType == nullptr || (!resultType->IsUnit() && !resultType->IsInteger() &&
        !resultType->IsBoolean() && !resultType->IsEnum()) ||
        callee->GetBody() == nullptr || callee->GetParams().size() != arguments.size() ||
        firstScalarArgument > arguments.size()) {
        return false;
    }
    return std::all_of(arguments.begin() + static_cast<std::ptrdiff_t>(firstScalarArgument),
        arguments.end(), [](const Value* argument) {
        auto type = argument == nullptr ? nullptr : argument->GetType();
        return type != nullptr && !type->IsRef() && (type->IsInteger() || type->IsBoolean());
    });
}

bool IsBoundedScalarCallSupported(const Apply* apply)
{
    if (apply == nullptr) {
        return false;
    }
    auto arguments = apply->GetArgs();
    return IsBoundedScalarCallTargetSupported(
        apply->GetResult(), arguments, DynamicCast<Function*>(apply->GetCallee()), 0);
}

bool IsBoundedScalarInvokeShapeSupported(const Invoke* invoke)
{
    if (invoke == nullptr || invoke->GetObject() == nullptr) {
        return false;
    }
    auto arguments = invoke->GetArgs();
    if (arguments.empty() || arguments.front() != invoke->GetObject()) {
        return false;
    }
    auto result = invoke->GetResult();
    auto resultType = result == nullptr ? nullptr : result->GetType();
    if (resultType == nullptr || (!resultType->IsUnit() && !resultType->IsInteger() &&
        !resultType->IsBoolean() && !resultType->IsEnum())) {
        return false;
    }
    return std::all_of(arguments.begin() + 1, arguments.end(), [](const Value* argument) {
        auto type = argument == nullptr ? nullptr : argument->GetType();
        return type != nullptr && !type->IsRef() && (type->IsInteger() || type->IsBoolean());
    });
}

bool IsLoopLocalScalarLocation(Value* location, const std::unordered_set<const Block*>& loopBlocks)
{
    auto definingExpression = GetDefiningExpr(location);
    return definingExpression != nullptr && definingExpression->GetExprKind() == ExprKind::ALLOCATE &&
        loopBlocks.find(definingExpression->GetParentBlock()) != loopBlocks.end();
}

bool IsDeadLoopLocalNullInitialization(
    const Store* store, const std::unordered_set<const Block*>& loopBlocks)
{
    if (store == nullptr || !IsLoopLocalScalarLocation(store->GetLocation(), loopBlocks)) {
        return false;
    }
    auto valueExpression = GetDefiningExpr(store->GetValue());
    if (valueExpression == nullptr || valueExpression->GetExprKind() != ExprKind::CONSTANT ||
        !StaticCast<const Constant*>(valueExpression)->IsConstantNull()) {
        return false;
    }

    // for-in lowering can create an uninitialized placeholder slot that is
    // never read and has no source-level Debug binding. It is not part of the
    // concrete loop state, so its null sentinel must not invalidate an
    // otherwise exact bounded execution.
    auto location = store->GetLocation();
    for (auto user : location->GetUsers()) {
        if (user->GetExprKind() == ExprKind::STORE &&
            StaticCast<const Store*>(user)->GetLocation() == location) {
            continue;
        }
        return false;
    }
    return true;
}

bool IsLoopLocalElementLocation(Value* location, const std::unordered_set<const Block*>& loopBlocks)
{
    auto definingExpression = GetDefiningExpr(location);
    return definingExpression != nullptr &&
        definingExpression->GetExprKind() == ExprKind::GET_ELEMENT_REF &&
        loopBlocks.find(definingExpression->GetParentBlock()) != loopBlocks.end();
}

Value* ResolveBoundedElementBaseStorage(
    Value* location, const std::unordered_set<const Block*>& loopBlocks)
{
    constexpr size_t MAX_ELEMENT_BASE_DEPTH = 16;
    std::unordered_set<Value*> visited;
    auto current = location;
    for (size_t depth = 0; current != nullptr && depth < MAX_ELEMENT_BASE_DEPTH; ++depth) {
        if (!visited.emplace(current).second) {
            return nullptr;
        }
        auto expression = GetDefiningExpr(current);
        if (expression == nullptr) {
            break;
        }
        if (expression->GetExprKind() == ExprKind::LOAD) {
            current = StaticCast<const Load*>(expression)->GetLocation();
            continue;
        }
        if (expression->GetExprKind() == ExprKind::GET_ELEMENT_REF) {
            current = StaticCast<const GetElementRef*>(expression)->GetLocation();
            continue;
        }
        if (expression->GetExprKind() == ExprKind::TYPECAST) {
            auto cast = StaticCast<const TypeCast*>(expression);
            if (cast->GetSourceTy()->IsRef() && cast->GetTargetTy()->IsRef()) {
                current = cast->GetSourceValue();
                continue;
            }
        }
        if (loopBlocks.find(expression->GetParentBlock()) != loopBlocks.end()) {
            return nullptr;
        }
        break;
    }
    auto rootType = current == nullptr ? nullptr : GetRefRootBaseType(current->GetType());
    return rootType != nullptr &&
            (rootType->IsClass() || rootType->IsStruct() || rootType->IsTuple())
        ? current
        : nullptr;
}

bool IsBoundedFlatAggregateType(const Type* type)
{
    return type != nullptr && (type->IsEnum() || type->IsTuple());
}

bool HasOnlyBoundedAggregateElements(const Expression* expression)
{
    auto operands = expression->GetOperands();
    return std::all_of(operands.begin(), operands.end(), [](const Value* operand) {
        auto type = operand == nullptr ? nullptr : operand->GetType();
        return type != nullptr && !type->IsRef() && (type->IsInteger() || type->IsBoolean());
    });
}

bool IsProvenBoundedStructArrayAccessor(const Apply* apply)
{
    if (apply == nullptr) {
        return false;
    }
    const auto& args = apply->GetArgs();
    if (args.empty() || !IsStructArrayValue(args.front()) ||
        !LookupStructArrayLiteralInfo(args.front()).has_value()) {
        return false;
    }
    if (IsProvenStructArrayIndexBody(apply, /* isSetter = */ false)) {
        auto resultType = apply->GetResult()->GetType();
        return resultType != nullptr && (resultType->IsInteger() || resultType->IsBoolean());
    }
    if (!IsProvenStructArrayIndexBody(apply, /* isSetter = */ true) || args.size() != 3) {
        return false;
    }
    auto valueType = args[2] == nullptr ? nullptr : args[2]->GetType();
    return valueType != nullptr && !valueType->IsRef() &&
        (valueType->IsInteger() || valueType->IsBoolean());
}

bool IsProvenBoundedStructArrayLoad(const Load* load)
{
    if (load == nullptr || !IsStructArrayValue(load->GetResult()) ||
        !LookupStructArrayLiteralInfo(load->GetLocation()).has_value()) {
        return false;
    }
    auto locationType = load->GetLocation()->GetType();
    auto rootType = locationType != nullptr && locationType->IsRef()
        ? StaticCast<RefType*>(locationType)->GetRootBaseType()
        : nullptr;
    auto users = load->GetResult()->GetUsers();
    return rootType != nullptr && rootType->IsStructArray() && !users.empty() &&
        std::all_of(users.begin(), users.end(), [load](const Expression* user) {
            if (user == nullptr || user->GetExprKind() != ExprKind::APPLY) {
                return false;
            }
            auto apply = StaticCast<const Apply*>(user);
            const auto& args = apply->GetArgs();
            return !args.empty() && args.front() == load->GetResult() &&
                IsProvenBoundedStructArrayAccessor(apply);
        });
}

bool IsBoundedScalarLoopExpressionSupported(const Expression* expression)
{
    if (expression == nullptr) {
        return false;
    }
    if (expression->GetExprMajorKind() == ExprMajorKind::UNARY_EXPR ||
        expression->GetExprMajorKind() == ExprMajorKind::BINARY_EXPR) {
        auto resultType = expression->GetResult()->GetType();
        return resultType != nullptr && (resultType->IsInteger() || resultType->IsBoolean());
    }
    switch (expression->GetExprKind()) {
        case ExprKind::DEBUGEXPR:
            return true;
        case ExprKind::CONSTANT: {
            auto type = expression->GetResult()->GetType();
            return type != nullptr && (type->IsInteger() || type->IsBoolean());
        }
        case ExprKind::LOAD: {
            auto load = StaticCast<const Load*>(expression);
            auto rootType = GetBoundedLoopRefRootType(load->GetLocation());
            if (rootType != nullptr && (rootType->IsBoolean() ||
                rootType->IsInteger() || IsBoundedFlatAggregateType(rootType))) {
                return true;
            }
            auto loadedRootType = GetRefRootBaseType(load->GetResult()->GetType());
            return IsProvenBoundedStructArrayLoad(load) ||
                (loadedRootType != nullptr &&
                (loadedRootType->IsClass() || loadedRootType->IsStruct() ||
                    loadedRootType->IsTuple()));
        }
        case ExprKind::STORE: {
            auto store = StaticCast<const Store*>(expression);
            auto rootType = GetBoundedLoopRefRootType(store->GetLocation());
            return rootType != nullptr && (rootType->IsBoolean() ||
                rootType->IsInteger() || IsBoundedFlatAggregateType(rootType));
        }
        case ExprKind::GET_ELEMENT_REF: {
            auto getElement = StaticCast<const GetElementRef*>(expression);
            auto resultRootType = GetRefRootBaseType(getElement->GetResult()->GetType());
            auto locationRootType = GetRefRootBaseType(getElement->GetLocation()->GetType());
            return !getElement->GetPath().empty() && resultRootType != nullptr &&
                (resultRootType->IsBoolean() || resultRootType->IsInteger() ||
                    IsBoundedFlatAggregateType(resultRootType) ||
                    resultRootType->IsClass() || resultRootType->IsStruct()) &&
                locationRootType != nullptr &&
                (locationRootType->IsClass() || locationRootType->IsStruct() ||
                    locationRootType->IsTuple());
        }
        case ExprKind::STORE_ELEMENT_REF: {
            auto storeElement = StaticCast<const StoreElementRef*>(expression);
            auto valueType = storeElement->GetValue()->GetType();
            auto locationRootType = GetRefRootBaseType(storeElement->GetLocation()->GetType());
            return !storeElement->GetPath().empty() && valueType != nullptr &&
                (valueType->IsBoolean() || valueType->IsInteger() ||
                    IsBoundedFlatAggregateType(valueType)) &&
                locationRootType != nullptr &&
                (locationRootType->IsClass() || locationRootType->IsStruct() ||
                    locationRootType->IsTuple());
        }
        case ExprKind::TYPECAST: {
            auto cast = StaticCast<const TypeCast*>(expression);
            return (cast->GetSourceTy()->IsInteger() && cast->GetTargetTy()->IsInteger()) ||
                (cast->GetSourceTy()->IsEnum() && cast->GetTargetTy()->IsTuple());
        }
        case ExprKind::ALLOCATE: {
            auto rootType = GetBoundedLoopRefRootType(expression->GetResult());
            return rootType != nullptr &&
                (rootType->IsInteger() || rootType->IsBoolean() || IsBoundedFlatAggregateType(rootType));
        }
        case ExprKind::TUPLE:
            return IsBoundedFlatAggregateType(expression->GetResult()->GetType()) &&
                HasOnlyBoundedAggregateElements(expression);
        case ExprKind::FIELD: {
            auto field = StaticCast<const Field*>(expression);
            auto resultType = field->GetResult()->GetType();
            auto baseType = field->GetBase()->GetType();
            return resultType != nullptr && (resultType->IsInteger() || resultType->IsBoolean()) &&
                IsBoundedFlatAggregateType(baseType);
        }
        case ExprKind::APPLY:
            return IsProvenBoundedStructArrayAccessor(StaticCast<const Apply*>(expression)) ||
                IsBoundedScalarCallSupported(StaticCast<const Apply*>(expression));
        case ExprKind::INVOKE:
            return IsBoundedScalarInvokeShapeSupported(StaticCast<const Invoke*>(expression));
        default:
            return false;
    }
}

bool IsSafeQGSRProjectionExpression(
    const Expression* expression, const std::unordered_set<const Value*>& ignoredValues)
{
    if (expression == nullptr) {
        return false;
    }
    if (expression->GetExprMajorKind() == ExprMajorKind::UNARY_EXPR ||
        expression->GetExprMajorKind() == ExprMajorKind::BINARY_EXPR) {
        return true;
    }
    switch (expression->GetExprKind()) {
        case ExprKind::ALLOCATE:
        case ExprKind::LOAD:
        case ExprKind::STORE:
        case ExprKind::GET_ELEMENT_REF:
        case ExprKind::GET_ELEMENT_BY_NAME:
        case ExprKind::STORE_ELEMENT_REF:
        case ExprKind::STORE_ELEMENT_BY_NAME:
        case ExprKind::DEBUGEXPR:
        case ExprKind::FIELD:
        case ExprKind::FIELD_BY_NAME:
        case ExprKind::TYPECAST:
            return true;
        case ExprKind::APPLY: {
            auto apply = StaticCast<const Apply*>(expression);
            auto arguments = apply->GetArgs();
            auto callee = DynamicCast<Function*>(apply->GetCallee());
            if (callee == nullptr || arguments.empty() ||
                ignoredValues.find(arguments.front()) == ignoredValues.end()) {
                return false;
            }
            auto globals = CollectTrackedMutableGlobals(callee);
            return globals.complete && globals.size() == 0 &&
                std::all_of(arguments.begin() + 1, arguments.end(), [](const Value* argument) {
                    auto type = argument == nullptr ? nullptr : argument->GetType();
                    return type != nullptr && !type->IsRef() && (type->IsInteger() || type->IsBoolean());
                });
        }
        default:
            return false;
    }
}

struct QGSRLoopProjection {
    std::unordered_set<const Expression*> ignoredExpressions;
    std::unordered_set<const Value*> ignoredValues;
    std::unordered_set<Value*> ignoredLocations;
};

// Project only chains rooted at an unsupported loop-local allocation. A call
// is skipped only when it can affect the ignored receiver but no tracked
// global; taint reaching loop control is rejected before execution.
std::optional<QGSRLoopProjection> BuildQGSRLoopProjection(
    const std::unordered_set<const Block*>& loopBlocks,
    const std::unordered_set<const Value*>& queryValues)
{
    QGSRLoopProjection projection;
    std::vector<const Expression*> expressions;
    for (auto block : loopBlocks) {
        for (auto expression : block->GetNonTerminatorExpressions()) {
            expressions.emplace_back(expression);
            if (expression->GetExprKind() != ExprKind::ALLOCATE ||
                IsBoundedScalarLoopExpressionSupported(expression)) {
                continue;
            }
            projection.ignoredExpressions.emplace(expression);
            projection.ignoredValues.emplace(expression->GetResult());
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto expression : expressions) {
            const auto operands = expression->GetOperands();
            const bool usesIgnoredValue = std::any_of(operands.begin(), operands.end(), [&](Value* operand) {
                return projection.ignoredValues.find(operand) != projection.ignoredValues.end();
            });
            if (!usesIgnoredValue) {
                continue;
            }
            if (!IsSafeQGSRProjectionExpression(expression, projection.ignoredValues)) {
                if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                    std::cerr << "[RangeAnalysisQGSR] unsafeDependency kind="
                              << static_cast<int>(expression->GetExprKind()) << '\n';
                }
                return std::nullopt;
            }
            changed = projection.ignoredExpressions.emplace(expression).second || changed;
            if (auto result = expression->GetResult(); result != nullptr) {
                changed = projection.ignoredValues.emplace(result).second || changed;
            }
            if (expression->GetExprKind() == ExprKind::STORE) {
                auto location = StaticCast<const Store*>(expression)->GetLocation();
                changed = projection.ignoredValues.emplace(location).second || changed;
                projection.ignoredLocations.emplace(location);
            }
        }
    }

    if (std::any_of(queryValues.begin(), queryValues.end(), [&projection](const Value* value) {
            return projection.ignoredValues.find(value) != projection.ignoredValues.end();
        })) {
        return std::nullopt;
    }
    for (auto expression : expressions) {
        if (projection.ignoredExpressions.find(expression) == projection.ignoredExpressions.end() &&
            !IsBoundedScalarLoopExpressionSupported(expression)) {
            if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                std::cerr << "[RangeAnalysisQGSR] unsupportedProjection kind="
                          << static_cast<int>(expression->GetExprKind()) << '\n';
            }
            return std::nullopt;
        }
    }
    return projection;
}

struct PureAffineQueryForm {
    __int128 coefficient{0};
    __int128 constant{0};
};

bool IsSameModeledIntegerType(Type* lhs, Type* rhs)
{
    return lhs != nullptr && rhs != nullptr && lhs->IsInteger() && rhs->IsInteger() &&
        lhs->IsUnsignedInteger() == rhs->IsUnsignedInteger() && ToWidth(*lhs) == ToWidth(*rhs);
}

bool CheckedAffineAdd(__int128 lhs, __int128 rhs, __int128& result)
{
    if (!FitsSignedWidth(lhs, IntWidth::I64) || !FitsSignedWidth(rhs, IntWidth::I64)) {
        return false;
    }
    result = lhs + rhs;
    return FitsSignedWidth(result, IntWidth::I64);
}

bool CheckedAffineSub(__int128 lhs, __int128 rhs, __int128& result)
{
    if (!FitsSignedWidth(lhs, IntWidth::I64) || !FitsSignedWidth(rhs, IntWidth::I64)) {
        return false;
    }
    result = lhs - rhs;
    return FitsSignedWidth(result, IntWidth::I64);
}

bool CheckedAffineMul(__int128 lhs, __int128 rhs, __int128& result)
{
    if (!FitsSignedWidth(lhs, IntWidth::I64) || !FitsSignedWidth(rhs, IntWidth::I64)) {
        return false;
    }
    result = lhs * rhs;
    return FitsSignedWidth(result, IntWidth::I64);
}

bool AffineFormFitsTypeAcrossInduction(
    const PureAffineQueryForm& form, __int128 inductionMin, __int128 inductionMax, Type* type)
{
    __int128 firstProduct = 0;
    __int128 secondProduct = 0;
    __int128 first = 0;
    __int128 second = 0;
    if (!FitsSignedWidth(form.coefficient, IntWidth::I64) ||
        !FitsSignedWidth(form.constant, IntWidth::I64) ||
        !FitsSignedWidth(inductionMin, IntWidth::I64) ||
        !FitsSignedWidth(inductionMax, IntWidth::I64)) {
        return false;
    }
    firstProduct = form.coefficient * inductionMin;
    secondProduct = form.coefficient * inductionMax;
    first = firstProduct + form.constant;
    second = secondProduct + form.constant;
    return FitsModeledIntegerWidth(std::min(first, second), type) &&
        FitsModeledIntegerWidth(std::max(first, second), type);
}

Value* GetUniqueLoopLocalStoreValue(
    Value* location, const std::unordered_set<const Block*>& loopBlocks)
{
    auto definition = GetDefiningExpr(location);
    if (definition == nullptr || definition->GetExprKind() != ExprKind::ALLOCATE ||
        loopBlocks.find(definition->GetParentBlock()) == loopBlocks.end()) {
        return nullptr;
    }
    const Store* definingStore = nullptr;
    for (auto user : location->GetUsers()) {
        if (user == nullptr || user->GetExprKind() == ExprKind::DEBUGEXPR) {
            continue;
        }
        if (user->GetExprKind() == ExprKind::LOAD &&
            StaticCast<const Load*>(user)->GetLocation() == location) {
            continue;
        }
        if (user->GetExprKind() == ExprKind::STORE &&
            StaticCast<const Store*>(user)->GetLocation() == location &&
            loopBlocks.find(user->GetParentBlock()) != loopBlocks.end() && definingStore == nullptr) {
            definingStore = StaticCast<const Store*>(user);
            continue;
        }
        return nullptr;
    }
    return definingStore == nullptr ? nullptr : definingStore->GetValue();
}

std::optional<PureAffineQueryForm> GetPureAffineQueryForm(Value* value, Value* inductionLocation,
    Type* modeledType, __int128 inductionMin, __int128 inductionMax,
    const std::unordered_set<const Block*>& loopBlocks, std::unordered_set<Value*>& expandingLocations,
    size_t depth = 0)
{
    constexpr size_t MAX_AFFINE_QUERY_DEPTH = 12;
    if (value == nullptr || inductionLocation == nullptr || modeledType == nullptr ||
        depth > MAX_AFFINE_QUERY_DEPTH || !IsSameModeledIntegerType(value->GetType(), modeledType) ||
        modeledType->IsUnsignedInteger()) {
        return std::nullopt;
    }
    if (auto constant = GetSignedConstantFromDefiningConstant(value); constant.has_value()) {
        PureAffineQueryForm result{0, constant.value()};
        return AffineFormFitsTypeAcrossInduction(result, inductionMin, inductionMax, modeledType)
            ? std::optional<PureAffineQueryForm>{result}
            : std::nullopt;
    }
    auto loadLocation = GetLoadLocation(value);
    if (loadLocation == inductionLocation) {
        PureAffineQueryForm result{1, 0};
        return AffineFormFitsTypeAcrossInduction(result, inductionMin, inductionMax, modeledType)
            ? std::optional<PureAffineQueryForm>{result}
            : std::nullopt;
    }
    if (loadLocation != nullptr) {
        if (!expandingLocations.emplace(loadLocation).second) {
            return std::nullopt;
        }
        auto storedValue = GetUniqueLoopLocalStoreValue(loadLocation, loopBlocks);
        auto result = storedValue == nullptr ? std::nullopt :
            GetPureAffineQueryForm(storedValue, inductionLocation, modeledType,
                inductionMin, inductionMax, loopBlocks, expandingLocations, depth + 1);
        expandingLocations.erase(loadLocation);
        return result;
    }
    auto expression = GetDefiningExpr(value);
    if (expression == nullptr || (expression->GetExprKind() != ExprKind::ADD &&
        expression->GetExprKind() != ExprKind::SUB && expression->GetExprKind() != ExprKind::MUL)) {
        return std::nullopt;
    }
    auto binary = StaticCast<const BinaryExpression*>(expression);
    if (binary->GetOverflowStrategy() != OverflowStrategy::THROWING ||
        !IsSameModeledIntegerType(binary->GetLHSOperand()->GetType(), modeledType) ||
        !IsSameModeledIntegerType(binary->GetRHSOperand()->GetType(), modeledType)) {
        return std::nullopt;
    }
    auto lhs = GetPureAffineQueryForm(binary->GetLHSOperand(), inductionLocation, modeledType,
        inductionMin, inductionMax, loopBlocks, expandingLocations, depth + 1);
    auto rhs = GetPureAffineQueryForm(binary->GetRHSOperand(), inductionLocation, modeledType,
        inductionMin, inductionMax, loopBlocks, expandingLocations, depth + 1);
    if (!lhs.has_value() || !rhs.has_value()) {
        return std::nullopt;
    }

    PureAffineQueryForm result;
    if (expression->GetExprKind() == ExprKind::ADD) {
        if (!CheckedAffineAdd(lhs->coefficient, rhs->coefficient, result.coefficient) ||
            !CheckedAffineAdd(lhs->constant, rhs->constant, result.constant)) {
            return std::nullopt;
        }
    } else if (expression->GetExprKind() == ExprKind::SUB) {
        if (!CheckedAffineSub(lhs->coefficient, rhs->coefficient, result.coefficient) ||
            !CheckedAffineSub(lhs->constant, rhs->constant, result.constant)) {
            return std::nullopt;
        }
    } else {
        const bool lhsConstant = lhs->coefficient == 0;
        const bool rhsConstant = rhs->coefficient == 0;
        if (!lhsConstant && !rhsConstant) {
            return std::nullopt;
        }
        const auto& affine = lhsConstant ? rhs.value() : lhs.value();
        const auto factor = lhsConstant ? lhs->constant : rhs->constant;
        if (!CheckedAffineMul(affine.coefficient, factor, result.coefficient) ||
            !CheckedAffineMul(affine.constant, factor, result.constant)) {
            return std::nullopt;
        }
    }
    return AffineFormFitsTypeAcrossInduction(result, inductionMin, inductionMax, modeledType)
        ? std::optional<PureAffineQueryForm>{result}
        : std::nullopt;
}

bool IsCallExpressionKind(ExprKind kind)
{
    return kind == ExprKind::APPLY || kind == ExprKind::INVOKE ||
        kind == ExprKind::APPLY_WITH_EXCEPTION || kind == ExprKind::INVOKE_WITH_EXCEPTION;
}

bool QueryRootTouchesLoop(const Value* root, const std::unordered_set<const Block*>& loopBlocks)
{
    auto definition = GetDefiningExpr(const_cast<Value*>(root));
    if (definition != nullptr && loopBlocks.find(definition->GetParentBlock()) != loopBlocks.end()) {
        return true;
    }
    auto users = root->GetUsers();
    return std::any_of(users.begin(), users.end(), [&loopBlocks](const Expression* user) {
        return user != nullptr && loopBlocks.find(user->GetParentBlock()) != loopBlocks.end();
    });
}

bool IsPureAffineQueryBlockUnconditional(const Block* block,
    const VariableBoundInductionExit& induction, const Branch* branch,
    const Block* exitSuccessor, const std::unordered_set<const Block*>& loopBlocks)
{
    if (block == induction.header) {
        return true;
    }
    return IsProvenSingleExecutionPerLoopIteration(
        induction, branch, exitSuccessor, block, loopBlocks);
}

bool IsPureAffineQueryRoot(Value* root, const std::unordered_set<const Block*>& loopBlocks,
    const VariableBoundInductionExit& induction, const Branch* branch,
    const Block* exitSuccessor, __int128 inductionMin, __int128 inductionMax)
{
    auto definition = GetDefiningExpr(root);
    if (definition == nullptr || loopBlocks.find(definition->GetParentBlock()) == loopBlocks.end()) {
        return false;
    }
    if (!IsPureAffineQueryBlockUnconditional(
            definition->GetParentBlock(), induction, branch, exitSuccessor, loopBlocks)) {
        return false;
    }
    if (root->GetType() != nullptr && root->GetType()->IsInteger()) {
        if (root->GetType()->IsUnsignedInteger() ||
            !IsSameModeledIntegerType(root->GetType(), induction.loadValue->GetType())) {
            return false;
        }
        std::unordered_set<Value*> expandingLocations;
        return GetPureAffineQueryForm(root, induction.location, root->GetType(),
            inductionMin, inductionMax, loopBlocks, expandingLocations).has_value();
    }
    if (definition->GetExprKind() != ExprKind::ALLOCATE) {
        return false;
    }
    auto modeledType = GetBoundedLoopRefRootType(root);
    if (modeledType == nullptr || !modeledType->IsInteger() || modeledType->IsUnsignedInteger() ||
        !IsSameModeledIntegerType(modeledType, induction.loadValue->GetType())) {
        return false;
    }

    const Store* definingStore = nullptr;
    for (auto user : root->GetUsers()) {
        if (user == nullptr || user->GetExprKind() == ExprKind::DEBUGEXPR) {
            continue;
        }
        if (user->GetExprKind() == ExprKind::LOAD &&
            StaticCast<const Load*>(user)->GetLocation() == root) {
            continue;
        }
        if (user->GetExprKind() == ExprKind::STORE &&
            StaticCast<const Store*>(user)->GetLocation() == root &&
            loopBlocks.find(user->GetParentBlock()) != loopBlocks.end() && definingStore == nullptr) {
            definingStore = StaticCast<const Store*>(user);
            continue;
        }
        return false;
    }
    std::unordered_set<Value*> expandingLocations{root};
    return definingStore != nullptr &&
        IsPureAffineQueryBlockUnconditional(definingStore->GetParentBlock(), induction,
            branch, exitSuccessor, loopBlocks) &&
        GetPureAffineQueryForm(definingStore->GetValue(), induction.location, modeledType,
            inductionMin, inductionMax, loopBlocks, expandingLocations).has_value();
}

bool CanSkipBoundedReplayForPureAffineQueries(const RangeDomain& state, const Branch* branch,
    const Block* exitSuccessor, const std::unordered_set<const Block*>& loopBlocks)
{
    auto induction = GetVariableBoundInductionExit(
        state, branch, exitSuccessor, /* resolveForwardedCondition = */ true);
    if (!induction.has_value() || induction->step != 1 ||
        induction->loadValue->GetType()->IsUnsignedInteger()) {
        return false;
    }
    auto forest = GetLoopForest(induction->header);
    auto loop = forest == nullptr ? nullptr : forest->FindByHeader(induction->header);
    if (loop == nullptr || loop->incoming.size() != 1 || loop->latches.empty() ||
        loop->latches.size() > 2 || loop->exits.size() != 1 ||
        !loop->IsExitEdge(branch->GetParentBlock(), exitSuccessor)) {
        return false;
    }
    for (auto block : loopBlocks) {
        if (forest->FindInnermost(block) != loop) {
            return false;
        }
        for (auto expression : block->GetExpressions()) {
            if (expression != nullptr && IsCallExpressionKind(expression->GetExprKind())) {
                return false;
            }
        }
    }

    auto bound = GetSignedConstantFromDefiningConstant(induction->boundValue);
    if (!bound.has_value() ||
        !SatisfiesSignedRelation(induction->init, induction->relation, bound.value())) {
        return false;
    }
    auto exactExit = ComputeExactInductionExit(induction->init, induction->step,
        induction->relation, bound.value(), ToWidth(*induction->loadValue->GetType()));
    if (!exactExit.has_value()) {
        return false;
    }
    const __int128 inductionMin = induction->init;
    const __int128 inductionMax = static_cast<__int128>(exactExit->SVal()) - induction->step;
    if (inductionMax < inductionMin) {
        return false;
    }

    size_t relevantRoots = 0;
    for (auto queryRoot : queryRefinementRoots) {
        if (queryRoot == nullptr || !QueryRootTouchesLoop(queryRoot, loopBlocks)) {
            continue;
        }
        ++relevantRoots;
        if (!IsPureAffineQueryRoot(const_cast<Value*>(queryRoot), loopBlocks,
            induction.value(), branch, exitSuccessor, inductionMin, inductionMax)) {
            return false;
        }
    }
    return relevantRoots != 0;
}
} // namespace

std::optional<RangeDomain> RangeAnalysis::TryEvaluateBoundedScalarLoopExit(
    const RangeDomain& state, const Branch* branch, const Block* successor,
    size_t stepBudget, bool publishPrefixOnBudget)
{
    constexpr size_t MAX_BOUNDED_LOOP_BLOCKS = 128;
    constexpr size_t MAX_BOUNDED_LOOP_LOCATIONS = 64;
    // Repeated static calls that hit an existing scalar summary are cheap,
    // while a new context or a virtual dispatch can trigger more analysis.
    // Keep the conservative Invoke limit, but let deterministic loops reuse
    // bounded Apply summaries up to the existing global summary budget.
    constexpr size_t MAX_BOUNDED_LOOP_APPLY_EVALUATIONS = 2048;
    constexpr size_t MAX_BOUNDED_LOOP_INVOKE_EVALUATIONS = 256;
    constexpr size_t MAX_BOUNDED_LOOP_UNIQUE_APPLY_CONTEXTS =
        MAX_TOTAL_BOUNDED_LOOP_CONTEXT_SUMMARIES;
    constexpr size_t MAX_BOUNDED_LOOP_UNIQUE_INVOKE_CONTEXTS = 256;
    constexpr size_t MAX_BOUNDED_LOOP_CACHE_ENTRIES = 64;
    if (stepBudget == 0 || stepBudget > MAX_BOUNDED_LOOP_STEPS) {
        return std::nullopt;
    }
    const auto traceReject = [branch](const char* reason) {
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") == nullptr) {
            return;
        }
        const auto& location = branch->GetDebugLocation();
        std::cerr << "[RangeAnalysisBoundedReject] reason=" << reason
                  << " loop-line=" << location.GetBeginPos().line << '\n';
    };
    if (boundedLoopEvaluationOwner != nullptr && boundedLoopEvaluationOwner != this) {
        return std::nullopt;
    }
    auto* parentCallContextRecorder = boundedLoopCallContextRecorder;
    const Block* exitSuccessor = nullptr;
    for (auto candidate : {branch->GetTrueBlock(), branch->GetFalseBlock()}) {
        if (IsLoopExitSuccessor(branch, candidate)) {
            exitSuccessor = candidate;
            break;
        }
    }
    if (exitSuccessor == nullptr) {
        return std::nullopt;
    }
    const bool evaluateExitEdge = successor == exitSuccessor;
    auto header = branch->GetParentBlock();
    if (auto forest = GetLoopForest(header); forest != nullptr) {
        auto enclosingLoop = forest->FindInnermost(header);
        auto enclosingTerminator = enclosingLoop == nullptr || enclosingLoop->header == header
            ? nullptr
            : enclosingLoop->header->GetTerminator();
        if (enclosingTerminator != nullptr &&
            enclosingTerminator->GetExprKind() == ExprKind::BRANCH &&
            ShouldPreferAnalyticInductionOverBoundedReplay(
                state, StaticCast<const Branch*>(enclosingTerminator))) {
            traceReject("analytic-enclosing-loop");
            return std::nullopt;
        }
    }
    auto loopBlocks = CollectLoopBackPathBlockSet(branch, exitSuccessor, header);
    if (header == nullptr || loopBlocks.empty() || loopBlocks.size() > MAX_BOUNDED_LOOP_BLOCKS) {
        return std::nullopt;
    }
    if (CanSkipBoundedReplayForPureAffineQueries(state, branch, exitSuccessor, loopBlocks)) {
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisBoundedSkip] reason=pure-affine-query\n";
        }
        return std::nullopt;
    }
    std::optional<QGSRLoopProjection> qgsrProjection;
    const bool isQueryRelevantLoop =
        std::any_of(queryRefinementBlocks.begin(), queryRefinementBlocks.end(), [this, header](const Block* block) {
            if (block == nullptr || block->GetTopLevelFunc() != func) {
                return false;
            }
            std::unordered_set<const Block*> visited;
            if (!CanReachBlock(header, block, visited)) {
                return false;
            }
            visited.clear();
            return CanReachBlock(block, header, visited);
        });
    if (isQueryRelevantLoop) {
        qgsrProjection = BuildQGSRLoopProjection(loopBlocks, queryRefinementValues);
    }
    const auto isProjectedExpression = [&qgsrProjection](const Expression* expression) {
        return qgsrProjection.has_value() &&
            qgsrProjection->ignoredExpressions.find(expression) != qgsrProjection->ignoredExpressions.end();
    };
    const auto havocProjectedLocations = [&qgsrProjection](RangeDomain& result) {
        if (!qgsrProjection.has_value()) {
            return;
        }
        for (auto location : qgsrProjection->ignoredLocations) {
            auto rootType = GetBoundedLoopRefRootType(location);
            if (rootType == nullptr || (!rootType->IsInteger() && !rootType->IsBoolean())) {
                continue;
            }
            if (auto object = result.CheckAbstractObjectRefBy(location); object != nullptr) {
                result.SetToBound(object, /* isTop = */ true);
            } else {
                result.SetToTopOrTopRef(location, /* isRef = */ true);
            }
        }
    };
    const auto& analysisContext = analysisContextKey;
    std::vector<const Expression*> loopExpressions;
    std::vector<const Expression*> loopCallExpressions;
    for (auto block : loopBlocks) {
        for (auto expression : block->GetNonTerminatorExpressions()) {
            loopExpressions.emplace_back(expression);
            if (expression->GetExprKind() == ExprKind::APPLY ||
                expression->GetExprKind() == ExprKind::INVOKE) {
                loopCallExpressions.emplace_back(expression);
            }
        }
    }
    const std::vector<const Expression*> noCallExpressions;
    struct BoundedContextAttempt {
        ~BoundedContextAttempt()
        {
            if (succeeded) {
                return;
            }
            if (incompleteObservations != nullptr) {
                incompleteObservations->insert(loopExpressions.begin(), loopExpressions.end());
            }
            if (!callExpressions.empty()) {
                if (parentRecorder != nullptr) {
                    MarkBoundedLoopCallContextsIncomplete(*parentRecorder, callExpressions);
                } else {
                    MarkBoundedLoopCallContextsIncomplete(parentContext, callExpressions);
                }
            }
        }

        void MarkSucceeded()
        {
            succeeded = true;
        }

        const std::string& parentContext;
        const std::vector<const Expression*>& loopExpressions;
        const std::vector<const Expression*>& callExpressions;
        BoundedLoopCallContextMap* parentRecorder;
        std::unordered_set<const Expression*>* incompleteObservations;
        bool succeeded{false};
    } boundedContextAttempt{analysisContext, loopExpressions,
        publishPrefixOnBudget ? noCallExpressions : loopCallExpressions,
        parentCallContextRecorder,
        isContextAnalysis && !publishPrefixOnBudget ? &incompleteLocalBoundedLoopObservations : nullptr};
    const Block* loopEntry = nullptr;
    for (auto block : loopBlocks) {
        bool hasOutsidePredecessor = false;
        for (auto predecessor : block->GetPredecessors()) {
            if (loopBlocks.find(predecessor) == loopBlocks.end()) {
                hasOutsidePredecessor = true;
                break;
            }
        }
        if (!hasOutsidePredecessor) {
            continue;
        }
        if (loopEntry != nullptr && loopEntry != block) {
            return std::nullopt;
        }
        loopEntry = block;
    }
    if (loopEntry == nullptr) {
        loopEntry = header;
    }

    std::vector<Value*> mutatedLocations;
    std::vector<Value*> readLocations;
    std::vector<Value*> elementBaseLocations;
    std::vector<Value*> arrayBackingValues;
    std::vector<Value*> externalValues;
    std::vector<Value*> callReceiverValues;
    std::unordered_set<Value*> seenMutatedLocations;
    std::unordered_set<Value*> seenReadLocations;
    std::unordered_set<Value*> seenElementBaseLocations;
    std::unordered_set<Value*> seenArrayBackingValues;
    std::unordered_set<Value*> seenExternalValues;
    std::unordered_set<Value*> seenCallReceiverValues;
    struct BoundedStandardRangeIterator {
        Type* elementType;
        std::vector<SInt> values;
        size_t nextIndex{0};
    };
    struct BoundedStandardOptionValue {
        Type* elementType;
        bool isNone;
        std::optional<SInt> payload;
    };
    std::unordered_map<const Invoke*, BoundedStandardRangeIterator> standardRangeIterators;
    std::unordered_map<Value*, BoundedStandardOptionValue> standardOptionValues;
    const auto buildStandardRangeIterator = [this, &state, &loopBlocks, loopEntry](
                                                const Invoke* invoke)
        -> std::optional<BoundedStandardRangeIterator> {
        if (invoke == nullptr || invoke->GetObject() == nullptr) {
            return std::nullopt;
        }
        if (invoke->GetMethodName() != "next") {
            return std::nullopt;
        }
        auto iteratorExpression = GetDefiningExpr(invoke->GetObject());
        if (!IsKnownPureStandardRangeIteratorCall(iteratorExpression)) {
            return std::nullopt;
        }
        auto iteratorApply = StaticCast<const Apply*>(iteratorExpression);
        const auto& iteratorArgs = iteratorApply->GetArgs();
        auto rangeValue = iteratorArgs.front();
        auto rangeType = rangeValue == nullptr || rangeValue->GetType() == nullptr
            ? nullptr
            : rangeValue->GetType()->StripAllRefs();
        if (rangeType == nullptr || !rangeType->IsStruct()) {
            return std::nullopt;
        }
        auto structType = StaticCast<StructType*>(rangeType);
        auto structDef = structType->GetStructDef();
        if (structDef == nullptr || structDef->GetPackageName() != "std.core" ||
            structDef->GetSrcCodeIdentifier() != "Range") {
            return std::nullopt;
        }
        auto memberTypes = structType->GetInstantiatedMemberTys(builder);
        auto range = CaptureContextValue(state, rangeValue, /* preserveIntervals = */ true);
        std::optional<ContextAbstractValue> incomingRange;
        bool sawReachableEntry = false;
        bool completeEntryState = true;
        for (auto predecessor : loopEntry->GetPredecessors()) {
            if (loopBlocks.find(predecessor) != loopBlocks.end()) {
                continue;
            }
            auto edgeState = GetRecordedTerminatorEdgeState(predecessor->GetTerminator(), loopEntry);
            if (edgeState == nullptr) {
                completeEntryState = false;
                break;
            }
            if (edgeState->IsBottom()) {
                continue;
            }
            sawReachableEntry = true;
            auto candidate =
                CaptureContextValue(*edgeState, rangeValue, /* preserveIntervals = */ true);
            if (candidate.kind != ContextAbstractValue::Kind::OBJECT) {
                completeEntryState = false;
                break;
            }
            incomingRange = incomingRange.has_value()
                ? std::optional<ContextAbstractValue>{
                      JoinContextValues(incomingRange.value(), candidate)}
                : std::optional<ContextAbstractValue>{std::move(candidate)};
            if (incomingRange->kind != ContextAbstractValue::Kind::OBJECT) {
                completeEntryState = false;
                break;
            }
        }
        if (completeEntryState && sawReachableEntry && incomingRange.has_value()) {
            range = std::move(incomingRange.value());
        }
        auto rangeDefinition = GetDefiningExpr(rangeValue);
        if (rangeDefinition != nullptr && rangeDefinition->GetExprKind() == ExprKind::LOAD) {
            auto location = StaticCast<const Load*>(rangeDefinition)->GetLocation();
            auto global = DynamicCast<GlobalVar*>(location);
            if (global != nullptr && global->TestAttr(Attribute::READONLY)) {
                // SSA aggregate temporaries defined outside a loop are not
                // necessarily retained in the loop-header domain. A readonly
                // global cannot change after this load, so its abstract-memory
                // object is an equivalent, stable source for the Range fields.
                auto globalRange =
                    CaptureContextValue(state, global, /* preserveIntervals = */ true);
                if (globalRange.kind == ContextAbstractValue::Kind::OBJECT) {
                    range = std::move(globalRange);
                }
            }
        }
        if (range.kind != ContextAbstractValue::Kind::OBJECT || memberTypes.size() != 6 ||
            range.objectFields.size() != memberTypes.size() || memberTypes[0] == nullptr ||
            !memberTypes[0]->IsInteger()) {
            return std::nullopt;
        }
        const auto singleInteger = [](const ContextAbstractValue& value) -> std::optional<SInt> {
            if (value.kind != ContextAbstractValue::Kind::SINT || value.sintValue == nullptr ||
                !value.sintValue->IsSingleValue()) {
                return std::nullopt;
            }
            return value.sintValue->NumericBound().GetSingleElement();
        };
        const auto singleBool = [](const ContextAbstractValue& value) -> std::optional<bool> {
            if (value.kind != ContextAbstractValue::Kind::BOOL || !value.boolValue.has_value() ||
                !value.boolValue->IsSingleValue()) {
                return std::nullopt;
            }
            return value.boolValue->IsTrue();
        };
        auto start = singleInteger(range.objectFields[0]);
        auto end = singleInteger(range.objectFields[1]);
        auto stepValue = singleInteger(range.objectFields[2]);
        auto hasStart = singleBool(range.objectFields[3]);
        auto hasEnd = singleBool(range.objectFields[4]);
        auto isClosed = singleBool(range.objectFields[5]);
        if (!start.has_value() || !end.has_value() || !stepValue.has_value() ||
            !hasStart.value_or(false) || !hasEnd.value_or(false) || !isClosed.has_value() ||
            stepValue->SVal() == 0 || start->Width() != end->Width()) {
            return std::nullopt;
        }

        const bool isUnsigned = memberTypes[0]->IsUnsignedInteger();
        const auto width = start->Width();
        const __int128 minValue = isUnsigned ? 0 : -((static_cast<__int128>(1)) << (width - 1));
        const __int128 maxValue = isUnsigned
            ? ((static_cast<__int128>(1) << width) - 1)
            : ((static_cast<__int128>(1) << (width - 1)) - 1);
        __int128 current = isUnsigned ? static_cast<__int128>(start->UVal()) : start->SVal();
        const __int128 limit = isUnsigned ? static_cast<__int128>(end->UVal()) : end->SVal();
        const __int128 step = stepValue->SVal();
        const auto inRange = [step, limit, closed = isClosed.value()](__int128 value) {
            if (step > 0) {
                return closed ? value <= limit : value < limit;
            }
            return closed ? value >= limit : value > limit;
        };

        std::vector<SInt> values;
        while (inRange(current)) {
            if (values.size() >= MAX_BOUNDED_LOOP_STEPS || current < minValue || current > maxValue) {
                return std::nullopt;
            }
            values.emplace_back(width, static_cast<uint64_t>(current));
            auto next = current + step;
            if (next < minValue || next > maxValue) {
                // The concrete iterator updates cur after yielding the last
                // element. Overflow before the mathematical range ends is
                // not modeled by this bounded summary.
                if (inRange(next)) {
                    return std::nullopt;
                }
                break;
            }
            current = next;
        }
        return BoundedStandardRangeIterator{memberTypes[0], std::move(values), 0};
    };
    bool hasElementMemoryAccess = false;
    for (auto block : loopBlocks) {
        for (auto expression : block->GetNonTerminatorExpressions()) {
            if (isProjectedExpression(expression)) {
                continue;
            }
            bool isStandardRangeNext = false;
            if (expression->GetExprKind() == ExprKind::INVOKE) {
                auto invoke = StaticCast<const Invoke*>(expression);
                if (auto model = buildStandardRangeIterator(invoke); model.has_value()) {
                    standardRangeIterators.emplace(invoke, std::move(model.value()));
                    isStandardRangeNext = true;
                }
            }
            if (!isStandardRangeNext && !IsBoundedScalarLoopExpressionSupported(expression)) {
                if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                    const auto& location = expression->GetDebugLocation();
                    std::cerr << "[RangeAnalysisBoundedReject] reason=unsupported-expression kind="
                              << static_cast<int>(expression->GetExprKind())
                              << " line=" << location.GetBeginPos().line << '\n';
                    if (expression->GetExprKind() == ExprKind::LOAD) {
                        auto load = StaticCast<const Load*>(expression);
                        auto locationType = load->GetLocation()->GetType();
                        auto locationRootType = GetBoundedLoopRefRootType(load->GetLocation());
                        auto directLocationRootType = locationType != nullptr && locationType->IsRef()
                            ? StaticCast<RefType*>(locationType)->GetRootBaseType()
                            : nullptr;
                        auto resultType = load->GetResult()->GetType();
                        auto resultRootType = GetRefRootBaseType(resultType);
                        std::cerr << "[RangeAnalysisBoundedRejectLoad] location-type="
                                  << (locationType == nullptr ? "<null>" : locationType->ToString())
                                  << " location-root="
                                  << (locationRootType == nullptr ? "<null>" : locationRootType->ToString())
                                  << " direct-location-root="
                                  << (directLocationRootType == nullptr
                                          ? "<null>"
                                          : directLocationRootType->ToString())
                                  << " location-literal-info="
                                  << LookupStructArrayLiteralInfo(load->GetLocation()).has_value()
                                  << " result-type="
                                  << (resultType == nullptr ? "<null>" : resultType->ToString())
                                  << " result-root="
                                  << (resultRootType == nullptr ? "<null>" : resultRootType->ToString())
                                  << " result-users=" << load->GetResult()->GetUsers().size() << '\n';
                        for (auto user : load->GetResult()->GetUsers()) {
                            const auto& userLocation = user->GetDebugLocation();
                            std::cerr << "[RangeAnalysisBoundedRejectLoadUser] kind="
                                      << static_cast<int>(user->GetExprKind())
                                      << " line=" << userLocation.GetBeginPos().line << '\n';
                        }
                    }
                    if (expression->GetExprKind() == ExprKind::APPLY) {
                        auto apply = StaticCast<const Apply*>(expression);
                        const auto& args = apply->GetArgs();
                        auto receiver = args.empty() ? nullptr : args.front();
                        std::cerr << "[RangeAnalysisBoundedRejectApply] args=" << args.size()
                                  << " receiver-type="
                                  << (receiver == nullptr || receiver->GetType() == nullptr
                                          ? "<null>"
                                          : receiver->GetType()->ToString())
                                  << " receiver-literal-info="
                                  << LookupStructArrayLiteralInfo(receiver).has_value()
                                  << " getter="
                                  << IsProvenStructArrayIndexBody(apply, /* isSetter = */ false)
                                  << " setter="
                                  << IsProvenStructArrayIndexBody(apply, /* isSetter = */ true)
                                  << '\n';
                    }
                    if (expression->GetExprKind() == ExprKind::INVOKE) {
                        auto invoke = StaticCast<const Invoke*>(expression);
                        auto result = invoke->GetResult();
                        auto receiver = invoke->GetObject();
                        std::cerr << "[RangeAnalysisBoundedRejectInvoke] method="
                                  << invoke->GetMethodName()
                                  << " result-type="
                                  << (result == nullptr || result->GetType() == nullptr
                                          ? "<null>"
                                          : result->GetType()->ToString())
                                  << " receiver-type="
                                  << (receiver == nullptr || receiver->GetType() == nullptr
                                          ? "<null>"
                                          : receiver->GetType()->ToString())
                                  << " args=" << invoke->GetArgs().size();
                        for (auto argument : invoke->GetArgs()) {
                            std::cerr << " arg-type="
                                      << (argument == nullptr || argument->GetType() == nullptr
                                              ? "<null>"
                                              : argument->GetType()->ToString());
                        }
                        std::cerr << '\n';
                    }
                }
                return std::nullopt;
            }
            struct BoundedCallTarget {
                Function* function;
                ContextArguments arguments;
            };
            std::vector<BoundedCallTarget> callTargets;
            const auto captureCallArguments =
                [this, &state](const std::vector<Value*>& arguments, ClassType* receiverClass) {
                    ContextArguments capturedArguments;
                    capturedArguments.reserve(arguments.size());
                    for (size_t i = 0; i < arguments.size(); ++i) {
                        auto captured =
                            CaptureContextValue(state, arguments[i], /* preserveIntervals = */ true);
                        if (i == 0 && receiverClass != nullptr) {
                            if (captured.kind == ContextAbstractValue::Kind::OBJECT) {
                                captured.classValue = receiverClass;
                            } else {
                                captured = ContextAbstractValue{receiverClass};
                            }
                        }
                        capturedArguments.emplace_back(std::move(captured));
                    }
                    return capturedArguments;
                };
            size_t firstScalarArgument = 0;
            if (expression->GetExprKind() == ExprKind::APPLY) {
                auto apply = StaticCast<const Apply*>(expression);
                if (IsProvenBoundedStructArrayAccessor(apply)) {
                    hasElementMemoryAccess = true;
                    auto info = LookupStructArrayLiteralInfo(apply->GetArgs().front());
                    if (!info.has_value() || info->rawArray == nullptr ||
                        !seenArrayBackingValues.emplace(info->rawArray).second) {
                        if (!info.has_value() || info->rawArray == nullptr) {
                            traceReject("missing-array-backing");
                            return std::nullopt;
                        }
                    } else {
                        arrayBackingValues.emplace_back(info->rawArray);
                    }
                } else {
                    auto callee = DynamicCast<Function*>(apply->GetCallee());
                    if (!IsBoundedScalarCallTargetSupported(
                        apply->GetResult(), apply->GetArgs(), callee, firstScalarArgument)) {
                        return std::nullopt;
                    }
                    callTargets.emplace_back(BoundedCallTarget{
                        callee, captureCallArguments(apply->GetArgs(), nullptr)});
                }
            } else if (expression->GetExprKind() == ExprKind::INVOKE && !isStandardRangeNext) {
                auto invoke = StaticCast<const Invoke*>(expression);
                auto receiver = invoke->GetObject();
                auto receiverRootType =
                    receiver == nullptr ? nullptr : GetRefRootBaseType(receiver->GetType());
                if (receiverRootType == nullptr || !IsSupportedContextRootType(receiverRootType)) {
                    traceReject("unsupported-invoke-receiver");
                    return std::nullopt;
                }
                if (seenCallReceiverValues.emplace(receiver).second) {
                    callReceiverValues.emplace_back(receiver);
                }
                auto classes = ResolveFiniteClassSetForValue(invoke->GetObject());
                if (!classes.has_value() || classes->empty()) {
                    traceReject("unresolved-invoke-classes");
                    return std::nullopt;
                }
                firstScalarArgument = 1;
                for (auto exactClass : *classes) {
                    auto target = ResolveExactInvokeTarget(invoke, exactClass, builder);
                    if (!IsBoundedScalarCallTargetSupported(
                        invoke->GetResult(), invoke->GetArgs(), target, firstScalarArgument)) {
                        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                            auto trackedGlobals = target == nullptr
                                ? TrackedMutableGlobals{}
                                : CollectTrackedMutableGlobals(target);
                            std::cerr << "[RangeAnalysisBoundedInvokeTarget] class="
                                      << (exactClass == nullptr
                                              ? "<null>"
                                              : exactClass->ToString())
                                      << " target="
                                      << (target == nullptr
                                              ? "<null>"
                                              : target->GetIdentifier())
                                      << " body="
                                      << (target != nullptr && target->GetBody() != nullptr)
                                      << " params="
                                      << (target == nullptr ? 0 : target->GetParams().size())
                                      << " args=" << invoke->GetArgs().size()
                                      << " globals-complete=" << trackedGlobals.complete
                                      << " globals=" << trackedGlobals.size() << '\n';
                        }
                        traceReject("unsupported-invoke-target");
                        return std::nullopt;
                    }
                    callTargets.emplace_back(BoundedCallTarget{
                        target, captureCallArguments(invoke->GetArgs(), exactClass)});
                }
                if (callTargets.empty()) {
                    traceReject("empty-invoke-targets");
                    return std::nullopt;
                }
            }
            for (const auto& callTarget : callTargets) {
                auto trackedGlobals = CollectTrackedMutableGlobals(callTarget.function);
                std::optional<std::vector<GlobalVar*>> contextualGlobals;
                if (!trackedGlobals.complete) {
                    contextualGlobals =
                        CollectContextMutableGlobals(callTarget.function, callTarget.arguments);
                }
                if (!trackedGlobals.complete && !contextualGlobals.has_value()) {
                    traceReject("incomplete-context-call-effects");
                    return std::nullopt;
                }
                const auto collectGlobals = [&](const auto& globals) {
                    for (auto global : globals) {
                        if (seenReadLocations.emplace(global).second) {
                            readLocations.emplace_back(global);
                        }
                        if (seenMutatedLocations.emplace(global).second) {
                            mutatedLocations.emplace_back(global);
                            if (mutatedLocations.size() > MAX_BOUNDED_LOOP_LOCATIONS) {
                                return false;
                            }
                        }
                    }
                    return true;
                };
                if (trackedGlobals.complete) {
                    if (!collectGlobals(trackedGlobals.values)) {
                        return std::nullopt;
                    }
                } else if (!collectGlobals(contextualGlobals.value())) {
                    return std::nullopt;
                }
            }
            // A CONSTANT's operand is its LiteralValue payload, not a value
            // supplied by the surrounding CFG. Treating that payload as an
            // external input makes every literal loop bound look unknown.
            std::vector<Value*> operands;
            if (expression->GetExprKind() == ExprKind::APPLY) {
                operands = StaticCast<const Apply*>(expression)->GetArgs();
            } else if (expression->GetExprKind() != ExprKind::CONSTANT) {
                operands = expression->GetOperands();
            }
            for (auto operand : operands) {
                auto type = operand == nullptr ? nullptr : operand->GetType();
                if (type == nullptr || type->IsRef() || (!type->IsInteger() && !type->IsBoolean())) {
                    continue;
                }
                auto definingExpression = GetDefiningExpr(operand);
                auto definingBlock = definingExpression == nullptr ? nullptr : definingExpression->GetParentBlock();
                if (definingBlock == nullptr || loopBlocks.find(definingBlock) == loopBlocks.end()) {
                    const bool isConstant = type->IsInteger()
                        ? GetSignedConstantFromDefiningConstant(operand).has_value()
                        : GetSingleBoolFromDefiningConstant(operand).has_value();
                    if (isConstant ||
                        (definingExpression != nullptr && definingExpression->GetExprKind() == ExprKind::CONSTANT)) {
                        continue;
                    }
                    if (seenExternalValues.emplace(operand).second) {
                        externalValues.emplace_back(operand);
                    }
                }
            }
            Value* location = nullptr;
            if (expression->GetExprKind() == ExprKind::LOAD) {
                location = StaticCast<const Load*>(expression)->GetLocation();
            } else if (expression->GetExprKind() == ExprKind::STORE) {
                location = StaticCast<const Store*>(expression)->GetLocation();
            } else if (expression->GetExprKind() == ExprKind::GET_ELEMENT_REF ||
                expression->GetExprKind() == ExprKind::STORE_ELEMENT_REF) {
                hasElementMemoryAccess = true;
                auto elementLocation = expression->GetExprKind() == ExprKind::GET_ELEMENT_REF
                    ? StaticCast<const GetElementRef*>(expression)->GetLocation()
                    : StaticCast<const StoreElementRef*>(expression)->GetLocation();
                auto baseStorage = ResolveBoundedElementBaseStorage(elementLocation, loopBlocks);
                if (baseStorage == nullptr) {
                    traceReject("unknown-element-base-storage");
                    return std::nullopt;
                }
                if (seenElementBaseLocations.emplace(baseStorage).second) {
                    elementBaseLocations.emplace_back(baseStorage);
                }
            }
            if (location == nullptr) {
                continue;
            }
            if (expression->GetExprKind() == ExprKind::STORE &&
                IsDeadLoopLocalNullInitialization(StaticCast<const Store*>(expression), loopBlocks)) {
                continue;
            }
            if (GetBoundedLoopRefRootType(location) == nullptr ||
                IsLoopLocalElementLocation(location, loopBlocks)) {
                continue;
            }
            if (!HasOnlyDirectLoadStoreUsers(location)) {
                return std::nullopt;
            }
            if (seenReadLocations.emplace(location).second) {
                readLocations.emplace_back(location);
            }
            if (expression->GetExprKind() == ExprKind::STORE &&
                seenMutatedLocations.emplace(location).second) {
                mutatedLocations.emplace_back(location);
                if (mutatedLocations.size() > MAX_BOUNDED_LOOP_LOCATIONS) {
                    return std::nullopt;
                }
            }
        }
        auto terminator = block->GetTerminator();
        if (terminator == nullptr || (terminator->GetExprKind() != ExprKind::GOTO &&
            terminator->GetExprKind() != ExprKind::BRANCH &&
            terminator->GetExprKind() != ExprKind::MULTIBRANCH)) {
            return std::nullopt;
        }
    }
    if (mutatedLocations.empty()) {
        traceReject("no-mutated-locations");
        return std::nullopt;
    }
    std::vector<Value*> loopCarriedLocations;
    loopCarriedLocations.reserve(mutatedLocations.size());
    for (auto location : mutatedLocations) {
        if (!IsLoopLocalScalarLocation(location, loopBlocks)) {
            loopCarriedLocations.emplace_back(location);
        }
    }
    auto incomingEdgeState = [this, loopEntry](const Block* predecessor) {
        return predecessor == nullptr
            ? nullptr
            : GetRecordedTerminatorEdgeState(predecessor->GetTerminator(), loopEntry);
    };
    std::optional<RangeDomain> stableElementEntryState;
    if (hasElementMemoryAccess) {
        for (auto predecessor : loopEntry->GetPredecessors()) {
            if (loopBlocks.find(predecessor) != loopBlocks.end()) {
                continue;
            }
            auto edgeState = incomingEdgeState(predecessor);
            if (edgeState == nullptr) {
                traceReject("missing-element-entry-edge-state");
                return std::nullopt;
            }
            if (edgeState->IsBottom()) {
                continue;
            }
            if (!stableElementEntryState.has_value()) {
                stableElementEntryState = *edgeState;
            } else {
                stableElementEntryState->Join(*edgeState);
            }
        }
        if (!stableElementEntryState.has_value()) {
            traceReject("unreachable-element-entry");
            return std::nullopt;
        }
    }
    auto loopState = state;
    BoundedLoopObservationMap observedRanges;
    BoundedLoopCallContextMap observedCallContexts;
    std::unordered_set<const Expression*> invalidObservedExpressions;
    std::unordered_set<const Block*> takenExactSuccessors;
    std::unordered_set<const Block*> skippedExactSuccessors;
    for (auto baseStorage : elementBaseLocations) {
        std::optional<ContextAbstractValue> incomingObject;
        for (auto predecessor : loopEntry->GetPredecessors()) {
            if (loopBlocks.find(predecessor) != loopBlocks.end()) {
                continue;
            }
            auto edgeState = incomingEdgeState(predecessor);
            if (edgeState == nullptr || edgeState->IsBottom()) {
                continue;
            }
            auto candidate =
                CaptureContextValue(*edgeState, baseStorage, /* preserveIntervals = */ true);
            if (candidate.kind != ContextAbstractValue::Kind::OBJECT) {
                traceReject("top-element-base-entry");
                return std::nullopt;
            }
            if (!incomingObject.has_value()) {
                incomingObject = std::move(candidate);
                continue;
            }
            auto joined = JoinContextValues(incomingObject.value(), candidate);
            if (joined.kind != ContextAbstractValue::Kind::OBJECT) {
                traceReject("unknown-element-base-entry");
                return std::nullopt;
            }
            incomingObject = std::move(joined);
        }
        if (!incomingObject.has_value()) {
            traceReject("missing-element-base-entry");
            return std::nullopt;
        }
        ApplyContextValue(
            loopState, baseStorage, baseStorage->GetType(), incomingObject.value());
    }
    for (auto rawArray : arrayBackingValues) {
        auto entryObject = stableElementEntryState->CheckAbstractObjectRefBy(rawArray);
        auto loopObject = loopState.CheckAbstractObjectRefBy(rawArray);
        if (entryObject == nullptr || loopObject == nullptr) {
            traceReject("missing-array-entry-object");
            return std::nullopt;
        }
        auto entryElements = stableElementEntryState->GetChildren(entryObject);
        auto loopElements = loopState.GetChildren(loopObject);
        if (entryElements.empty() || entryElements.size() != loopElements.size()) {
            traceReject("invalid-array-entry-elements");
            return std::nullopt;
        }
        for (size_t i = 0; i < entryElements.size(); ++i) {
            auto entryValue =
                stableElementEntryState->CheckAbstractValueWithTopBottom(entryElements[i]);
            if (entryValue == nullptr ||
                (entryValue->GetKind() != RangeValueDomain::ValueKind::VAL &&
                    entryValue->GetKind() != RangeValueDomain::ValueKind::BOTTOM)) {
                traceReject("unknown-array-entry-element");
                return std::nullopt;
            }
            loopState.Update(loopElements[i], *entryValue);
        }
    }
    for (auto receiver : callReceiverValues) {
        std::optional<ContextAbstractValue> incomingReceiver;
        for (auto predecessor : loopEntry->GetPredecessors()) {
            if (loopBlocks.find(predecessor) != loopBlocks.end()) {
                continue;
            }
            auto edgeState = incomingEdgeState(predecessor);
            if (edgeState == nullptr || edgeState->IsBottom()) {
                continue;
            }
            auto candidate =
                CaptureContextValue(*edgeState, receiver, /* preserveIntervals = */ true);
            if (candidate.IsTop()) {
                traceReject("top-invoke-receiver-entry");
                return std::nullopt;
            }
            if (!incomingReceiver.has_value()) {
                incomingReceiver = std::move(candidate);
                continue;
            }
            auto joined = JoinContextValues(incomingReceiver.value(), candidate);
            if (joined.IsTop()) {
                traceReject("unknown-invoke-receiver-entry");
                return std::nullopt;
            }
            incomingReceiver = std::move(joined);
        }
        if (!incomingReceiver.has_value()) {
            auto candidate =
                CaptureContextValue(state, receiver, /* preserveIntervals = */ true);
            if (candidate.IsTop()) {
                traceReject("missing-invoke-receiver-entry");
                return std::nullopt;
            }
            incomingReceiver = std::move(candidate);
        }
        ApplyContextValue(loopState, receiver, receiver->GetType(), incomingReceiver.value());
    }
    auto observeExpression = [&](const RangeDomain& observationState, const Expression* expression) {
        if (expression == nullptr) {
            return;
        }
        Value* observedValue = expression->GetResult();
        if (expression->GetExprKind() == ExprKind::STORE) {
            observedValue = StaticCast<const Store*>(expression)->GetValue();
        }
        if (observedValue == nullptr) {
            return;
        }
        auto type = observedValue->GetType();
        if (type == nullptr || (!type->IsInteger() && !type->IsBoolean())) {
            return;
        }
        auto range = observationState.CheckAbstractValue(observedValue);
        if (range == nullptr) {
            observedRanges.erase(expression);
            invalidObservedExpressions.emplace(expression);
            return;
        }
        if (invalidObservedExpressions.find(expression) != invalidObservedExpressions.end()) {
            return;
        }
        auto current = observedRanges.find(expression);
        if (current == observedRanges.end()) {
            observedRanges.emplace(expression, range->Clone());
        } else if (auto joined = JoinBoundedLoopObservationRanges(*current->second, *range); joined.has_value()) {
            current->second = std::move(joined.value());
        }
    };
    for (auto location : loopCarriedLocations) {
        auto rootType = GetBoundedLoopRefRootType(location);
        auto object = loopState.CheckAbstractObjectRefBy(location);
        if (rootType == nullptr || object == nullptr || IsBoundedFlatAggregateType(rootType)) {
            traceReject("unsupported-loop-carried-location");
            return std::nullopt;
        }
        std::optional<bool> entryBoolValue;
        std::optional<std::vector<SInt>> entryIntegerValues;
        if (auto global = DynamicCast<GlobalVar*>(location); global != nullptr) {
            bool foundContextGlobal = false;
            if (isContextAnalysis) {
                for (const auto& [contextGlobal, value] : contextGlobalArguments) {
                    if (contextGlobal != global) {
                        continue;
                    }
                    foundContextGlobal = true;
                    if (rootType->IsBoolean() && value.kind == ContextAbstractValue::Kind::BOOL &&
                        value.boolValue.has_value() && value.boolValue->IsSingleValue()) {
                        entryBoolValue = value.boolValue->IsTrue();
                    } else if (rootType->IsInteger() && value.kind == ContextAbstractValue::Kind::SINT &&
                        value.sintValue != nullptr && value.sintValue->IsSingleValue()) {
                        entryIntegerValues = std::vector<SInt>{
                            value.sintValue->NumericBound().GetSingleElement()};
                    }
                    break;
                }
            }
            if (!foundContextGlobal) {
                auto initializer = global->GetInitializer();
                if (initializer != nullptr && rootType->IsBoolean() && initializer->IsBoolLiteral()) {
                    entryBoolValue = StaticCast<BoolLiteral*>(initializer)->GetVal();
                } else if (initializer != nullptr && rootType->IsInteger() && initializer->IsIntLiteral()) {
                    auto initialDomain = SIntDomain::From(*initializer);
                    if (initialDomain.IsSingleValue()) {
                        entryIntegerValues = std::vector<SInt>{
                            initialDomain.NumericBound().GetSingleElement()};
                    }
                }
            }
        }
        if (rootType->IsBoolean()) {
            auto incoming = FindIncomingBoolStoreValue(
                loopState, loopEntry, location, object, loopBlocks, entryBoolValue, incomingEdgeState);
            if (!incoming.has_value()) {
                traceReject("unknown-loop-entry-bool");
                return std::nullopt;
            }
            loopState.Update(object, std::make_unique<BoolRange>(BoolDomain::FromBool(incoming.value())));
            continue;
        }
        auto incoming = FindIncomingSmallIntegerStoreValues(
            loopState, loopEntry, location, object, rootType, loopBlocks, entryIntegerValues, incomingEdgeState);
        if (!incoming.has_value() || incoming->size() != 1) {
            traceReject("unknown-loop-entry-integer");
            return std::nullopt;
        }
        auto exact = BuildIntegerExactRange(rootType, std::move(incoming.value()));
        if (!exact.has_value()) {
            traceReject("invalid-loop-entry-integer");
            return std::nullopt;
        }
        loopState.Update(object, std::make_unique<SIntRange>(std::move(exact.value())));
    }

    std::vector<Value*> callPreservedScalarLocations;
    std::unordered_set<Value*> seenCallPreservedLocations;
    const auto collectCallPreservedLocation = [&](Value* location) {
        auto rootType = GetBoundedLoopRefRootType(location);
        if (location == nullptr || location->IsGlobal() || rootType == nullptr ||
            (!rootType->IsInteger() && !rootType->IsBoolean()) ||
            !HasOnlyDirectLoadStoreUsers(location) ||
            !seenCallPreservedLocations.emplace(location).second) {
            return;
        }
        callPreservedScalarLocations.emplace_back(location);
    };
    for (auto location : readLocations) {
        collectCallPreservedLocation(location);
    }
    for (auto location : mutatedLocations) {
        collectCallPreservedLocation(location);
    }
    std::vector<Value*> callPreservedScalarValues;
    std::unordered_set<Value*> seenCallPreservedValues;
    std::unordered_set<Value*> callProducedScalarValues;
    for (auto callExpression : loopCallExpressions) {
        if (callExpression != nullptr && callExpression->GetResult() != nullptr) {
            callProducedScalarValues.emplace(callExpression->GetResult());
        }
    }
    const auto collectCallPreservedValue = [&](Value* value) {
        auto type = value == nullptr ? nullptr : value->GetType();
        if (type != nullptr && !type->IsRef() && (type->IsInteger() || type->IsBoolean()) &&
            callProducedScalarValues.find(value) == callProducedScalarValues.end() &&
            seenCallPreservedValues.emplace(value).second) {
            callPreservedScalarValues.emplace_back(value);
        }
    };
    for (auto expression : loopExpressions) {
        if (expression->GetExprKind() != ExprKind::APPLY &&
            expression->GetExprKind() != ExprKind::INVOKE) {
            collectCallPreservedValue(expression->GetResult());
        }
        if (expression->GetExprKind() == ExprKind::CONSTANT) {
            continue;
        }
        for (auto operand : expression->GetOperands()) {
            collectCallPreservedValue(operand);
        }
    }
    for (auto value : externalValues) {
        collectCallPreservedValue(value);
    }

    std::sort(readLocations.begin(), readLocations.end());
    std::sort(externalValues.begin(), externalValues.end());
    const bool cacheableLoop = !hasElementMemoryAccess && std::none_of(
        loopCallExpressions.begin(), loopCallExpressions.end(), [](const Expression* expression) {
            return expression->GetExprKind() == ExprKind::INVOKE;
        });
    std::stringstream cacheKey;
    cacheKey << branch << ":" << successor;
    for (auto location : readLocations) {
        if (IsLoopLocalScalarLocation(location, loopBlocks)) {
            continue;
        }
        auto rootType = GetBoundedLoopRefRootType(location);
        auto object = loopState.CheckAbstractObjectRefBy(location);
        if (IsBoundedFlatAggregateType(rootType)) {
            traceReject("external-aggregate-location");
            return std::nullopt;
        }
        const bool isSingle = rootType != nullptr && object != nullptr && (rootType->IsBoolean()
            ? GetBoolDomainFromStateWithType(loopState, object, rootType).IsSingleValue()
            : ::Cangjie::CHIR::GetSIntDomainFromState(loopState, object, rootType).IsSingleValue());
        if (!isSingle) {
            traceReject("non-single-read-location");
            return std::nullopt;
        }
        auto value = CaptureContextValue(loopState, object, rootType, /* preserveIntervals = */ true);
        if (value.IsTop()) {
            traceReject("top-read-location");
            return std::nullopt;
        }
        cacheKey << ":mem@" << location << "=" << value.ToKeyString(rootType);
    }
    for (auto value : externalValues) {
        auto type = value->GetType();
        auto captured = CaptureContextValue(loopState, value, type, /* preserveIntervals = */ true);
        auto isSingle = type->IsBoolean()
            ? GetBoolDomainFromState(loopState, value).IsSingleValue()
            : GetSIntDomainFromState(loopState, value).IsSingleValue();
        if (!isSingle || captured.IsTop()) {
            traceReject("non-single-external-value");
            return std::nullopt;
        }
        cacheKey << ":value@" << value << "=" << captured.ToKeyString(type);
    }
    for (auto receiver : callReceiverValues) {
        auto type = receiver == nullptr ? nullptr : receiver->GetType();
        auto captured =
            CaptureContextValue(loopState, receiver, /* preserveIntervals = */ true);
        if (type == nullptr || captured.IsTop()) {
            traceReject("top-cache-receiver");
            return std::nullopt;
        }
        cacheKey << ":receiver@" << receiver << "=" << captured.ToKeyString(type);
    }
    auto key = cacheKey.str();
    if (publishPrefixOnBudget) {
        key += ":prefix";
        std::lock_guard<std::mutex> lock(boundedLoopObservationsMtx);
        auto owner = completedBoundedLoopPrefixAttempts.find(this);
        if (owner != completedBoundedLoopPrefixAttempts.end() &&
            owner->second.find(key) != owner->second.end()) {
            return std::nullopt;
        }
    }
    std::optional<ContextLocationValues> cachedSummary;
    if (cacheableLoop && !publishPrefixOnBudget) {
        std::lock_guard<std::mutex> lock(GetBoundedLoopExitCacheMutex());
        auto ownerCache = GetBoundedLoopExitCaches().find(this);
        if (ownerCache != GetBoundedLoopExitCaches().end()) {
            auto cached = ownerCache->second.find(key);
            if (cached != ownerCache->second.end()) {
                cachedSummary = cached->second;
            } else if (ownerCache->second.size() >= MAX_BOUNDED_LOOP_CACHE_ENTRIES) {
                return std::nullopt;
            }
        }
    }
    if (cachedSummary.has_value()) {
        auto result = state;
        for (const auto& [location, value] : cachedSummary.value()) {
            auto rootType = GetBoundedLoopRefRootType(location);
            auto object = result.CheckAbstractObjectRefBy(location);
            if (rootType == nullptr || object == nullptr || IsBoundedFlatAggregateType(rootType)) {
                return std::nullopt;
            }
            ApplyContextValue(result, object, rootType, value);
        }
        havocProjectedLocations(result);
        boundedContextAttempt.MarkSucceeded();
        return result;
    }
    if (failedBoundedLoopContextAttempts.find(key) !=
        failedBoundedLoopContextAttempts.end()) {
        traceReject("cached-context-budget-failure");
        return std::nullopt;
    }

    struct FailedContextAttemptGuard {
        FailedContextAttemptGuard(std::unordered_set<std::string>& failedKeys,
            const std::string& key, size_t initialBudgetRejects, size_t limit)
            : failedKeys(failedKeys), key(key), initialBudgetRejects(initialBudgetRejects),
              limit(limit)
        {
        }

        ~FailedContextAttemptGuard()
        {
            if (!succeeded &&
                (contextAnalysisBudgetExhausted.load(std::memory_order_relaxed) ||
                    contextSummaryBudgetRejectCount > initialBudgetRejects) &&
                failedKeys.size() < limit) {
                failedKeys.emplace(key);
            }
        }

        void MarkSucceeded()
        {
            succeeded = true;
        }

        std::unordered_set<std::string>& failedKeys;
        const std::string& key;
        size_t initialBudgetRejects;
        size_t limit;
        bool succeeded{false};
    } failedContextAttempt{failedBoundedLoopContextAttempts, key,
        contextSummaryBudgetRejectCount, MAX_BOUNDED_LOOP_CACHE_ENTRIES};

    auto cacheResultState = [&](const RangeDomain& resultState) -> std::optional<RangeDomain> {
        ContextLocationValues summary;
        summary.reserve(loopCarriedLocations.size());
        for (auto location : loopCarriedLocations) {
            auto rootType = GetBoundedLoopRefRootType(location);
            auto object = resultState.CheckAbstractObjectRefBy(location);
            if (rootType == nullptr || object == nullptr || IsBoundedFlatAggregateType(rootType)) {
                return std::nullopt;
            }
            auto value = CaptureContextValue(resultState, object, rootType, /* preserveIntervals = */ true);
            if (value.IsTop()) {
                return std::nullopt;
            }
            summary.emplace_back(location, std::move(value));
        }

        std::optional<RangeDomain> normalizedElementResult;
        if (!elementBaseLocations.empty()) {
            if (!stableElementEntryState.has_value()) {
                return std::nullopt;
            }
            ContextLocationValues elementSummary;
            elementSummary.reserve(elementBaseLocations.size());
            for (auto baseStorage : elementBaseLocations) {
                auto value =
                    CaptureContextValue(resultState, baseStorage, /* preserveIntervals = */ true);
                if (value.kind != ContextAbstractValue::Kind::OBJECT) {
                    traceReject("top-element-exit-summary");
                    return std::nullopt;
                }
                elementSummary.emplace_back(baseStorage, std::move(value));
            }

            normalizedElementResult = stableElementEntryState.value();
            for (const auto& [location, value] : summary) {
                auto rootType = GetBoundedLoopRefRootType(location);
                auto object = normalizedElementResult->CheckAbstractObjectRefBy(location);
                if (rootType == nullptr || object == nullptr) {
                    traceReject("missing-stable-loop-location");
                    return std::nullopt;
                }
                ApplyContextValue(*normalizedElementResult, object, rootType, value);
            }
            for (const auto& [baseStorage, value] : elementSummary) {
                auto stableObject = normalizedElementResult->CheckAbstractObjectRefBy(baseStorage);
                if (stableObject == nullptr ||
                    GetContextObjectIdentity(stableObject) != value.objectIdentity) {
                    traceReject("unstable-element-exit-identity");
                    return std::nullopt;
                }
                ApplyContextValue(
                    *normalizedElementResult, baseStorage, baseStorage->GetType(), value);
            }

            std::unordered_set<Value*> copiedScalarValues;
            const auto copyScalarValue = [&](Value* value) {
                auto type = value == nullptr ? nullptr : value->GetType();
                if (type == nullptr || type->IsRef() ||
                    (!type->IsInteger() && !type->IsBoolean()) ||
                    !copiedScalarValues.emplace(value).second) {
                    return;
                }
                auto captured =
                    CaptureContextValue(resultState, value, /* preserveIntervals = */ true);
                if (captured.IsTop()) {
                    normalizedElementResult->SetToTopOrTopRef(value, /* isRef = */ false);
                } else {
                    ApplyContextValue(*normalizedElementResult, value, type, captured);
                }
            };
            for (auto expression : loopExpressions) {
                copyScalarValue(expression->GetResult());
                for (auto operand : expression->GetOperands()) {
                    copyScalarValue(operand);
                }
            }
            for (auto block : loopBlocks) {
                auto terminator = block->GetTerminator();
                copyScalarValue(terminator == nullptr ? nullptr : terminator->GetResult());
                if (terminator == nullptr) {
                    continue;
                }
                for (auto operand : terminator->GetOperands()) {
                    copyScalarValue(operand);
                }
            }
        }

        if (evaluateExitEdge) {
            constexpr size_t MAX_BOUNDED_EXIT_OBSERVATION_BLOCKS = 32;
            constexpr size_t MAX_BOUNDED_EXIT_OBSERVATION_EXPRESSIONS = 256;
            const auto traceExitObservation = [](const char* reason, const Block* block) {
                if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") == nullptr) {
                    return;
                }
                std::cerr << "[RangeAnalysisBoundedExitObservation] reason=" << reason
                          << " block=" << (block == nullptr ? "<null>" : block->GetIdentifier()) << '\n';
            };
            std::unordered_map<const Block*, bool> skippedPathMemo;
            std::unordered_set<const Block*> skippedPathVisiting;
            std::function<bool(const Block*)> isProvenSkippedPath =
                [&](const Block* block) -> bool {
                    if (block == nullptr) {
                        return false;
                    }
                    if (auto memo = skippedPathMemo.find(block);
                        memo != skippedPathMemo.end()) {
                        return memo->second;
                    }
                    if (skippedExactSuccessors.find(block) != skippedExactSuccessors.end() &&
                        takenExactSuccessors.find(block) == takenExactSuccessors.end()) {
                        skippedPathMemo.emplace(block, true);
                        return true;
                    }
                    if (loopBlocks.find(block) != loopBlocks.end() ||
                        !skippedPathVisiting.emplace(block).second) {
                        return false;
                    }
                    const auto& predecessors = block->GetPredecessors();
                    const bool skipped = !predecessors.empty() &&
                        std::all_of(predecessors.begin(), predecessors.end(),
                            [&isProvenSkippedPath](const Block* predecessor) {
                                return isProvenSkippedPath(predecessor);
                            });
                    skippedPathVisiting.erase(block);
                    skippedPathMemo.emplace(block, skipped);
                    return skipped;
                };
            auto successorState = normalizedElementResult.has_value()
                ? normalizedElementResult.value()
                : resultState;
            const Block* currentBlock = exitSuccessor;
            const Block* expectedPredecessor = header;
            std::unordered_set<const Block*> visitedBlocks;
            size_t observedExpressionCount = 0;
            for (size_t blockCount = 0;
                currentBlock != nullptr && blockCount < MAX_BOUNDED_EXIT_OBSERVATION_BLOCKS;
                ++blockCount) {
                if (!visitedBlocks.emplace(currentBlock).second ||
                    loopBlocks.find(currentBlock) != loopBlocks.end()) {
                    traceExitObservation("cycle-or-loop", currentBlock);
                    break;
                }
                const auto& predecessors = currentBlock->GetPredecessors();
                const bool isFirstExitBlock = currentBlock == exitSuccessor;
                const bool hasCompleteLoopPredecessors = isFirstExitBlock &&
                    !predecessors.empty() &&
                    std::all_of(predecessors.begin(), predecessors.end(),
                        [&loopBlocks, &isProvenSkippedPath](const Block* predecessor) {
                            return loopBlocks.find(predecessor) != loopBlocks.end() ||
                                isProvenSkippedPath(predecessor);
                        });
                if ((!isFirstExitBlock &&
                        (predecessors.size() != 1 || predecessors.front() != expectedPredecessor)) ||
                    (isFirstExitBlock && !hasCompleteLoopPredecessors)) {
                    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                        std::cerr << "[RangeAnalysisBoundedExitPredecessors] block="
                                  << currentBlock->GetIdentifier()
                                  << " expected="
                                  << (expectedPredecessor == nullptr
                                          ? "<null>"
                                          : expectedPredecessor->GetIdentifier());
                        for (auto predecessor : predecessors) {
                            auto predecessorTerminator = predecessor->GetTerminator();
                            const auto& predecessorLocation =
                                predecessorTerminator->GetDebugLocation();
                            std::cerr << " pred=" << predecessor->GetIdentifier()
                                      << ":in-loop="
                                      << (loopBlocks.find(predecessor) != loopBlocks.end())
                                      << ":reaches-header="
                                      << CanReachBlockForWidening(predecessor, header)
                                      << ":term-kind="
                                      << static_cast<int>(predecessorTerminator->GetExprKind())
                                      << ":term-line="
                                      << predecessorLocation.GetBeginPos().line;
                            for (auto parent : predecessor->GetPredecessors()) {
                                auto parentTerminator = parent->GetTerminator();
                                const auto& parentLocation =
                                    parentTerminator->GetDebugLocation();
                                std::cerr << ":parent=" << parent->GetIdentifier()
                                          << ":parent-in-loop="
                                          << (loopBlocks.find(parent) != loopBlocks.end())
                                          << ":parent-kind="
                                          << static_cast<int>(parentTerminator->GetExprKind())
                                          << ":parent-line="
                                          << parentLocation.GetBeginPos().line;
                            }
                            for (auto expression : predecessor->GetNonTerminatorExpressions()) {
                                const auto& expressionLocation = expression->GetDebugLocation();
                                std::cerr << ":expr-kind="
                                          << static_cast<int>(expression->GetExprKind())
                                          << ":expr-line="
                                          << expressionLocation.GetBeginPos().line;
                            }
                        }
                        std::cerr << '\n';
                    }
                    traceExitObservation("non-unique-predecessor", currentBlock);
                    break;
                }
                auto expressions = currentBlock->GetNonTerminatorExpressions();
                const bool supportedBlock = std::all_of(
                    expressions.begin(), expressions.end(), [](const Expression* expression) {
                        if (expression == nullptr || expression->GetExprKind() == ExprKind::INVOKE) {
                            return false;
                        }
                        if (expression->GetExprKind() == ExprKind::APPLY) {
                            return IsProvenBoundedStructArrayAccessor(
                                StaticCast<const Apply*>(expression));
                        }
                        return IsBoundedScalarLoopExpressionSupported(expression);
                    });
                if (!supportedBlock ||
                    expressions.size() >
                        MAX_BOUNDED_EXIT_OBSERVATION_EXPRESSIONS - observedExpressionCount) {
                    if (!supportedBlock && std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                        for (auto expression : expressions) {
                            const bool isSupportedApply = expression != nullptr &&
                                expression->GetExprKind() == ExprKind::APPLY &&
                                IsProvenBoundedStructArrayAccessor(
                                    StaticCast<const Apply*>(expression));
                            if (isSupportedApply ||
                                (expression != nullptr &&
                                    expression->GetExprKind() != ExprKind::APPLY &&
                                    expression->GetExprKind() != ExprKind::INVOKE &&
                                    IsBoundedScalarLoopExpressionSupported(expression))) {
                                continue;
                            }
                            if (expression == nullptr) {
                                std::cerr << "[RangeAnalysisBoundedExitUnsupported] kind=<null> line=0\n";
                                break;
                            }
                            const auto& location = expression->GetDebugLocation();
                            std::cerr << "[RangeAnalysisBoundedExitUnsupported] kind="
                                      << static_cast<int>(expression->GetExprKind())
                                      << " line=" << location.GetBeginPos().line << '\n';
                            break;
                        }
                    }
                    traceExitObservation(
                        supportedBlock ? "expression-budget" : "unsupported-expression", currentBlock);
                    break;
                }
                for (auto expression : expressions) {
                    PropagateExpressionEffect(successorState, expression);
                    if (successorState.IsBottom()) {
                        break;
                    }
                    observeExpression(successorState, expression);
                    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                        const auto& location = expression->GetDebugLocation();
                        std::cerr << "[RangeAnalysisBoundedExitStep] block="
                                  << currentBlock->GetIdentifier()
                                  << " kind=" << static_cast<int>(expression->GetExprKind())
                                  << " line=" << location.GetBeginPos().line << '\n';
                    }
                    if (std::find(loopExpressions.begin(), loopExpressions.end(), expression) ==
                        loopExpressions.end()) {
                        loopExpressions.emplace_back(expression);
                    }
                    ++observedExpressionCount;
                }
                if (successorState.IsBottom()) {
                    break;
                }
                auto terminator = currentBlock->GetTerminator();
                if (terminator == nullptr) {
                    traceExitObservation("missing-terminator", currentBlock);
                    break;
                }
                Block* nextBlock = nullptr;
                if (terminator->GetExprKind() == ExprKind::GOTO) {
                    nextBlock = StaticCast<const GoTo*>(terminator)->GetDestination();
                } else if (terminator->GetExprKind() == ExprKind::BRANCH) {
                    auto exitBranch = StaticCast<const Branch*>(terminator);
                    auto condition =
                        GetBoolDomainFromState(successorState, exitBranch->GetCondition());
                    if (!condition.IsSingleValue()) {
                        traceExitObservation("non-single-branch", currentBlock);
                        break;
                    }
                    const bool branchCondition = condition.IsTrue();
                    nextBlock = branchCondition
                        ? exitBranch->GetTrueBlock()
                        : exitBranch->GetFalseBlock();
                    if (!ApplyConditionConstraint(
                            successorState, exitBranch->GetCondition(), branchCondition)) {
                        traceExitObservation("inconsistent-branch", currentBlock);
                        break;
                    }
                } else if (terminator->GetExprKind() == ExprKind::MULTIBRANCH) {
                    nextBlock = HandleMultiBranchTerminator(
                        successorState, StaticCast<const MultiBranch*>(terminator)).value_or(nullptr);
                    if (nextBlock == nullptr) {
                        traceExitObservation("non-single-multibranch", currentBlock);
                        break;
                    }
                } else {
                    traceExitObservation("unsupported-terminator", currentBlock);
                    break;
                }
                if (nextBlock == nullptr) {
                    traceExitObservation("missing-successor", currentBlock);
                    break;
                }
                takenExactSuccessors.emplace(nextBlock);
                for (auto candidate : currentBlock->GetSuccessors()) {
                    if (candidate != nextBlock) {
                        skippedExactSuccessors.emplace(candidate);
                    }
                }
                expectedPredecessor = currentBlock;
                currentBlock = nextBlock;
            }
        }

        auto result = normalizedElementResult.has_value()
            ? std::move(normalizedElementResult.value())
            : resultState;
        havocProjectedLocations(result);

        // Publish observations only after every loop-carried location has a
        // complete exit summary. A failed concrete evaluation must not leave
        // a prefix of its observations visible to contest-query aggregation.
        if (isContextAnalysis) {
            for (const auto& [expression, range] : observedRanges) {
                if (expression == nullptr || range == nullptr ||
                    incompleteLocalBoundedLoopObservations.find(expression) !=
                        incompleteLocalBoundedLoopObservations.end()) {
                    continue;
                }
                auto current = localBoundedLoopObservations.find(expression);
                if (current == localBoundedLoopObservations.end()) {
                    localBoundedLoopObservations.emplace(expression, range->Clone());
                } else if (auto joined = JoinBoundedLoopObservationRanges(*current->second, *range);
                    joined.has_value()) {
                    current->second = std::move(joined.value());
                }
            }
        } else {
            CommitBoundedLoopObservations(observedRanges);
        }
        if (cacheableLoop) {
            std::lock_guard<std::mutex> lock(GetBoundedLoopExitCacheMutex());
            auto& ownerCache = GetBoundedLoopExitCaches()[this];
            if (ownerCache.size() < MAX_BOUNDED_LOOP_CACHE_ENTRIES) {
                ownerCache.emplace(key, std::move(summary));
            }
        }
        if (parentCallContextRecorder != nullptr) {
            MergeBoundedLoopCallContexts(*parentCallContextRecorder, observedCallContexts);
        } else {
            CommitBoundedLoopCallContexts(analysisContext, observedCallContexts);
        }
        boundedContextAttempt.MarkSucceeded();
        failedContextAttempt.MarkSucceeded();
        return result;
    };

    BoundedAggregateStoreMap concreteAggregateStores;
    struct EvaluationGuard {
        EvaluationGuard(const RangeAnalysis* owner, BoundedLoopCallContextMap* callContexts,
            BoundedAggregateStoreMap* aggregateStores)
            : previous(boundedLoopEvaluationOwner), previousCallContexts(boundedLoopCallContextRecorder),
              previousAggregateStores(boundedAggregateStores)
        {
            boundedLoopEvaluationOwner = owner;
            boundedLoopCallContextRecorder = callContexts;
            boundedAggregateStores = aggregateStores;
        }
        ~EvaluationGuard()
        {
            boundedLoopEvaluationOwner = previous;
            boundedLoopCallContextRecorder = previousCallContexts;
            boundedAggregateStores = previousAggregateStores;
        }
        const RangeAnalysis* previous;
        BoundedLoopCallContextMap* previousCallContexts;
        BoundedAggregateStoreMap* previousAggregateStores;
    } guard(this, &observedCallContexts, &concreteAggregateStores);

    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
        const auto& location = branch->GetDebugLocation();
        std::cerr << "[RangeAnalysisBoundedExecute] loop-line=" << location.GetBeginPos().line
                  << " blocks=" << loopBlocks.size()
                  << " locations=" << mutatedLocations.size() << '\n';
        if (qgsrProjection.has_value() && !qgsrProjection->ignoredExpressions.empty()) {
            std::cerr << "[RangeAnalysisQGSR] projectedLoop line=" << location.GetBeginPos().line
                      << " skippedExpressions=" << qgsrProjection->ignoredExpressions.size() << '\n';
        }
    }
    auto current = loopEntry;
    size_t applyEvaluations = 0;
    size_t invokeEvaluations = 0;
    std::unordered_set<std::string> uniqueApplyContexts;
    std::unordered_set<std::string> uniqueInvokeContexts;
    std::optional<RangeDomain> observedSuccessorState;
    for (size_t step = 0; step < stepBudget; ++step) {
        if (current == exitSuccessor) {
            if (evaluateExitEdge) {
                return cacheResultState(loopState);
            }
            return observedSuccessorState.has_value()
                ? cacheResultState(observedSuccessorState.value())
                : std::nullopt;
        }
        if (!evaluateExitEdge && current == successor) {
            if (!observedSuccessorState.has_value()) {
                observedSuccessorState = loopState;
            } else {
                observedSuccessorState->Join(loopState);
            }
        }
        if (current == nullptr || loopBlocks.find(current) == loopBlocks.end()) {
            return std::nullopt;
        }
        for (auto expression : current->GetNonTerminatorExpressions()) {
            if (isProjectedExpression(expression)) {
                continue;
            }
            const bool isApplyExpression = expression->GetExprKind() == ExprKind::APPLY;
            const bool isInvokeExpression = expression->GetExprKind() == ExprKind::INVOKE;
            const bool isCallExpression = isApplyExpression || isInvokeExpression;
            if ((isApplyExpression &&
                    applyEvaluations >= MAX_BOUNDED_LOOP_APPLY_EVALUATIONS) ||
                (isInvokeExpression &&
                    invokeEvaluations >= MAX_BOUNDED_LOOP_INVOKE_EVALUATIONS)) {
                if (failedBoundedLoopContextAttempts.size() <
                    MAX_BOUNDED_LOOP_CACHE_ENTRIES) {
                    failedBoundedLoopContextAttempts.emplace(key);
                }
                traceReject("call-evaluation-budget");
                return std::nullopt;
            }
            applyEvaluations += static_cast<size_t>(isApplyExpression);
            invokeEvaluations += static_cast<size_t>(isInvokeExpression);
            if (isInvokeExpression && IsRangeAnalysisPerfCollectionEnabled()) {
                ++rangeAnalysisPerfStats.boundedInvokeReplays;
            }
            if (expression->GetExprKind() == ExprKind::ALLOCATE) {
                auto rootType = GetBoundedLoopRefRootType(expression->GetResult());
                if (IsBoundedFlatAggregateType(rootType)) {
                    concreteAggregateStores.erase(expression->GetResult());
                }
            }
            ContextLocationValues preservedCallState;
            ContextLocationValues preservedCallValues;
            if (expression->GetExprKind() == ExprKind::INVOKE) {
                preservedCallState.reserve(callPreservedScalarLocations.size());
                for (auto location : callPreservedScalarLocations) {
                    auto rootType = GetBoundedLoopRefRootType(location);
                    auto object = loopState.CheckAbstractObjectRefBy(location);
                    auto value = CaptureContextValue(
                        loopState, object, rootType, /* preserveIntervals = */ true);
                    if (!value.IsTop()) {
                        preservedCallState.emplace_back(location, std::move(value));
                    }
                }
                preservedCallValues.reserve(callPreservedScalarValues.size());
                for (auto value : callPreservedScalarValues) {
                    auto captured = CaptureContextValue(
                        loopState, value, /* preserveIntervals = */ true);
                    if (!captured.IsTop()) {
                        preservedCallValues.emplace_back(value, std::move(captured));
                    }
                }
            }
            bool handledStandardRangeExpression = false;
            if (isInvokeExpression) {
                auto invoke = StaticCast<const Invoke*>(expression);
                auto model = standardRangeIterators.find(invoke);
                if (model != standardRangeIterators.end()) {
                    auto& iterator = model->second;
                    const bool hasValue = iterator.nextIndex < iterator.values.size();
                    std::optional<SInt> payload;
                    if (hasValue) {
                        payload = iterator.values[iterator.nextIndex++];
                    }
                    auto result = invoke->GetResult();
                    loopState.SetToBound(result, /* isTop = */ true);
                    loopState.EnsureChildren(result, 1, [&loopState](AbstractObject* child, size_t) {
                        loopState.SetToTopOrTopRef(child, /* isRef = */ false);
                    });
                    auto children = loopState.GetChildren(result);
                    if (children.size() != 1) {
                        traceReject("range-option-layout");
                        return std::nullopt;
                    }
                    loopState.Update(children.front(),
                        std::make_unique<BoolRange>(BoolDomain::FromBool(!hasValue)));
                    standardOptionValues.insert_or_assign(result,
                        BoundedStandardOptionValue{iterator.elementType, !hasValue, std::move(payload)});
                    handledStandardRangeExpression = true;
                }
            } else if (expression->GetExprKind() == ExprKind::TYPECAST) {
                auto cast = StaticCast<const TypeCast*>(expression);
                auto option = standardOptionValues.find(cast->GetSourceValue());
                if (option != standardOptionValues.end() && !option->second.isNone &&
                    option->second.payload.has_value() && cast->GetTargetTy()->IsTuple()) {
                    auto elementTypes = StaticCast<TupleType*>(cast->GetTargetTy())->GetElementTypes();
                    if (elementTypes.size() != 2 || !elementTypes[0]->IsBoolean() ||
                        !elementTypes[1]->IsInteger()) {
                        traceReject("range-option-cast-layout");
                        return std::nullopt;
                    }
                    auto result = cast->GetResult();
                    loopState.SetToBound(result, /* isTop = */ true);
                    loopState.EnsureChildren(result, elementTypes.size(),
                        [&loopState, &elementTypes](AbstractObject* child, size_t index) {
                            loopState.SetToTopOrTopRef(child, elementTypes[index]->IsRef());
                        });
                    auto children = loopState.GetChildren(result);
                    if (children.size() != elementTypes.size()) {
                        traceReject("range-option-cast-children");
                        return std::nullopt;
                    }
                    loopState.Update(children[0],
                        std::make_unique<BoolRange>(BoolDomain::FromBool(false)));
                    auto payloadRange = BuildIntegerExactRange(
                        option->second.elementType, {option->second.payload.value()});
                    if (!payloadRange.has_value()) {
                        traceReject("range-option-payload");
                        return std::nullopt;
                    }
                    loopState.Update(children[1],
                        std::make_unique<SIntRange>(std::move(payloadRange.value())));
                    handledStandardRangeExpression = true;
                }
            }
            if (!handledStandardRangeExpression) {
                PropagateExpressionEffect(loopState, expression);
            }
            if (isCallExpression) {
                auto recorded = observedCallContexts.find(expression);
                if (recorded != observedCallContexts.end()) {
                    auto& uniqueContexts =
                        isInvokeExpression ? uniqueInvokeContexts : uniqueApplyContexts;
                    for (const auto& context : recorded->second.keys) {
                        uniqueContexts.emplace(context);
                    }
                    const size_t contextLimit = isInvokeExpression
                        ? MAX_BOUNDED_LOOP_UNIQUE_INVOKE_CONTEXTS
                        : MAX_BOUNDED_LOOP_UNIQUE_APPLY_CONTEXTS;
                    if (uniqueContexts.size() > contextLimit) {
                        if (failedBoundedLoopContextAttempts.size() <
                            MAX_BOUNDED_LOOP_CACHE_ENTRIES) {
                            failedBoundedLoopContextAttempts.emplace(key);
                        }
                        traceReject("call-context-budget");
                        return std::nullopt;
                    }
                }
            }
            for (const auto& [value, captured] : preservedCallValues) {
                ApplyContextValue(loopState, value, captured);
            }
            for (const auto& [location, value] : preservedCallState) {
                auto rootType = GetBoundedLoopRefRootType(location);
                auto object = loopState.CheckAbstractObjectRefBy(location);
                if (rootType != nullptr && object != nullptr) {
                    ApplyContextValue(loopState, object, rootType, value);
                }
            }
            if (loopState.IsBottom()) {
                traceReject("bottom-after-expression");
                return std::nullopt;
            }
            if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                Value* tracedValue = expression->GetResult();
                if (expression->GetExprKind() == ExprKind::STORE) {
                    tracedValue = StaticCast<const Store*>(expression)->GetValue();
                }
                auto tracedRange = tracedValue == nullptr ? nullptr : loopState.CheckAbstractValue(tracedValue);
                const auto& locationInfo = expression->GetDebugLocation();
                std::cerr << "[RangeAnalysisBoundedStep] step=" << step
                          << " kind=" << static_cast<int>(expression->GetExprKind())
                          << " line=" << locationInfo.GetBeginPos().line
                          << " value=" << (tracedRange == nullptr ? "Top" : tracedRange->ToString()) << '\n';
                for (auto trackedLocation : mutatedLocations) {
                    auto trackedObject = loopState.CheckAbstractObjectRefBy(trackedLocation);
                    auto trackedRange =
                        trackedObject == nullptr ? nullptr : loopState.CheckAbstractValue(trackedObject);
                    std::cerr << "[RangeAnalysisBoundedMemory] step=" << step
                              << " location=" << trackedLocation
                              << " value=" << (trackedRange == nullptr ? "Top" : trackedRange->ToString()) << '\n';
                }
            }
            observeExpression(loopState, expression);
            if (expression->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto location = StaticCast<const Store*>(expression)->GetLocation();
            auto storedValue = StaticCast<const Store*>(expression)->GetValue();
            auto object = loopState.CheckAbstractObjectRefBy(location);
            auto rootType = GetBoundedLoopRefRootType(location);
            if (IsBoundedFlatAggregateType(rootType)) {
                if (storedValue != nullptr && storedValue->GetType() != nullptr && storedValue->GetType()->IsEnum()) {
                    concreteAggregateStores[location] = storedValue;
                } else {
                    concreteAggregateStores.erase(location);
                }
                continue;
            }
            const bool isSingle = rootType != nullptr && object != nullptr && (rootType->IsBoolean()
                ? GetBoolDomainFromStateWithType(loopState, object, rootType).IsSingleValue()
                : ::Cangjie::CHIR::GetSIntDomainFromState(loopState, object, rootType).IsSingleValue());
            if (!isSingle) {
                if (IsDeadLoopLocalNullInitialization(
                        StaticCast<const Store*>(expression), loopBlocks)) {
                    continue;
                }
                if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                    const auto& locationInfo = expression->GetDebugLocation();
                    std::cerr << "[RangeAnalysisBoundedRejectDetail] store-line="
                              << locationInfo.GetBeginPos().line << " location=" << location
                              << " root-type=" << rootType << '\n';
                }
                traceReject("non-single-store");
                return std::nullopt;
            }
        }

        auto terminator = current->GetTerminator();
        if (qgsrProjection.has_value()) {
            const auto operands = terminator->GetOperands();
            if (std::any_of(operands.begin(), operands.end(), [&](Value* operand) {
                    return qgsrProjection->ignoredValues.find(operand) != qgsrProjection->ignoredValues.end();
                })) {
                traceReject("projected-value-controls-flow");
                return std::nullopt;
            }
        }
        auto target = PropagateTerminatorEffect(loopState, terminator);
        Block* next = nullptr;
        if (terminator->GetExprKind() == ExprKind::GOTO) {
            next = StaticCast<const GoTo*>(terminator)->GetDestination();
            takenExactSuccessors.emplace(next);
        } else if (terminator->GetExprKind() == ExprKind::BRANCH) {
            auto currentBranch = StaticCast<const Branch*>(terminator);
            auto condition = GetBoolDomainFromState(loopState, currentBranch->GetCondition());
            if (!condition.IsSingleValue()) {
                traceReject("non-single-branch");
                return std::nullopt;
            }
            next = condition.IsTrue() ? currentBranch->GetTrueBlock() : currentBranch->GetFalseBlock();
            takenExactSuccessors.emplace(next);
            for (auto candidate : {
                currentBranch->GetTrueBlock(), currentBranch->GetFalseBlock()}) {
                if (candidate != next) {
                    skippedExactSuccessors.emplace(candidate);
                }
            }
        } else {
            next = target.value_or(nullptr);
            if (next != nullptr) {
                takenExactSuccessors.emplace(next);
                for (auto candidate : current->GetSuccessors()) {
                    if (candidate != next) {
                        skippedExactSuccessors.emplace(candidate);
                    }
                }
            }
        }
        if (next == nullptr || (next != exitSuccessor && loopBlocks.find(next) == loopBlocks.end())) {
            traceReject("invalid-next-block");
            return std::nullopt;
        }
        current = next;
    }
    if (publishPrefixOnBudget) {
        if (isContextAnalysis) {
            std::lock_guard<std::mutex> lock(boundedLoopObservationsMtx);
            auto& ownerObservations = localBoundedLoopPrefixObservations[this];
            for (const auto& [expression, range] : observedRanges) {
                if (expression == nullptr || range == nullptr) {
                    continue;
                }
                auto currentRange = ownerObservations.find(expression);
                if (currentRange == ownerObservations.end()) {
                    ownerObservations.emplace(expression, range->Clone());
                } else if (auto joined = JoinBoundedLoopObservationRanges(
                    *currentRange->second, *range); joined.has_value()) {
                    currentRange->second = std::move(joined.value());
                }
            }
        } else {
            CommitBoundedLoopPrefixObservations(observedRanges);
        }
        {
            std::lock_guard<std::mutex> lock(boundedLoopObservationsMtx);
            completedBoundedLoopPrefixAttempts[this].emplace(key);
        }
        boundedContextAttempt.MarkSucceeded();
        failedContextAttempt.MarkSucceeded();
    }
    traceReject("step-budget");
    return std::nullopt;
}

std::optional<SIntRange> TryComputeCountedAccumulatorUpdateRange(
    const RangeDomain& state, const BinaryExpression* binaryExpr)
{
    return TryComputeCountedAccumulatorUpdateRangeImpl(state, binaryExpr);
}

std::optional<SIntRange> TryComputeLockstepDifferenceRange(
    const RangeDomain& state, const BinaryExpression* binaryExpr)
{
    return TryComputeLockstepDifferenceRangeImpl(state, binaryExpr);
}

std::optional<SIntRange> TryComputeCommonSymbolDifferenceRange(
    const RangeDomain& state, const BinaryExpression* binaryExpr)
{
    return TryComputeCommonSymbolDifferenceRangeImpl(state, binaryExpr);
}

std::optional<SIntRange> TryComputeSimpleInductionLoadExitRange(const RangeDomain& state, const Load* load)
{
    return TryComputeSimpleInductionLoadExitRangeImpl(state, load);
}

std::optional<SIntRange> TryComputeCountedAccumulatorLoadExitRange(const RangeDomain& state, const Load* load)
{
    return TryComputeCountedAccumulatorLoadExitRangeImpl(state, load);
}

std::optional<SIntRange> TryComputeCountedAccumulatorBodyLoadRange(const RangeDomain& state, const Load* load)
{
    return TryComputeCountedAccumulatorBodyLoadRangeImpl(state, load);
}

// 识别简单归纳变量的 i +/- const 更新，并直接推导更新表达式结果范围。
std::optional<SIntDomain> RangeAnalysis::TryComputeSimpleInductionUpdateRange(
    const BinaryExpression* binaryExpr) const
{
    auto dest = binaryExpr->GetResult();
    if (!dest->GetType()->IsInteger() || dest->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }

    auto lhs = binaryExpr->GetLHSOperand();
    auto rhs = binaryExpr->GetRHSOperand();
    Value* loadValue = nullptr;
    Value* location = nullptr;
    std::optional<int64_t> updateStep;
    switch (binaryExpr->GetExprKind()) {
        case ExprKind::ADD:
            if ((location = GetLoadLocation(lhs)) != nullptr) {
                loadValue = lhs;
                updateStep = GetSignedConstantFromDefiningConstant(rhs);
            } else if ((location = GetLoadLocation(rhs)) != nullptr) {
                loadValue = rhs;
                updateStep = GetSignedConstantFromDefiningConstant(lhs);
            }
            break;
        case ExprKind::SUB:
            location = GetLoadLocation(lhs);
            if (location != nullptr) {
                loadValue = lhs;
                auto rhsStep = GetSignedConstantFromDefiningConstant(rhs);
                updateStep = rhsStep.has_value() ? NegateSignedStep(rhsStep.value()) : std::nullopt;
            }
            break;
        default:
            break;
    }
    if (location == nullptr || loadValue == nullptr || !updateStep.has_value() || updateStep.value() == 0) {
        return std::nullopt;
    }

    auto loadExpr = GetDefiningExpr(loadValue);
    if (loadExpr == nullptr || loadExpr->GetExprKind() != ExprKind::LOAD) {
        return std::nullopt;
    }
    auto header = loadExpr->GetParentBlock();
    auto terminator = header == nullptr ? nullptr : header->GetTerminator();
    if (terminator == nullptr || terminator->GetExprKind() != ExprKind::BRANCH) {
        return std::nullopt;
    }
    auto branch = StaticCast<const Branch*>(terminator);
    if (!IsLoopBranch(branch)) {
        return std::nullopt;
    }

    auto condition = GetSimpleInductionCondition(branch->GetCondition());
    if (!condition.has_value() || condition->location != location) {
        return std::nullopt;
    }
    auto loopStep = FindSingleBackedgeStep(header, location);
    auto init = FindIncomingSignedStoreConstant(header, location);
    if (!loopStep.has_value() || !init.has_value() || loopStep.value() != updateStep.value()) {
        return std::nullopt;
    }

    const Block* exitSuccessor = nullptr;
    for (auto successor : {branch->GetTrueBlock(), branch->GetFalseBlock()}) {
        if (!IsLoopExitSuccessor(branch, successor)) {
            continue;
        }
        if (exitSuccessor != nullptr && exitSuccessor != successor) {
            return std::nullopt;
        }
        exitSuccessor = successor;
    }
    if (exitSuccessor == nullptr) {
        return std::nullopt;
    }
    auto continuationRelation = GetLoopContinuationRelation(
        branch, exitSuccessor, condition->relation);

    auto width = ToWidth(*dest->GetType());
    auto exactExit =
        ComputeExactInductionExit(
            init.value(), loopStep.value(), continuationRelation,
            condition->bound, width);
    if (!exactExit.has_value()) {
        return std::nullopt;
    }
    __int128 firstUpdate = static_cast<__int128>(init.value()) + static_cast<__int128>(updateStep.value());
    if (!FitsSignedWidth(firstUpdate, width)) {
        return std::nullopt;
    }
    __int128 lastUpdate = static_cast<__int128>(exactExit->SVal());
    __int128 lower = firstUpdate < lastUpdate ? firstUpdate : lastUpdate;
    __int128 upper = firstUpdate < lastUpdate ? lastUpdate : firstUpdate;
    if (!FitsSignedWidth(lower, width) || !FitsSignedWidth(upper, width)) {
        return std::nullopt;
    }

    auto lowerValue = SInt{width, static_cast<uint64_t>(static_cast<int64_t>(lower))};
    auto upperValue = SInt{width, static_cast<uint64_t>(static_cast<int64_t>(upper))};
    return SIntDomain::Intersects(SIntDomain::FromNumeric(RelationalOperation::GE, lowerValue, false),
        SIntDomain::FromNumeric(RelationalOperation::LE, upperValue, false));
}

// 处理终结符转移效果，并折叠可证明确定的终结符。
std::optional<Block*> RangeAnalysis::HandleTerminatorEffect(RangeDomain& state, const Terminator* terminator)
{
    RangeAnalysis::ExceptionKind res = ExceptionKind::NA;
    switch (terminator->GetExprKind()) {
        case ExprKind::GOTO:
        case ExprKind::EXIT:
            break;
        case ExprKind::BRANCH:
            return HandleBranchTerminator(state, StaticCast<const Branch*>(terminator));
        case ExprKind::MULTIBRANCH:
            return HandleMultiBranchTerminator(state, StaticCast<const MultiBranch*>(terminator));
        case ExprKind::TYPECAST_WITH_EXCEPTION:
            res = HandleTypeCastWithException(state, StaticCast<const TypeCastWithException*>(terminator));
            break;
        case ExprKind::INT_OP_WITH_EXCEPTION:
            res = HandleIntOpWithException(state, StaticCast<const IntOpWithException*>(terminator));
            break;
        case ExprKind::INVOKESTATIC_WITH_EXCEPTION: {
            auto invoke = StaticCast<const InvokeStaticWithException*>(terminator);
            RecordUnmodeledCallContext(analysisContextKey, terminator);
            HavocCallEffects(state, invoke->GetArgs(), invoke->GetResult());
            break;
        }
        case ExprKind::INTRINSIC_WITH_EXCEPTION:
        default: {
            auto dest = terminator->GetResult();
            if (dest) {
                state.SetToTopOrTopRef(dest, dest->GetType()->IsRef());
            }
            break;
        }
    }
    if (res == ExceptionKind::SUCCESS) {
        return terminator->GetSuccessor(0);
    } else if (res == ExceptionKind::FAIL) {
        return terminator->GetSuccessor(1);
    }

    return std::nullopt;
}

// 为分支和 MultiBranch 后继构造边专属 RangeDomain 状态。
RangeDomain GetTerminatorStateForSuccessor(
    Analysis<RangeDomain>& analysis, const RangeDomain& state, const Terminator* terminator, const Block* successor)
{
    auto& rangeAnalysis = static_cast<RangeAnalysis&>(analysis);
    auto recordAndReturn = [&](RangeDomain result) {
        rangeAnalysis.RecordTerminatorEdgeState(terminator, successor, result);
        return result;
    };
    auto edgeState = state;
    switch (terminator->GetExprKind()) {
        case ExprKind::APPLY_WITH_EXCEPTION: {
            auto apply = StaticCast<const ApplyWithException*>(terminator);
            if (successor == apply->GetErrorBlock()) {
                static_cast<RangeAnalysis&>(analysis).HavocCallEffects(
                    edgeState, apply->GetArgs(), apply->GetResult(), IsApplyToLambda(apply));
            }
            break;
        }
        case ExprKind::INVOKE_WITH_EXCEPTION: {
            auto invoke = StaticCast<const InvokeWithException*>(terminator);
            if (successor == invoke->GetErrorBlock()) {
                static_cast<RangeAnalysis&>(analysis).HavocCallEffects(
                    edgeState, invoke->GetArgs(), invoke->GetResult());
            }
            break;
        }
        case ExprKind::BRANCH: {
            auto branch = StaticCast<const Branch*>(terminator);
            if (branch->GetTrueBlock() == branch->GetFalseBlock()) {
                return recordAndReturn(std::move(edgeState));
            }
            if (successor == branch->GetTrueBlock() || successor == branch->GetFalseBlock()) {
                if (std::getenv("CANGJIE_RA_DISABLE_EDGE_NARROWING") != nullptr) {
                    bool branchCondition = successor == branch->GetTrueBlock();
                    if (!ApplyConditionConstraint(edgeState, branch->GetCondition(), branchCondition)) {
                        edgeState.SetUnreachable();
                    }
                    break;
                }
                const bool preferAnalyticInduction =
                    ShouldPreferAnalyticInductionOverBoundedReplay(state, branch);
                if (std::getenv("CANGJIE_RA_DISABLE_BOUNDED_LOOP") == nullptr &&
                    !preferAnalyticInduction) {
                    if (auto exactExit = rangeAnalysis.TryEvaluateBoundedScalarLoopExit(
                        state, branch, successor, MAX_BOUNDED_LOOP_STEPS,
                        /* publishPrefixOnBudget = */ false);
                        exactExit.has_value()) {
                        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                            const auto& location = branch->GetDebugLocation();
                            std::cerr << "[RangeAnalysisBoundedEdge] line="
                                      << location.GetBeginPos().line
                                      << " successor=" << successor->GetIdentifier()
                                      << " exact=1 state=" << exactExit->ToString() << '\n';
                        }
                        return recordAndReturn(std::move(exactExit.value()));
                    }
                    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                        const auto& location = branch->GetDebugLocation();
                        std::cerr << "[RangeAnalysisBoundedEdge] line="
                                  << location.GetBeginPos().line
                                  << " successor=" << successor->GetIdentifier()
                                  << " exact=0\n";
                    }
                } else if (preferAnalyticInduction) {
                    constexpr size_t MAX_ANALYTIC_LOOP_PREFIX_STEPS = 64;
                    if (IsLoopExitSuccessor(branch, successor)) {
                        (void)rangeAnalysis.TryEvaluateBoundedScalarLoopExit(
                            state, branch, successor, MAX_ANALYTIC_LOOP_PREFIX_STEPS,
                            /* publishPrefixOnBudget = */ true);
                    }
                    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                        const auto& location = branch->GetDebugLocation();
                        std::cerr << "[RangeAnalysisAnalyticInduction] line="
                                  << location.GetBeginPos().line << " replay=skipped\n";
                    }
                }
                bool branchCondition = successor == branch->GetTrueBlock();
                if (!ApplyConditionConstraint(edgeState, branch->GetCondition(), branchCondition)) {
                    if (CanComputeSimpleInductionExitFromState(state, branch, successor)) {
                        edgeState = state;
                        if (!TryNarrowSimpleInductionExit(edgeState, branch, successor)) {
                            edgeState.SetUnreachable();
                        } else if (std::getenv("CANGJIE_RA_DISABLE_LOCKSTEP") == nullptr &&
                            !TryNarrowLockstepInductionEdge(edgeState, branch, successor)) {
                            edgeState.SetUnreachable();
                        } else if (!TryNarrowVariableBoundAccumulatorExit(edgeState, branch, successor)) {
                            edgeState.SetUnreachable();
                        } else if (!TryNarrowPairInductionExit(edgeState, branch, successor)) {
                            edgeState.SetUnreachable();
                        } else if (!TryNarrowMustExecuteLoopCarriedValues(edgeState, branch, successor)) {
                            edgeState.SetUnreachable();
                        }
                    } else if (CanComputePairInductionExitFromState(state, branch, successor)) {
                        edgeState = state;
                        if (!TryNarrowPairInductionExit(edgeState, branch, successor)) {
                            edgeState.SetUnreachable();
                        }
                    } else {
                        edgeState.SetUnreachable();
                    }
                } else if (!TryNarrowSimpleInductionBody(edgeState, branch, successor)) {
                    edgeState.SetUnreachable();
                } else if (std::getenv("CANGJIE_RA_DISABLE_LOCKSTEP") == nullptr &&
                    !TryNarrowLockstepInductionEdge(edgeState, branch, successor)) {
                    edgeState.SetUnreachable();
                } else if (IsLoopExitSuccessor(branch, successor) &&
                    !TryNarrowSimpleInductionExit(edgeState, branch, successor)) {
                    edgeState.SetUnreachable();
                } else if (IsLoopExitSuccessor(branch, successor) &&
                    !TryNarrowVariableBoundAccumulatorExit(edgeState, branch, successor)) {
                    edgeState.SetUnreachable();
                } else if (IsLoopExitSuccessor(branch, successor) &&
                    !TryNarrowPairInductionExit(edgeState, branch, successor)) {
                    edgeState.SetUnreachable();
                } else if (IsLoopExitSuccessor(branch, successor) &&
                    !TryNarrowMustExecuteLoopCarriedValues(edgeState, branch, successor)) {
                    edgeState.SetUnreachable();
                }
            }
            break;
        }
        case ExprKind::MULTIBRANCH:
            if (!ApplyMultiBranchConstraint(edgeState, StaticCast<const MultiBranch*>(terminator), successor)) {
                edgeState.SetUnreachable();
            }
            break;
        default:
            break;
    }
    return recordAndReturn(std::move(edgeState));
}

void RangeAnalysis::PrintBranchOptMessage(const Ptr<const Expression>& expr, bool isTrueBlockRemained) const
{
    std::string message = "[RangeAnalysis] The If Block" + ToPosInfo(expr->GetDebugLocation()) +
        " has been replace to the " + (isTrueBlockRemained ? "True Block" : "False Block") + "\n";
    std::cout << message;
}

// 仅在条件为单点且不是循环 guard 时选择分支后继。
std::optional<Block*> RangeAnalysis::HandleBranchTerminator(const RangeDomain& state, const Branch* branch) const
{
    if (IsLoopBranch(branch)) {
        return std::nullopt;
    }
    auto cond = branch->GetCondition();
    const auto& condVal = GetBoolDomainFromState(state, cond);
    if (!condVal.IsSingleValue()) {
        return std::nullopt;
    }
    if (isDebug) {
        PrintBranchOptMessage(branch, condVal.IsTrue());
    }
    return condVal.IsTrue() ? branch->GetTrueBlock() : branch->GetFalseBlock();
}

// 在条件为单点 case 值时选择 MultiBranch 后继。
std::optional<Block*> RangeAnalysis::HandleMultiBranchTerminator(
    const RangeDomain& state, const MultiBranch* multi) const
{
    auto cond = multi->GetCondition();
    const auto& condVal = GetSIntDomainFromState(state, cond);
    if (!condVal.IsSingleValue()) {
        return std::nullopt;
    }
    auto val = condVal.NumericBound().Lower().UVal();
    auto cases = multi->GetCaseVals();
    for (size_t i = 0; i < cases.size(); ++i) {
        if (val == cases[i]) {
            return multi->GetSuccessor(i + 1);
        }
    }
    return multi->GetDefaultBlock();
}
} // namespace Cangjie::CHIR
