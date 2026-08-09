#include "MonolithPCGActions.h"

#if WITH_PCG

#include "MonolithPCGExecution.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithParamSchema.h"

#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "MonolithPCGActions"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{
	/** Candidate spellings a caller may plausibly use for a settings class. */
	void GetSettingsClassAliases(UClass* InClass, TArray<FString>& OutAliases)
	{
		const FString ClassName = InClass->GetName();          // e.g. "PCGSurfaceSamplerSettings"
		OutAliases.Add(ClassName);

		FString Trimmed = ClassName;
		if (Trimmed.StartsWith(TEXT("PCG")))
		{
			Trimmed.RightChopInline(3, EAllowShrinking::No);
		}
		if (Trimmed.EndsWith(TEXT("Settings")))
		{
			Trimmed.LeftChopInline(8, EAllowShrinking::No);
		}
		if (!Trimmed.IsEmpty() && Trimmed != ClassName)
		{
			OutAliases.AddUnique(Trimmed);                     // e.g. "SurfaceSampler"
		}

#if WITH_EDITOR
		// The engine's own name for the node, straight off the CDO.
		// NOTE: UPCGSettings::GetDefaultNodeName/GetDefaultNodeTitle live inside
		// `#if WITH_EDITOR` in PCGSettings.h - guarded here so this file stays valid if it
		// is ever compiled into a non-editor target.
		if (const UPCGSettings* CDO = Cast<UPCGSettings>(InClass->GetDefaultObject()))
		{
			const FName DefaultNodeName = CDO->GetDefaultNodeName();
			if (!DefaultNodeName.IsNone())
			{
				OutAliases.AddUnique(DefaultNodeName.ToString());
			}
		}
#endif
	}

	bool IsUsableSettingsClass(UClass* InClass)
	{
		if (!InClass || !InClass->IsChildOf(UPCGSettings::StaticClass()))
		{
			return false;
		}
		if (InClass == UPCGSettings::StaticClass())
		{
			return false;
		}
		return !InClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Hidden);
	}

	TSharedPtr<FJsonObject> SerializePin(const UPCGPin* InPin)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("label"), InPin->Properties.Label.ToString());
		Obj->SetNumberField(TEXT("edge_count"), InPin->Edges.Num());
		Obj->SetBoolField(TEXT("allows_multiple_connections"), InPin->Properties.AllowsMultipleConnections());
		return Obj;
	}
}

UPCGGraph* FMonolithPCGActions::LoadGraph(const FString& InAssetPath, FString& OutError)
{
	OutError.Reset();

	// The registry deliberately skips its required-param check for `asset_path`
	// (MonolithToolRegistry.cpp), and FJsonObject::GetStringField yields "" for a missing
	// field rather than failing - so an absent path arrives here as an empty string.
	if (InAssetPath.IsEmpty())
	{
		OutError = TEXT("'asset_path' is required and was not supplied.");
		return nullptr;
	}

	UPCGGraph* Graph = FMonolithAssetUtils::LoadAssetByPath<UPCGGraph>(InAssetPath);
	if (!Graph)
	{
		OutError = FString::Printf(
			TEXT("No UPCGGraph could be loaded at '%s'. Check the path, and note that a PCG Graph Instance (UPCGGraphInstance) is a different asset type."),
			*InAssetPath);
		return nullptr;
	}
	return Graph;
}

FString FMonolithPCGActions::MakeNodeId(const UPCGGraph* InGraph, const UPCGNode* InNode)
{
	if (!InNode)
	{
		return FString();
	}
	if (InGraph)
	{
		if (InNode == InGraph->GetInputNode())
		{
			return TEXT("input");
		}
		if (InNode == InGraph->GetOutputNode())
		{
			return TEXT("output");
		}
	}
	return InNode->GetName();
}

UPCGNode* FMonolithPCGActions::ResolveNode(UPCGGraph* InGraph, const FString& InNodeId, FString& OutError)
{
	OutError.Reset();

	if (!InGraph)
	{
		OutError = TEXT("No graph.");
		return nullptr;
	}

	if (InNodeId.Equals(TEXT("input"), ESearchCase::IgnoreCase))
	{
		UPCGNode* Node = InGraph->GetInputNode();
		if (!Node)
		{
			OutError = TEXT("Graph has no input node.");
		}
		return Node;
	}
	if (InNodeId.Equals(TEXT("output"), ESearchCase::IgnoreCase))
	{
		UPCGNode* Node = InGraph->GetOutputNode();
		if (!Node)
		{
			OutError = TEXT("Graph has no output node.");
		}
		return Node;
	}

	for (UPCGNode* Node : InGraph->GetNodes())
	{
		if (Node && Node->GetName().Equals(InNodeId, ESearchCase::IgnoreCase))
		{
			return Node;
		}
	}

	// The remediation must name ids that actually exist - a suggestion that cannot be
	// used is worse than no suggestion.
	TArray<FString> ValidIds;
	ValidIds.Add(TEXT("input"));
	ValidIds.Add(TEXT("output"));
	for (const UPCGNode* Node : InGraph->GetNodes())
	{
		if (Node)
		{
			ValidIds.Add(Node->GetName());
		}
	}

	OutError = FString::Printf(TEXT("No node '%s' in this graph. Valid node ids: [%s]"),
		*InNodeId, *FString::Join(ValidIds, TEXT(", ")));
	return nullptr;
}

