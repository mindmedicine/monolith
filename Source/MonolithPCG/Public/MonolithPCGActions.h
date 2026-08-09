#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

#if WITH_PCG

class UPCGEdge;
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

	// --- Teardown. The other half of the authoring set: without these, a bad edge or a
	// wrong node is unrepairable through this namespace and the caller has to fall back to
	// editor.run_python. Both prove their blast radius against the graph's own edge list
	// after the fact, not against what they intended to touch.
	static FMonolithActionResult RemovePCGEdge(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemovePCGNode(const TSharedPtr<FJsonObject>& Params);

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

	/**
	 * Canonical, comparable identity for one edge: "fromNode.fromPin -> toNode.toPin".
	 * Returns an empty string for an edge missing either end. Used to diff the graph's edge
	 * list before and after a removal - a set difference computed from the graph itself is
	 * external to whatever the action intended to do, which is the only kind of scope proof
	 * that is worth anything (gap #37).
	 */
	static FString MakeEdgeKey(const UPCGGraph* InGraph, const UPCGEdge* InEdge);

	/** Every edge in the graph, as MakeEdgeKey strings. */
	static void SnapshotEdgeKeys(const UPCGGraph* InGraph, TArray<FString>& OutKeys);

	/**
	 * True when a directed path InTo -> ... -> InFrom already exists, i.e. when adding an
	 * edge InFrom -> InTo would close a cycle. OutPath is filled with that existing path in
	 * data-flow order (InTo first, InFrom last) so the error can name it.
	 *
	 * Mirrors the engine's own connection check (UPCGEditorGraphNodeBase::IsCompatible,
	 * PCGEditorGraphNodeBase.cpp:1232-1287), which walks UPSTREAM from the proposed source
	 * looking for the proposed destination. That check lives in the editor graph layer and
	 * is never reached by UPCGGraph::AddEdge, which is why an MCP caller could author a
	 * cycle the UI refuses.
	 *
	 * ITERATIVE on purpose - the engine's version recurses. A recursive detector blows the
	 * stack on exactly the graphs it exists to catch, and the visited set here also makes it
	 * terminate on a graph that is ALREADY cyclic, which is the state a caller most needs it
	 * to work in.
	 */
	static bool FindUpstreamPath(const UPCGNode* InFrom, const UPCGNode* InTo, TArray<const UPCGNode*>& OutPath);
};

#endif // WITH_PCG
