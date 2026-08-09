#include "MonolithPCGExecution.h"

#if WITH_PCG

#include "MonolithJsonUtils.h"

#include "PCGCommon.h"
#include "PCGData.h"
#include "PCGDefaultExecutionSource.h"
#include "PCGGraph.h"
#include "Data/PCGBasePointData.h"
#include "Data/PCGSpatialData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataCommon.h"
#include "Subsystems/IPCGBaseSubsystem.h"
#include "Subsystems/PCGEngineSubsystem.h"

namespace
{
	TSharedPtr<FJsonObject> BoxToJson(const FBox& InBox)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("min_x"), InBox.Min.X);
		Obj->SetNumberField(TEXT("min_y"), InBox.Min.Y);
		Obj->SetNumberField(TEXT("min_z"), InBox.Min.Z);
		Obj->SetNumberField(TEXT("max_x"), InBox.Max.X);
		Obj->SetNumberField(TEXT("max_y"), InBox.Max.Y);
		Obj->SetNumberField(TEXT("max_z"), InBox.Max.Z);
		return Obj;
	}
}

TSharedPtr<FJsonObject> MonolithPCGSummary::SummarizeDataCollection(const FPCGDataCollection& InCollection)
{
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();

	const TArray<FPCGTaggedData>& AllData = InCollection.GetAllInputs();

	int32 TotalPoints = 0;
	int32 PointDataCount = 0;
	TArray<TSharedPtr<FJsonValue>> Entries;
	TSet<FName> PinLabels;

	for (const FPCGTaggedData& Tagged : AllData)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("pin"), Tagged.Pin.ToString());
		PinLabels.Add(Tagged.Pin);

		const UPCGData* Data = Tagged.Data.Get();
		if (!Data)
		{
			// A null entry is real information, not something to hide.
			Entry->SetStringField(TEXT("data_class"), TEXT("<null>"));
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		// Ask the engine what the data IS (its concrete class), rather than inferring
		// from whether it happens to answer a point query.
		Entry->SetStringField(TEXT("data_class"), Data->GetClass()->GetName());

		if (Tagged.Tags.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> TagValues;
			for (const FString& Tag : Tagged.Tags)
			{
				TagValues.Add(MakeShared<FJsonValueString>(Tag));
			}
			Entry->SetArrayField(TEXT("tags"), TagValues);
		}

		if (const UPCGBasePointData* PointData = Cast<const UPCGBasePointData>(Data))
		{
			const int32 NumPoints = PointData->GetNumPoints();
			Entry->SetNumberField(TEXT("point_count"), NumPoints);
			TotalPoints += NumPoints;
			++PointDataCount;
		}

		if (const UPCGSpatialData* SpatialData = Cast<const UPCGSpatialData>(Data))
		{
			if (SpatialData->IsBounded())
			{
				const FBox Bounds = SpatialData->GetBounds();
				if (Bounds.IsValid != 0)
				{
					Entry->SetObjectField(TEXT("bounds"), BoxToJson(Bounds));
				}
				else
				{
					Entry->SetStringField(TEXT("bounds"), TEXT("<invalid>"));
				}
			}
			else
			{
				Entry->SetStringField(TEXT("bounds"), TEXT("<unbounded>"));
			}
		}

		if (const UPCGMetadata* Metadata = Data->ConstMetadata())
		{
			TArray<FName> AttributeNames;
			TArray<EPCGMetadataTypes> AttributeTypes;
			Metadata->GetAttributes(AttributeNames, AttributeTypes);

			if (AttributeNames.Num() > 0)
			{
				const UEnum* TypeEnum = StaticEnum<EPCGMetadataTypes>();
				TArray<TSharedPtr<FJsonValue>> AttributeValues;
				for (int32 Index = 0; Index < AttributeNames.Num(); ++Index)
				{
					TSharedPtr<FJsonObject> AttrObj = MakeShared<FJsonObject>();
					AttrObj->SetStringField(TEXT("name"), AttributeNames[Index].ToString());

					// Engine spelling of the type, read off the attribute — never echoed
					// back from a caller-supplied string.
					FString TypeName = TEXT("<unknown>");
					if (TypeEnum && AttributeTypes.IsValidIndex(Index))
					{
						TypeName = TypeEnum->GetNameStringByValue(static_cast<int64>(AttributeTypes[Index]));
					}
					AttrObj->SetStringField(TEXT("type"), TypeName);
					AttributeValues.Add(MakeShared<FJsonValueObject>(AttrObj));
				}
				Entry->SetArrayField(TEXT("attributes"), AttributeValues);
			}
		}

		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Summary->SetNumberField(TEXT("data_count"), AllData.Num());
	Summary->SetNumberField(TEXT("point_data_count"), PointDataCount);
	Summary->SetNumberField(TEXT("total_points"), TotalPoints);
	Summary->SetArrayField(TEXT("data"), Entries);

	TArray<TSharedPtr<FJsonValue>> PinValues;
	for (const FName& Pin : PinLabels)
	{
		PinValues.Add(MakeShared<FJsonValueString>(Pin.ToString()));
	}
	Summary->SetArrayField(TEXT("output_pins"), PinValues);

	return Summary;
}