UClass* FMonolithPCGActions::ResolveSettingsClass(const FString& InRequested, FString& OutError)
{
	OutError.Reset();

	UClass* Match = nullptr;
	TArray<FString> NearMisses;

	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Candidate = *ClassIt;
		if (!IsUsableSettingsClass(Candidate))
		{
			continue;
		}

		TArray<FString> Aliases;
		GetSettingsClassAliases(Candidate, Aliases);

		for (const FString& Alias : Aliases)
		{
			if (Alias.Equals(InRequested, ESearchCase::IgnoreCase))
			{
				if (Match && Match != Candidate)
				{
					OutError = FString::Printf(
						TEXT("'%s' is ambiguous - it matches both '%s' and '%s'. Use the exact UClass name."),
						*InRequested, *Match->GetName(), *Candidate->GetName());
					return nullptr;
				}
				Match = Candidate;
			}
			else if (NearMisses.Num() < 8 && Alias.Contains(InRequested, ESearchCase::IgnoreCase))
			{
				NearMisses.AddUnique(Candidate->GetName());
			}
		}
	}

	if (Match)
	{
		return Match;
	}

	OutError = FString::Printf(TEXT("No UPCGSettings subclass matches '%s'."), *InRequested);
	if (NearMisses.Num() > 0)
	{
		OutError += FString::Printf(TEXT(" Did you mean: [%s]?"), *FString::Join(NearMisses, TEXT(", ")));
	}
	OutError += TEXT(" Call pcg.list_pcg_node_types to enumerate what is actually registered.");
	return nullptr;
}

TSharedPtr<FJsonObject> FMonolithPCGActions::SerializeNode(const UPCGGraph* InGraph, const UPCGNode* InNode)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("node_id"), MakeNodeId(InGraph, InNode));
	Obj->SetStringField(TEXT("title"), InNode->GetDefaultTitle().ToString());

	const FName AuthoredTitle = InNode->GetAuthoredTitleName();
	if (!AuthoredTitle.IsNone())
	{
		Obj->SetStringField(TEXT("authored_title"), AuthoredTitle.ToString());
	}

	if (const UPCGSettings* Settings = InNode->GetSettings())
	{
		Obj->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
	}
	else
	{
		Obj->SetStringField(TEXT("settings_class"), TEXT("<none>"));
	}
	Obj->SetBoolField(TEXT("is_settings_instance"), InNode->IsInstance());

#if WITH_EDITOR
	int32 PosX = 0, PosY = 0;
	InNode->GetNodePosition(PosX, PosY);
	Obj->SetNumberField(TEXT("position_x"), PosX);
	Obj->SetNumberField(TEXT("position_y"), PosY);
#endif

	TArray<TSharedPtr<FJsonValue>> InputPins;
	for (const UPCGPin* Pin : InNode->GetInputPins())
	{
		if (Pin)
		{
			InputPins.Add(MakeShared<FJsonValueObject>(SerializePin(Pin)));
		}
	}
	Obj->SetArrayField(TEXT("input_pins"), InputPins);

	TArray<TSharedPtr<FJsonValue>> OutputPins;
	for (const UPCGPin* Pin : InNode->GetOutputPins())
	{
		if (Pin)
		{
			OutputPins.Add(MakeShared<FJsonValueObject>(SerializePin(Pin)));
		}
	}
	Obj->SetArrayField(TEXT("output_pins"), OutputPins);

	return Obj;
}

