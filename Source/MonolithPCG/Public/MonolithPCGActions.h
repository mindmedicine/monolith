#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

#if WITH_PCG

class UPCGGraph;
class UPCGNode;
class UPCGSettings;

/**
 * PCG domain action handlers for Monolith (namespace: "pcg").
 *
 * Deliberately small. The two execution actions exist so that everything else is
 * verifiable against executed output rather than against stored settings — read-back of
 * a value we just wrote proves nothing.
 */
class FMonolithPCGActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// --- Ground truth (build and trust these first) ---
	static FMonolithActionResult ExecutePCGGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetPCGOutputSummary(const TSharedPtr<FJsonObject>& Params);

	// --- Minimum authoring set ---
	static FMonolithActionResult CreatePCGGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListPCGNodes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddPCGNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ConnectPCGNodes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetPCGNodeSetting(const TSharedPtr<FJsonObject>& Params);

	// --- Discovery (makes add_pcg_node usable without guessing class names) ---
	static FMonolithActionResult ListPCGNodeTypes(const TSharedPtr<FJsonObject>& Params);

private:
	/** Load a UPCGGraph, or return nullptr. OutError is always populated on failure. */
	static UPCGGraph* LoadGraph(const FString& InAssetPath, FString& OutError);

	/**
	 * Resolve a node reference within a graph.
	 * Accepted forms: the node's object name (as returned by list_pcg_nodes), or the
	 * reserved ids "input" / "output" for the graph's terminal nodes.
	 * Returns nullptr and fills OutError (including the list of valid ids) on a miss.
	 */
	static UPCGNode* ResolveNode(UPCGGraph* InGraph, const FString& InNodeId, FString& OutError);

	/** Stable id for a node: "input", "output", or its object name. */
	static FString MakeNodeId(const UPCGGraph* InGraph, const UPCGNode* InNode);

	/**
	 * Resolve a UPCGSettings subclass from a caller-supplied string. Matches (case
	 * insensitively) the UClass name, the UClass name minus the leading "PCG" and
	 * trailing "Settings", and the CDO's GetDefaultNodeName(). Never guesses: on a miss
	 * OutError names the closest candidates that actually exist.
	 */
	static UClass* ResolveSettingsClass(const FString& InRequested, FString& OutError);

	/** Serialize one node (id, title, class, pins, edges) for list_pcg_nodes. */
	static TSharedPtr<FJsonObject> SerializeNode(const UPCGGraph* InGraph, const UPCGNode* InNode);
};

#endif // WITH_PCG