FMonolithPCGExecutionRegistry& FMonolithPCGExecutionRegistry::Get()
{
	static FMonolithPCGExecutionRegistry Instance;
	return Instance;
}

FString FMonolithPCGExecutionRegistry::Start(UPCGGraph* InGraph, const FString& InGraphPath, int32 InSeed, FString& OutError)
{
	OutError.Reset();

	if (!IsInGameThread())
	{
		// IPCGBaseSubsystem::CreateExecutionSource asserts on this; refuse rather than
		// trip an engine check().
		OutError = TEXT("PCG execution must be started from the game thread.");
		return FString();
	}

	if (!InGraph)
	{
		OutError = TEXT("No graph supplied.");
		return FString();
	}

	// The engine resolves a world-less execution state to UPCGEngineSubsystem
	// (IPCGGraphExecutionState::GetSubsystem). If that subsystem does not exist there is
	// nothing to schedule onto and CreateExecutionSource would check() on a null
	// subsystem — so verify BEFORE we register a record or touch anything.
	if (UPCGEngineSubsystem::Get() == nullptr)
	{
		OutError = TEXT("UPCGEngineSubsystem is unavailable; cannot schedule a headless PCG graph execution.");
		return FString();
	}

	const FString ExecutionId = FString::Printf(TEXT("pcgexec_%d"), ++Counter);

	FMonolithPCGExecutionRecord Record;
	Record.ExecutionId = ExecutionId;
	Record.GraphPath = InGraphPath;
	Record.Seed = InSeed;
	Record.Status = EMonolithPCGExecStatus::Running;
	Record.StartTimeSeconds = FPlatformTime::Seconds();
	Records.Add(ExecutionId, Record);

	FPCGDefaultExecutionSourceParams Params;
	Params.GraphInterface = InGraph;
	Params.Seed = InSeed;
	Params.bFireAndForgetExecution = true;
	Params.PostProcessCallback = FPCGGenerationPostProcessCallback::CreateLambda(
		[ExecutionId](const FPCGDataCollection& InData)
		{
			FMonolithPCGExecutionRegistry::Get().OnPostProcess(ExecutionId, InData);
		});
	Params.GenerationCallback = FPCGOnEditorGenerationDone::CreateLambda(
		[ExecutionId](IPCGGraphExecutionSource* /*InSource*/, EPCGGenerationStatus InStatus)
		{
			FMonolithPCGExecutionRegistry::Get().OnGenerationDone(ExecutionId, InStatus == EPCGGenerationStatus::Completed);
		});

	UPCGDefaultExecutionSource* Source =
		IPCGBaseSubsystem::CreateExecutionSource<UPCGDefaultExecutionSource>(Params);

	if (!Source)
	{
		// Roll our own state back — we are the only owner of this record and nothing
		// engine-side was created, so this really is a clean refusal.
		Records.Remove(ExecutionId);
		OutError = TEXT("IPCGBaseSubsystem::CreateExecutionSource returned null (graph interface rejected).");
		return FString();
	}

	return ExecutionId;
}

void FMonolithPCGExecutionRegistry::OnPostProcess(const FString& InExecutionId, const FPCGDataCollection& InData)
{
	FMonolithPCGExecutionRecord* Record = Records.Find(InExecutionId);
	if (!Record)
	{
		return;
	}

	// Summarise eagerly: the UPCGData in this collection are not ours to keep alive.
	Record->Summary = MonolithPCGSummary::SummarizeDataCollection(InData);
	Record->bOutputCaptured = true;
}

void FMonolithPCGExecutionRegistry::OnGenerationDone(const FString& InExecutionId, bool bCompleted)
{
	FMonolithPCGExecutionRecord* Record = Records.Find(InExecutionId);
	if (!Record)
	{
		return;
	}

	Record->Status = bCompleted ? EMonolithPCGExecStatus::Completed : EMonolithPCGExecStatus::Aborted;
	Record->EndTimeSeconds = FPlatformTime::Seconds();

	if (!bCompleted)
	{
		Record->StatusDetail = TEXT("PCG reported EPCGGenerationStatus::Aborted.");
	}
	else if (!Record->bOutputCaptured)
	{
		// Completed but no data callback ran. Say so plainly instead of reporting an
		// empty graph output as if it were a measured result.
		Record->StatusDetail = TEXT("Generation completed but the post-process callback never delivered a data collection — the output summary is UNKNOWN, not empty.");
	}
}

const FMonolithPCGExecutionRecord* FMonolithPCGExecutionRegistry::Find(const FString& InExecutionId) const
{
	return Records.Find(InExecutionId);
}

FString FMonolithPCGExecutionRegistry::FindLatestForGraph(const FString& InGraphPath) const
{
	FString Best;
	double BestTime = -1.0;
	for (const TPair<FString, FMonolithPCGExecutionRecord>& Pair : Records)
	{
		if (Pair.Value.GraphPath == InGraphPath && Pair.Value.StartTimeSeconds > BestTime)
		{
			BestTime = Pair.Value.StartTimeSeconds;
			Best = Pair.Key;
		}
	}
	return Best;
}

TArray<FString> FMonolithPCGExecutionRegistry::GetAllIds() const
{
	TArray<FString> Ids;
	Records.GetKeys(Ids);
	return Ids;
}

#endif // WITH_PCG