// ---------------------------------------------------------------------------
// PRIORITY 1 - ground truth
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPCGActions::ExecutePCGGraph(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	FString LoadError;
	UPCGGraph* Graph = LoadGraph(AssetPath, LoadError);
	if (!Graph)
	{
		return FMonolithActionResult::Error(LoadError);
	}

	int32 Seed = 42;
	if (Params->HasTypedField<EJson::Number>(TEXT("seed")))
	{
		Seed = static_cast<int32>(Params->GetNumberField(TEXT("seed")));
	}

	FString StartError;
	const FString ExecutionId = FMonolithPCGExecutionRegistry::Get().Start(Graph, AssetPath, Seed, StartError);
	if (ExecutionId.IsEmpty())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Could not start PCG graph execution: %s NOTHING WAS SCHEDULED."), *StartError));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("execution_id"), ExecutionId);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetStringField(TEXT("status"), TEXT("running"));
	Result->SetStringField(TEXT("note"),
		TEXT("Execution is ASYNCHRONOUS and this call does not wait. PCG's executor is driven by ")
		TEXT("UPCGEngineSubsystem::Tick and cannot be pumped from an action handler, so blocking here would ")
		TEXT("deadlock rather than wait. Poll pcg.get_pcg_output_summary with this execution_id; it will report ")
		TEXT("status 'running' until the editor has ticked enough to finish the graph."));
	Result->SetStringField(TEXT("execution_model"),
		TEXT("headless: UPCGDefaultExecutionSource with no world, no actor and no UPCGComponent. ")
		TEXT("Nodes that require a world (landscape / world ray hit / actor sampling) will produce nothing on this path."));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGActions::GetPCGOutputSummary(const TSharedPtr<FJsonObject>& Params)
{
	FString ExecutionId;
	if (Params->HasTypedField<EJson::String>(TEXT("execution_id")))
	{
		ExecutionId = Params->GetStringField(TEXT("execution_id"));
	}
	else if (Params->HasTypedField<EJson::String>(TEXT("asset_path")))
	{
		const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
		ExecutionId = FMonolithPCGExecutionRegistry::Get().FindLatestForGraph(AssetPath);
		if (ExecutionId.IsEmpty())
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("No execution has been started for '%s' in this editor session. Call pcg.execute_pcg_graph first."),
				*AssetPath));
		}
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Supply either 'execution_id' or 'asset_path'."));
	}

	const FMonolithPCGExecutionRecord* Record = FMonolithPCGExecutionRegistry::Get().Find(ExecutionId);
	if (!Record)
	{
		const TArray<FString> Known = FMonolithPCGExecutionRegistry::Get().GetAllIds();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown execution_id '%s'. Known ids this session: [%s]"),
			*ExecutionId, *FString::Join(Known, TEXT(", "))));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("execution_id"), Record->ExecutionId);
	Result->SetStringField(TEXT("asset_path"), Record->GraphPath);
	Result->SetNumberField(TEXT("seed"), Record->Seed);

	const TCHAR* StatusText = TEXT("running");
	switch (Record->Status)
	{
	case EMonolithPCGExecStatus::Completed: StatusText = TEXT("completed"); break;
	case EMonolithPCGExecStatus::Aborted:   StatusText = TEXT("aborted");   break;
	default:                                StatusText = TEXT("running");   break;
	}
	Result->SetStringField(TEXT("status"), StatusText);

	const double EndTime = (Record->EndTimeSeconds > 0.0) ? Record->EndTimeSeconds : FPlatformTime::Seconds();
	Result->SetNumberField(TEXT("elapsed_seconds"), EndTime - Record->StartTimeSeconds);
	Result->SetBoolField(TEXT("output_captured"), Record->bOutputCaptured);

	if (!Record->StatusDetail.IsEmpty())
	{
		Result->SetStringField(TEXT("status_detail"), Record->StatusDetail);
	}

	if (Record->Summary.IsValid())
	{
		Result->SetObjectField(TEXT("output"), Record->Summary);
	}
	else if (Record->Status == EMonolithPCGExecStatus::Running)
	{
		Result->SetStringField(TEXT("output_note"),
			TEXT("Still running - no output yet. This is NOT an empty result; poll again."));
	}
	else
	{
		Result->SetStringField(TEXT("output_note"),
			TEXT("Execution reached a terminal status without delivering a data collection. Output is UNKNOWN, not empty."));
	}

	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// PRIORITY 2 - minimum authoring set
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPCGActions::CreatePCGGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	// --- validate everything before creating anything ---
	const FString PathError = MonolithCore::ValidatePackagePath(AssetPath);
	if (!PathError.IsEmpty())
	{
		return FMonolithActionResult::Error(PathError);
	}

	FString PackagePath, AssetName;
	int32 LastSlash = INDEX_NONE;
	if (!AssetPath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(TEXT("Invalid asset path - must contain at least one '/' (e.g. /Game/FX/_Probes/PCG_Test_Graph)"));
	}
	PackagePath = AssetPath.Left(LastSlash);
	AssetName = AssetPath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Asset name is empty"));
	}

	if (FMonolithAssetUtils::AssetExists(AssetPath))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists at '%s'. NOTHING WAS CHANGED."), *AssetPath));
	}

	// Validate the usage enum against the real UEnum, and refuse anything it does not
	// contain. Silently coercing an unknown string to index 0 has bitten this codebase
	// repeatedly.
	EPCGGraphUsage Usage = EPCGGraphUsage::Standard;
	if (Params->HasTypedField<EJson::String>(TEXT("usage")))
	{
		const FString UsageStr = Params->GetStringField(TEXT("usage"));
		const UEnum* UsageEnum = StaticEnum<EPCGGraphUsage>();
		if (!UsageEnum)
		{
			return FMonolithActionResult::Error(TEXT("EPCGGraphUsage enum is not available for reflection."));
		}

		int64 FoundValue = INDEX_NONE;
		for (int32 Index = 0; Index < UsageEnum->NumEnums() - 1; ++Index)
		{
			if (UsageEnum->GetNameStringByIndex(Index).Equals(UsageStr, ESearchCase::IgnoreCase))
			{
				FoundValue = UsageEnum->GetValueByIndex(Index);
				break;
			}
		}

		if (FoundValue == INDEX_NONE)
		{
			TArray<FString> ValidNames;
			for (int32 Index = 0; Index < UsageEnum->NumEnums() - 1; ++Index)
			{
				ValidNames.Add(UsageEnum->GetNameStringByIndex(Index));
			}
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Unknown usage '%s'. Valid values: [%s]. NOTHING WAS CREATED."),
				*UsageStr, *FString::Join(ValidNames, TEXT(", "))));
		}
		Usage = static_cast<EPCGGraphUsage>(FoundValue);
	}

	// --- mutate ---
	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));
	}
	Pkg->FullyLoad();

	// This mirrors UPCGGraphFactory::FactoryCreateNew's non-template branch exactly
	// (NewObject<UPCGGraph>(InParent, InClass, InName, Flags)); the UPCGGraph constructor
	// is what creates the input/output nodes and their default pins, so no extra setup is
	// required or wanted here.
	UPCGGraph* NewGraph = NewObject<UPCGGraph>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!NewGraph)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UPCGGraph object"));
	}

