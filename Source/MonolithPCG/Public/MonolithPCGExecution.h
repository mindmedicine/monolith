#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

#if WITH_PCG

class UPCGGraph;
struct FPCGDataCollection;

/**
 * MonolithPCG — the ground-truth instrument.
 *
 * WHY THIS SHAPE (read before changing it):
 *
 * 1. PCG graph execution is ASYNCHRONOUS and cannot be pumped by us. The executor is
 *    driven by `UPCGEngineSubsystem::Tick` (an FTickableGameObject with
 *    `IsTickableInEditor() == true`), and `IPCGBaseSubsystem::Tick()` is *protected* —
 *    there is no public way to advance it. An MCP handler that blocked the game thread
 *    waiting for completion would therefore deadlock, not wait. So the instrument is
 *    deliberately two-call: `execute_pcg_graph` starts and returns an id;
 *    `get_pcg_output_summary` reports status/result.
 *
 * 2. The output is summarised INSIDE the post-process callback, and only plain JSON is
 *    retained. The callback hands us a `const FPCGDataCollection&` whose
 *    `FPCGDataPtrWrapper`s point at UPCGData UObjects that are kept alive only by the
 *    executor context and by `IPCGBaseSubsystem::GraphExecutions` — and the source is
 *    removed from `GraphExecutions` (RemoveAtSwap, IPCGBaseSubsystem.cpp) the moment
 *    generation completes. Stashing the collection in a plain TMap would leave dangling
 *    or GC'd pointers; the engine works around this for inspection with
 *    `FPCGInspectionData : public FGCObject`. Summarising eagerly sidesteps the whole
 *    lifetime problem and costs us nothing we need.
 *
 * 3. Thread affinity is VERIFIED, not assumed. `UPCGDefaultExecutionSource::Generate()`
 *    builds its post-process `FPCGScheduleGenericParams` with the 5-argument constructor
 *    (Operation, ExecutionSource, ExecutionDependencies, DataDependencies,
 *    bSupportBasePointDataInput), which leaves `bCanExecuteOnlyOnMainThread` at its
 *    default of `true` (PCGCommon.h). The callbacks therefore run on the game thread and
 *    this registry needs no lock.
 *
 * 4. Ordering is VERIFIED: in `UPCGDefaultExecutionSource::Generate()` the post-process
 *    lambda calls `OnPostProcess(InContext->InputData)` (data) BEFORE
 *    `OnPCGSourceGenerationDone` (status). So the summary always lands before the
 *    terminal status. `OnPostProcess` is ungated; the generation-done broadcast is inside
 *    `#if PCG_PROFILING_ENABLED` — which is 1 whenever WITH_EDITOR, so it holds for us,
 *    but the data callback is the one to trust.
 */

enum class EMonolithPCGExecStatus : uint8
{
	Running,
	Completed,
	Aborted
};

struct FMonolithPCGExecutionRecord
{
	FString ExecutionId;
	FString GraphPath;
	int32 Seed = 42;
	EMonolithPCGExecStatus Status = EMonolithPCGExecStatus::Running;

	/** FPlatformTime::Seconds() at schedule time / at terminal status. */
	double StartTimeSeconds = 0.0;
	double EndTimeSeconds = 0.0;

	/** True once the post-process callback delivered a data collection. */
	bool bOutputCaptured = false;

	/** Summary computed eagerly inside the post-process callback. Null until then. */
	TSharedPtr<FJsonObject> Summary;

	/** Human-readable note attached to a terminal status (e.g. why it aborted). */
	FString StatusDetail;
};

class FMonolithPCGExecutionRegistry
{
public:
	static FMonolithPCGExecutionRegistry& Get();

	/**
	 * Schedules InGraph for headless execution and registers a pending record.
	 * Returns the new execution id, or an empty string with OutError set. Never
	 * partially mutates: every precondition is checked before anything is scheduled.
	 */
	FString Start(UPCGGraph* InGraph, const FString& InGraphPath, int32 InSeed, FString& OutError);

	const FMonolithPCGExecutionRecord* Find(const FString& InExecutionId) const;

	/** Most recent execution id for a given graph path, or empty. */
	FString FindLatestForGraph(const FString& InGraphPath) const;

	TArray<FString> GetAllIds() const;

private:
	void OnPostProcess(const FString& InExecutionId, const FPCGDataCollection& InData);
	void OnGenerationDone(const FString& InExecutionId, bool bCompleted);

	TMap<FString, FMonolithPCGExecutionRecord> Records;
	int32 Counter = 0;
};

namespace MonolithPCGSummary
{
	/**
	 * Builds the authoritative output summary: per-tagged-data pin label, concrete UPCGData
	 * class, point count for point data, bounds for spatial data, and metadata attribute
	 * names + engine-spelled types. This is read off the executed data, not off anything
	 * the caller wrote.
	 */
	TSharedPtr<FJsonObject> SummarizeDataCollection(const FPCGDataCollection& InCollection);
}

#endif // WITH_PCG