#if WITH_EDITORONLY_DATA
	NewGraph->GraphUsageContext = Usage;
#endif

	FAssetRegistryModule::AssetCreated(NewGraph);
	Pkg->MarkPackageDirty();

	bool bSaved = false;
	const bool bSaveRequested = Params->HasTypedField<EJson::Boolean>(TEXT("save")) && Params->GetBoolField(TEXT("save"));
	if (bSaveRequested)
	{
		bSaved = UEditorAssetLibrary::SaveLoadedAsset(NewGraph, /*bOnlyIfIsDirty=*/false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("graph_name"), NewGraph->GetName());
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (bSaveRequested && !bSaved)
	{
		Result->SetStringField(TEXT("save_warning"),
			TEXT("save was requested but SaveLoadedAsset returned false - the asset exists in memory only."));
	}

	// Report the usage read back off the object, not the string we were handed.
#if WITH_EDITORONLY_DATA
	if (const UEnum* UsageEnum = StaticEnum<EPCGGraphUsage>())
	{
		Result->SetStringField(TEXT("usage"), UsageEnum->GetNameStringByValue(static_cast<int64>(NewGraph->GraphUsageContext)));
	}
	Result->SetBoolField(TEXT("is_standalone_graph"), NewGraph->IsStandaloneGraph());
#endif

	Result->SetStringField(TEXT("input_node_id"), TEXT("input"));
	Result->SetStringField(TEXT("output_node_id"), TEXT("output"));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGActions::ListPCGNodes(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	FString LoadError;
	UPCGGraph* Graph = LoadGraph(AssetPath, LoadError);
	if (!Graph)
	{
		return FMonolithActionResult::Error(LoadError);
	}

	TArray<TSharedPtr<FJsonValue>> Nodes;

	// The terminal nodes are separate UPROPERTYs on UPCGGraph and are NOT in GetNodes();
	// omitting them would make the graph look unconnectable.
	if (const UPCGNode* InputNode = Graph->GetInputNode())
	{
		Nodes.Add(MakeShared<FJsonValueObject>(SerializeNode(Graph, InputNode)));
	}
	if (const UPCGNode* OutputNode = Graph->GetOutputNode())
	{
		Nodes.Add(MakeShared<FJsonValueObject>(SerializeNode(Graph, OutputNode)));
	}
	for (const UPCGNode* Node : Graph->GetNodes())
	{
		if (Node)
		{
			Nodes.Add(MakeShared<FJsonValueObject>(SerializeNode(Graph, Node)));
		}
	}

	TArray<TSharedPtr<FJsonValue>> Edges;
	for (const UPCGEdge* Edge : Graph->GetAllEdges())
	{
		if (!Edge || !Edge->InputPin || !Edge->OutputPin)
		{
			continue;
		}

		// UPCGEdge::InputPin is the UPSTREAM end and OutputPin the DOWNSTREAM end
		// (see PCGEdge.h). Naming them from/to here so callers are not misled.
		TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
		EdgeObj->SetStringField(TEXT("from_node"), MakeNodeId(Graph, Edge->InputPin->Node));
		EdgeObj->SetStringField(TEXT("from_pin"), Edge->InputPin->Properties.Label.ToString());
		EdgeObj->SetStringField(TEXT("to_node"), MakeNodeId(Graph, Edge->OutputPin->Node));
		EdgeObj->SetStringField(TEXT("to_pin"), Edge->OutputPin->Properties.Label.ToString());
		Edges.Add(MakeShared<FJsonValueObject>(EdgeObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("node_count"), Nodes.Num());
	Result->SetArrayField(TEXT("nodes"), Nodes);
	Result->SetNumberField(TEXT("edge_count"), Edges.Num());
	Result->SetArrayField(TEXT("edges"), Edges);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGActions::AddPCGNode(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString RequestedClass = Params->GetStringField(TEXT("settings_class"));

	FString LoadError;
	UPCGGraph* Graph = LoadGraph(AssetPath, LoadError);
	if (!Graph)
	{
		return FMonolithActionResult::Error(LoadError);
	}

	// Resolve everything BEFORE opening a transaction: CancelTransaction discards the
	// undo record, not the changes, so a late failure would leave real damage behind.
	FString ClassError;
	UClass* SettingsClass = ResolveSettingsClass(RequestedClass, ClassError);
	if (!SettingsClass)
	{
		return FMonolithActionResult::Error(ClassError + TEXT(" NOTHING WAS CHANGED."));
	}

	FScopedTransaction Transaction(LOCTEXT("MonolithAddPCGNode", "Monolith: Add PCG Node"));
	Graph->Modify();

	UPCGSettings* DefaultNodeSettings = nullptr;
	UPCGNode* NewNode = Graph->AddNodeOfType(SettingsClass, DefaultNodeSettings);
	if (!NewNode)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("UPCGGraph::AddNodeOfType returned null for class '%s'. No node was added."),
			*SettingsClass->GetName()));
	}

#if WITH_EDITOR
	if (Params->HasTypedField<EJson::Number>(TEXT("position_x")) || Params->HasTypedField<EJson::Number>(TEXT("position_y")))
	{
		const int32 PosX = Params->HasTypedField<EJson::Number>(TEXT("position_x")) ? static_cast<int32>(Params->GetNumberField(TEXT("position_x"))) : 0;
		const int32 PosY = Params->HasTypedField<EJson::Number>(TEXT("position_y")) ? static_cast<int32>(Params->GetNumberField(TEXT("position_y"))) : 0;
		NewNode->SetNodePosition(PosX, PosY);
	}

	if (Params->HasTypedField<EJson::String>(TEXT("node_title")))
	{
		const FString Title = Params->GetStringField(TEXT("node_title"));
		if (!Title.IsEmpty())
		{
			NewNode->SetNodeTitle(FName(*Title), /*bApplySanitization=*/true);
		}
	}
#endif

	Graph->MarkPackageDirty();

	// Verify against the graph's own node list rather than trusting the return value.
	const bool bPresent = Graph->GetNodes().Contains(NewNode);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("node_id"), MakeNodeId(Graph, NewNode));
	Result->SetStringField(TEXT("settings_class"), SettingsClass->GetName());
	Result->SetBoolField(TEXT("present_in_graph"), bPresent);
	Result->SetObjectField(TEXT("node"), SerializeNode(Graph, NewNode));
	if (!bPresent)
	{
		Result->SetStringField(TEXT("warning"),
			TEXT("AddNodeOfType returned a node that is not in UPCGGraph::GetNodes(). Treat this result as unverified."));
	}
	Result->SetStringField(TEXT("note"), TEXT("Asset is dirty in memory; it is not saved by this action."));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGActions::ConnectPCGNodes(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString FromNodeId = Params->GetStringField(TEXT("from_node"));
	const FString ToNodeId = Params->GetStringField(TEXT("to_node"));
	const FString FromPinLabel = Params->GetStringField(TEXT("from_pin"));
	const FString ToPinLabel = Params->GetStringField(TEXT("to_pin"));

	FString LoadError;
	UPCGGraph* Graph = LoadGraph(AssetPath, LoadError);
	if (!Graph)
	{
		return FMonolithActionResult::Error(LoadError);
	}

	// --- full validation before any mutation ---
	FString NodeError;
	UPCGNode* FromNode = ResolveNode(Graph, FromNodeId, NodeError);
	if (!FromNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("from_node: %s NOTHING WAS CHANGED."), *NodeError));
	}
	UPCGNode* ToNode = ResolveNode(Graph, ToNodeId, NodeError);
	if (!ToNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("to_node: %s NOTHING WAS CHANGED."), *NodeError));
	}

	// Ask the engine which pins exist and on which side, instead of assuming a label is
	// valid because it looks plausible.
	const FName FromPinName(*FromPinLabel);
	const FName ToPinName(*ToPinLabel);

	UPCGPin* FromPin = FromNode->GetOutputPin(FromPinName);
	if (!FromPin)
	{
		TArray<FString> Labels;
		for (const UPCGPin* Pin : FromNode->GetOutputPins())
		{
			if (Pin) { Labels.Add(Pin->Properties.Label.ToString()); }
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Node '%s' has no OUTPUT pin '%s'. Its output pins are: [%s]. NOTHING WAS CHANGED."),
			*FromNodeId, *FromPinLabel, *FString::Join(Labels, TEXT(", "))));
	}

	UPCGPin* ToPin = ToNode->GetInputPin(ToPinName);
	if (!ToPin)
	{
		TArray<FString> Labels;
		for (const UPCGPin* Pin : ToNode->GetInputPins())
		{
			if (Pin) { Labels.Add(Pin->Properties.Label.ToString()); }
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Node '%s' has no INPUT pin '%s'. Its input pins are: [%s]. NOTHING WAS CHANGED."),
			*ToNodeId, *ToPinLabel, *FString::Join(Labels, TEXT(", "))));
	}

	const int32 TargetEdgesBefore = ToPin->Edges.Num();

	FScopedTransaction Transaction(LOCTEXT("MonolithConnectPCGNodes", "Monolith: Connect PCG Nodes"));
	Graph->Modify();

	Graph->AddEdge(FromNode, FromPinName, ToNode, ToPinName);

	// AddEdge returns the "To" node for chaining, so a non-null return is NOT evidence
	// that an edge exists. Prove it by walking the pin's own edge list.
	bool bEdgeExists = false;
	for (const UPCGEdge* Edge : FromPin->Edges)
	{
		if (Edge && Edge->OutputPin == ToPin)
		{
			bEdgeExists = true;
			break;
		}
	}

	if (!bEdgeExists)
	{
		// Nothing verified as connected. Say so; do not report a success.
		Transaction.Cancel();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("AddEdge did not produce an edge from '%s.%s' to '%s.%s'. The pins may be type-incompatible. ")
			TEXT("Note that the transaction was cancelled, which discards the undo record but does NOT roll back ")
			TEXT("any change the engine already made - call pcg.list_pcg_nodes to see the graph's actual edge list."),
			*FromNodeId, *FromPinLabel, *ToNodeId, *ToPinLabel));
	}

	Graph->MarkPackageDirty();

	const int32 TargetEdgesAfter = ToPin->Edges.Num();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("from_node"), MakeNodeId(Graph, FromNode));
	Result->SetStringField(TEXT("from_pin"), FromPin->Properties.Label.ToString());
	Result->SetStringField(TEXT("to_node"), MakeNodeId(Graph, ToNode));
	Result->SetStringField(TEXT("to_pin"), ToPin->Properties.Label.ToString());
	Result->SetBoolField(TEXT("edge_verified"), true);
	Result->SetNumberField(TEXT("target_pin_edges_before"), TargetEdgesBefore);
	Result->SetNumberField(TEXT("target_pin_edges_after"), TargetEdgesAfter);

	if (TargetEdgesAfter <= TargetEdgesBefore && TargetEdgesBefore > 0)
	{
		// Single-connection pins silently drop the previous edge; that is a real side
		// effect the caller needs to know about.
		Result->SetStringField(TEXT("side_effect"),
			TEXT("The target pin's edge count did not increase - it does not allow multiple connections, so a pre-existing edge was replaced."));
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGActions::SetPCGNodeSetting(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString NodeId = Params->GetStringField(TEXT("node_id"));
	const FString PropertyName = Params->GetStringField(TEXT("property"));

	FString LoadError;
	UPCGGraph* Graph = LoadGraph(AssetPath, LoadError);
	if (!Graph)
	{
		return FMonolithActionResult::Error(LoadError);
	}

	FString NodeError;
	UPCGNode* Node = ResolveNode(Graph, NodeId, NodeError);
	if (!Node)
	{
		return FMonolithActionResult::Error(NodeError + TEXT(" NOTHING WAS CHANGED."));
	}

	UPCGSettings* Settings = Node->GetSettings();
	if (!Settings)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Node '%s' has no settings object to edit. NOTHING WAS CHANGED."), *NodeId));
	}

	const bool bAllowShared = Params->HasTypedField<EJson::Boolean>(TEXT("allow_shared_settings"))
		&& Params->GetBoolField(TEXT("allow_shared_settings"));
	if (Node->IsInstance() && !bAllowShared)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Node '%s' instances a shared UPCGSettings asset ('%s'); writing to it would change every node that uses it. ")
			TEXT("Pass allow_shared_settings=true to proceed deliberately. NOTHING WAS CHANGED."),
			*NodeId, *Settings->GetPathName()));
	}

	FProperty* Property = Settings->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Property)
	{
		TArray<FString> Available;
		for (TFieldIterator<FProperty> It(Settings->GetClass()); It && Available.Num() < 40; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Edit))
			{
				Available.Add(It->GetName());
			}
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("'%s' has no property '%s'. Editable properties include: [%s]. NOTHING WAS CHANGED."),
			*Settings->GetClass()->GetName(), *PropertyName, *FString::Join(Available, TEXT(", "))));
	}

	const TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("'value' is required. NOTHING WAS CHANGED."));
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Settings);

	// Whatever is validated below is the exact thing written - no separate
	// "validate one representation, ship another" step.
	auto MakeTypeError = [&](const TCHAR* Expected) -> FMonolithActionResult
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Property '%s' is of type '%s' and needs a %s value. NOTHING WAS CHANGED."),
			*PropertyName, *Property->GetCPPType(), Expected));
	};

	FScopedTransaction Transaction(LOCTEXT("MonolithSetPCGNodeSetting", "Monolith: Set PCG Node Setting"));
	Settings->Modify();

	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		bool BoolValue = false;
		if (!ValueField->TryGetBool(BoolValue)) { Transaction.Cancel(); return MakeTypeError(TEXT("boolean")); }
		BoolProp->SetPropertyValue(ValuePtr, BoolValue);
	}
	else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		FString EnumStr;
		if (!ValueField->TryGetString(EnumStr)) { Transaction.Cancel(); return MakeTypeError(TEXT("string (enum name)")); }

		// EGetByNameFlags has no CaseInsensitive member - the DEFAULT (None) is already
		// case-insensitive and CaseSensitive is the opt-in (UObject/Class.h).
		const UEnum* Enum = EnumProp->GetEnum();
		const int64 EnumValue = Enum ? Enum->GetValueByNameString(EnumStr) : INDEX_NONE;
		if (!Enum || EnumValue == INDEX_NONE)
		{
			TArray<FString> ValidNames;
			if (Enum)
			{
				for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
				{
					ValidNames.Add(Enum->GetNameStringByIndex(Index));
				}
			}
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("'%s' is not a valid value for enum property '%s'. Valid values: [%s]. NOTHING WAS CHANGED."),
				*EnumStr, *PropertyName, *FString::Join(ValidNames, TEXT(", "))));
		}
		EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumValue);
	}
	else if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
	{
		if (UEnum* Enum = ByteProp->Enum)
		{
			FString EnumStr;
			if (!ValueField->TryGetString(EnumStr)) { Transaction.Cancel(); return MakeTypeError(TEXT("string (enum name)")); }

			const int64 EnumValue = Enum->GetValueByNameString(EnumStr);
			if (EnumValue == INDEX_NONE)
			{
				TArray<FString> ValidNames;
				for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
				{
					ValidNames.Add(Enum->GetNameStringByIndex(Index));
				}
				Transaction.Cancel();
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("'%s' is not a valid value for enum property '%s'. Valid values: [%s]. NOTHING WAS CHANGED."),
					*EnumStr, *PropertyName, *FString::Join(ValidNames, TEXT(", "))));
			}
			ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(EnumValue));
		}
		else
		{
			double NumberValue = 0.0;
			if (!ValueField->TryGetNumber(NumberValue)) { Transaction.Cancel(); return MakeTypeError(TEXT("number")); }
			ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(NumberValue));
		}
	}
	else if (FIntProperty* IntProp = CastField<FIntProperty>(Property))
	{
		double NumberValue = 0.0;
		if (!ValueField->TryGetNumber(NumberValue)) { Transaction.Cancel(); return MakeTypeError(TEXT("number")); }
		IntProp->SetPropertyValue(ValuePtr, static_cast<int32>(NumberValue));
	}
	else if (FInt64Property* Int64Prop = CastField<FInt64Property>(Property))
	{
		double NumberValue = 0.0;
		if (!ValueField->TryGetNumber(NumberValue)) { Transaction.Cancel(); return MakeTypeError(TEXT("number")); }
		Int64Prop->SetPropertyValue(ValuePtr, static_cast<int64>(NumberValue));
	}
	else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
	{
		double NumberValue = 0.0;
		if (!ValueField->TryGetNumber(NumberValue)) { Transaction.Cancel(); return MakeTypeError(TEXT("number")); }
		FloatProp->SetPropertyValue(ValuePtr, static_cast<float>(NumberValue));
	}
	else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Property))
	{
		double NumberValue = 0.0;
		if (!ValueField->TryGetNumber(NumberValue)) { Transaction.Cancel(); return MakeTypeError(TEXT("number")); }
		DoubleProp->SetPropertyValue(ValuePtr, NumberValue);
	}
	else if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		FString StringValue;
		if (!ValueField->TryGetString(StringValue)) { Transaction.Cancel(); return MakeTypeError(TEXT("string")); }
		StrProp->SetPropertyValue(ValuePtr, StringValue);
	}
	else if (FNameProperty* NameProp = CastField<FNameProperty>(Property))
	{
		FString StringValue;
		if (!ValueField->TryGetString(StringValue)) { Transaction.Cancel(); return MakeTypeError(TEXT("string")); }
		NameProp->SetPropertyValue(ValuePtr, FName(*StringValue));
	}
	else
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Property '%s' has type '%s', which this action does not support (supported: bool, integer, float/double, string, name, enum). NOTHING WAS CHANGED."),
			*PropertyName, *Property->GetCPPType()));
	}

	// Let the engine propagate the change the way it does for a UI edit - this is what
	// updates dynamic pins and marks the node dirty.
	FPropertyChangedEvent ChangedEvent(Property);
	// UPCGSettings re-declares PostEditChangeProperty as PROTECTED (PCGSettings.h:541), so it is
	// inaccessible through a UPCGSettings*. UObject's declaration is public, and the call is
	// virtual, so dispatching through a UObject* reaches UPCGSettings' override anyway. Same
	// base-pointer trick MonolithNiagara uses for UNiagaraNode::AllocateDefaultPins.
	static_cast<UObject*>(Settings)->PostEditChangeProperty(ChangedEvent);

	Graph->MarkPackageDirty();

	// Read the value back OFF THE PROPERTY. This is the stored state, not an echo of the
	// request - it is what catches a write that was silently discarded. It is still not
	// behavioural proof; only pcg.execute_pcg_graph gives that.
	FString ValueAfter;
	Property->ExportTextItem_Direct(ValueAfter, ValuePtr, nullptr, Settings, PPF_None);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("node_id"), MakeNodeId(Graph, Node));
	Result->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
	Result->SetStringField(TEXT("property"), Property->GetName());
	Result->SetStringField(TEXT("property_type"), Property->GetCPPType());
	Result->SetStringField(TEXT("value_after"), ValueAfter);
	Result->SetStringField(TEXT("verification"),
		TEXT("'value_after' was exported from the property itself after the write. Stored state only - run pcg.execute_pcg_graph to prove the setting changes output."));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGActions::ListPCGNodeTypes(const TSharedPtr<FJsonObject>& Params)
{
	FString Filter;
	if (Params->HasTypedField<EJson::String>(TEXT("filter")))
	{
		Filter = Params->GetStringField(TEXT("filter"));
	}

	int32 Limit = 200;
	if (Params->HasTypedField<EJson::Number>(TEXT("limit")))
	{
		Limit = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("limit"))), 1, 2000);
	}

	TArray<TSharedPtr<FJsonValue>> Types;
	int32 TotalMatches = 0;

	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Candidate = *ClassIt;
		if (!IsUsableSettingsClass(Candidate))
		{
			continue;
		}

		TArray<FString> Aliases;
		GetSettingsClassAliases(Candidate, Aliases);

		if (!Filter.IsEmpty())
		{
			bool bMatches = false;
			for (const FString& Alias : Aliases)
			{
				if (Alias.Contains(Filter, ESearchCase::IgnoreCase))
				{
					bMatches = true;
					break;
				}
			}
			if (!bMatches)
			{
				continue;
			}
		}

		++TotalMatches;
		if (Types.Num() >= Limit)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("settings_class"), Candidate->GetName());

#if WITH_EDITOR
		if (const UPCGSettings* CDO = Cast<UPCGSettings>(Candidate->GetDefaultObject()))
		{
			Obj->SetStringField(TEXT("default_node_name"), CDO->GetDefaultNodeName().ToString());
			Obj->SetStringField(TEXT("default_node_title"), CDO->GetDefaultNodeTitle().ToString());
		}
#endif

		TArray<TSharedPtr<FJsonValue>> AliasValues;
		for (const FString& Alias : Aliases)
		{
			AliasValues.Add(MakeShared<FJsonValueString>(Alias));
		}
		Obj->SetArrayField(TEXT("accepted_names"), AliasValues);

		Types.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("match_count"), TotalMatches);
	Result->SetNumberField(TEXT("returned_count"), Types.Num());
	Result->SetArrayField(TEXT("node_types"), Types);
	if (TotalMatches > Types.Num())
	{
		Result->SetStringField(TEXT("truncation_note"),
			FString::Printf(TEXT("%d of %d matches returned - narrow with 'filter' or raise 'limit'."), Types.Num(), TotalMatches));
	}

	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithPCGActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("pcg"), TEXT("execute_pcg_graph"),
		TEXT("Start a headless execution of a PCG graph (no world/actor/component). Asynchronous - returns an execution_id to poll with get_pcg_output_summary"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::ExecutePCGGraph),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PCG graph asset path"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Generation seed"), TEXT("42"))
			.Build());

	Registry.RegisterAction(TEXT("pcg"), TEXT("get_pcg_output_summary"),
		TEXT("Read the executed output of a PCG graph: per-pin data class, point counts, bounds and metadata attributes. This is the ground-truth instrument"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::GetPCGOutputSummary),
		FParamSchemaBuilder()
			.Optional(TEXT("execution_id"), TEXT("string"), TEXT("Id returned by execute_pcg_graph"))
			.OptionalAssetPath(TEXT("asset_path"), TEXT("Graph path - resolves to that graph's most recent execution this session"))
			.Build());

	Registry.RegisterAction(TEXT("pcg"), TEXT("create_pcg_graph"),
		TEXT("Create a new empty UPCGGraph asset (input and output nodes are created by the engine constructor)"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::CreatePCGGraph),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Destination asset path, e.g. /Game/FX/_Probes/PCG_Test_Graph"))
			.Optional(TEXT("usage"), TEXT("string"), TEXT("EPCGGraphUsage name: Standard, Asset or Level"), TEXT("Standard"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the asset to disk immediately"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("pcg"), TEXT("list_pcg_nodes"),
		TEXT("List every node in a PCG graph (including the input/output terminals) with its pins, plus the graph's full edge list"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::ListPCGNodes),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PCG graph asset path"))
			.Build());

	Registry.RegisterAction(TEXT("pcg"), TEXT("add_pcg_node"),
		TEXT("Add a node of a given UPCGSettings class to a PCG graph"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::AddPCGNode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PCG graph asset path"))
			.Required(TEXT("settings_class"), TEXT("string"), TEXT("UPCGSettings subclass name, short name, or node name - see list_pcg_node_types"))
			.Optional(TEXT("node_title"), TEXT("string"), TEXT("Authored node title"))
			.Optional(TEXT("position_x"), TEXT("integer"), TEXT("Graph editor X position"))
			.Optional(TEXT("position_y"), TEXT("integer"), TEXT("Graph editor Y position"))
			.Build());

	Registry.RegisterAction(TEXT("pcg"), TEXT("connect_pcg_nodes"),
		TEXT("Connect an output pin to an input pin by label. Refuses before mutating if either node or pin does not exist, and verifies the edge afterwards"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::ConnectPCGNodes),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PCG graph asset path"))
			.Required(TEXT("from_node"), TEXT("string"), TEXT("Upstream node id, or 'input'"))
			.Required(TEXT("from_pin"), TEXT("string"), TEXT("Output pin label on the upstream node"))
			.Required(TEXT("to_node"), TEXT("string"), TEXT("Downstream node id, or 'output'"))
			.Required(TEXT("to_pin"), TEXT("string"), TEXT("Input pin label on the downstream node"))
			.Build());

	Registry.RegisterAction(TEXT("pcg"), TEXT("set_pcg_node_setting"),
		TEXT("Set one reflected property on a node's UPCGSettings. Validates enums against the real UEnum and reports the value read back off the property"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::SetPCGNodeSetting),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PCG graph asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node id from list_pcg_nodes, or 'input'/'output'"))
			.Required(TEXT("property"), TEXT("string"), TEXT("Property name on the settings class"))
			.Required(TEXT("value"), TEXT("string"), TEXT("New value. Send a JSON bool for bool properties, a JSON number for numeric ones, and a string for string/name/enum properties"))
			.Optional(TEXT("allow_shared_settings"), TEXT("bool"), TEXT("Permit writing to settings shared by other nodes"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("pcg"), TEXT("list_pcg_node_types"),
		TEXT("Enumerate the UPCGSettings subclasses that add_pcg_node accepts, with every name spelling each one answers to"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::ListPCGNodeTypes),
		FParamSchemaBuilder()
			.Optional(TEXT("filter"), TEXT("string"), TEXT("Case-insensitive substring filter"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum entries to return"), TEXT("200"))
			.Build());
}

#undef LOCTEXT_NAMESPACE

#endif // WITH_PCG
