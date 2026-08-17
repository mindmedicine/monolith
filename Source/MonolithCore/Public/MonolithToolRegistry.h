#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/** Result of an action execution */
struct FMonolithActionResult
{
	bool bSuccess = false;
	TSharedPtr<FJsonObject> Result;
	FString ErrorMessage;
	int32 ErrorCode = 0;

	// Survivor C (plan §3.C) — optional structured payload attached to error
	// responses. Currently used to carry `error.data.suggestions` (top-3
	// did-you-mean candidates) on Unknown-action / Unknown-namespace errors;
	// available for any future structured-error use. Null on Success and on
	// errors that don't carry structured data.
	TSharedPtr<FJsonValue> ErrorData;

	// Gap #111 — the ONE structured channel for non-fatal cautions.
	//
	// Before this existed there was no way for a handler to say "attach this warning":
	// 168 emission sites hand-rolled it, 58 of them onto a singular `warning` STRING that
	// FMonolithToolRegistry::ExecuteAction's merge (which reads `warnings` as an ARRAY)
	// cannot see, and gap #88 tells every reader to check the array. Anything added here
	// is merged into the response's `warnings[]` on success and APPENDED TO ErrorMessage
	// on refusal — see the merge block in ExecuteAction for why the refusal path is text.
	//
	// Deliberately NOT a replacement for writing `warnings[]` onto the Result object: a
	// handler reachable through niagara.batch_execute's direct function-pointer table
	// bypasses ExecuteAction entirely, and only the batch's own merge (which folds this
	// array in) makes the structured channel visible there. When in doubt for a batchable
	// action, FMonolithJsonUtils::AddWarning onto the Result object is the safer channel.
	TArray<FString> Warnings;

	static FMonolithActionResult Success(const TSharedPtr<FJsonObject>& InResult)
	{
		FMonolithActionResult R;
		R.bSuccess = true;
		R.Result = InResult;
		return R;
	}

	static FMonolithActionResult Error(const FString& Message, int32 Code = -32603)
	{
		FMonolithActionResult R;
		R.bSuccess = false;
		R.ErrorMessage = Message;
		R.ErrorCode = Code;
		return R;
	}

	/** Survivor C — attach a JSON-object data payload to an existing error. */
	FMonolithActionResult& WithErrorData(const TSharedPtr<FJsonObject>& Data)
	{
		if (Data.IsValid()) { ErrorData = MakeShared<FJsonValueObject>(Data); }
		else { ErrorData.Reset(); }
		return *this;
	}

	/**
	 * Gap #111 — attach a non-fatal caution to this result. Fluent, so it chains onto
	 * either factory:
	 *
	 *     return FMonolithActionResult::Success(Obj).WithWarning(TEXT("coerced X to Y"));
	 *     return FMonolithActionResult::Error(TEXT("refused")).WithWarning(TEXT("...also, X"));
	 *
	 * Empty strings are IGNORED, so a call site may pass a conditionally-built string
	 * without guarding it. Order is preserved and duplicates are NOT collapsed: two
	 * warnings that happen to read alike are usually about two different things, and
	 * silently dropping the second is exactly the clobber this channel exists to end.
	 */
	FMonolithActionResult& WithWarning(const FString& InWarning)
	{
		if (!InWarning.IsEmpty()) { Warnings.Add(InWarning); }
		return *this;
	}

	/** Gap #111 — attach several cautions at once. Empty entries are ignored, as above. */
	FMonolithActionResult& WithWarnings(const TArray<FString>& InWarnings)
	{
		for (const FString& W : InWarnings) { WithWarning(W); }
		return *this;
	}

	/**
	 * Gap #110/#111 — render warnings as a text block for the REFUSAL path.
	 *
	 * A failed action has no `Result` object to hang `warnings[]` on, and the tools/call
	 * transport gives an error response no structured field at all (MonolithHttpServer's
	 * error branch emits `ErrorMessage` and `isError` and nothing else), so on that path
	 * the warnings ride in the message. Text is a downgrade from structure; being silently
	 * destroyed — which is what happened before — is worse.
	 *
	 * Returns an EMPTY string when there are no warnings, so call sites can append
	 * unconditionally without manufacturing an empty "Warnings (0)" suffix.
	 */
	static FString FormatWarningBlock(const TArray<FString>& InWarnings)
	{
		if (InWarnings.Num() == 0) { return FString(); }
		FString Block;
		for (const FString& W : InWarnings)
		{
			Block += TEXT("\n  - ");
			Block += W;
		}
		return FString::Printf(
			TEXT("\n\nWarnings also raised on this call (%d) — carried in this message because a FAILED call has no warnings[] channel:%s"),
			InWarnings.Num(), *Block);
	}

	/**
	 * Twin of FormatWarningBlock for the REFUSAL path — renders ErrorData as text.
	 *
	 * Same layer, same reason, different field. An MCP `tools/call` error response is
	 * `content[] + isError` and NOTHING ELSE (see MonolithHttpServer's projection), so a
	 * structured payload attached to a refusal had nowhere to go and was destroyed at the
	 * transport on every one of the ~42 reachable error paths that set it. One of those
	 * paths — animation.set_transition_rule's compile-error rollback — had already undone
	 * its transaction and recompiled the Blueprint clean, so the diagnostics it harvested
	 * existed nowhere else by the time the caller read "See compile_errors".
	 *
	 * Rendered as a fenced JSON block so it stays machine-parseable inside prose: a client
	 * can lift it back out with a fence scan, which is not true of the equivalent sentence.
	 *
	 * Returns an EMPTY string when ErrorData is unset — call sites append unconditionally
	 * and an ordinary error message stays byte-for-byte unchanged.
	 *
	 * Deliberately NOT inline: defined in MonolithToolRegistry.cpp so the JSON serializer
	 * stays out of this header, which nearly every action .cpp includes.
	 */
	static MONOLITHCORE_API FString FormatErrorDataBlock(const TSharedPtr<FJsonValue>& InErrorData);
};

/** Delegate type for action handlers */
DECLARE_DELEGATE_RetVal_OneParam(FMonolithActionResult, FMonolithActionHandler, const TSharedPtr<FJsonObject>& /* Params */);

/** Metadata describing a registered action */
struct FMonolithActionInfo
{
	FString Namespace;
	FString Action;
	FString Description;
	FString Category;                     // Optional sub-grouping within a namespace (e.g. "CommonUI" inside "ui"). Empty = uncategorized.
	TSharedPtr<FJsonObject> ParamSchema;  // JSON Schema for parameter validation

	// Survivor A (plan §3.A) — MCP-spec tool annotation hints. Only emitted on
	// `tools/list` when at least one is non-default; per-call runtime cost is zero.
	// For individually-registered tools (`monolith_*`) the hints are read from the
	// action's own info. For namespace dispatcher tools (`*_query`) the hints come
	// from FMonolithToolRegistry::GetDispatcherAnnotations() — see that path for
	// the rationale (sibling actions in the same dispatcher can disagree on
	// destructive/read-only, so the dispatcher-level annotation is authoritative).
	bool bReadOnlyHint   = false;
	bool bDestructiveHint = false;
	bool bIdempotentHint = false;
	FString Title;
};

/**
 * Survivor A (plan §3.A) — per-namespace dispatcher annotations.
 * Used for namespace dispatcher tools (`source_query` etc.) where the four MCP
 * hint fields apply to the WHOLE dispatcher rather than any single action. Held
 * separately from FMonolithActionInfo because the dispatcher is not a
 * registered action — it is synthesised inside HandleToolsList at serialize time.
 */
struct FMonolithDispatcherAnnotations
{
	bool bReadOnlyHint   = false;
	bool bDestructiveHint = false;
	bool bIdempotentHint = false;
	FString Title;

	/** Helper: true iff any hint is non-default. Drives "do we emit annotations on the wire?" */
	bool IsAnyNonDefault() const
	{
		return bReadOnlyHint || bDestructiveHint || bIdempotentHint || !Title.IsEmpty();
	}
};

/**
 * Central registry for all Monolith tool actions.
 * Domain modules register actions here. The HTTP server dispatches through this.
 */
class MONOLITHCORE_API FMonolithToolRegistry
{
public:
	static FMonolithToolRegistry& Get();

	/**
	 * Register an action handler.
	 * @param Namespace   The tool namespace (e.g., "blueprint", "material")
	 * @param Action      The action name (e.g., "list_graphs", "get_node")
	 * @param Description Human-readable description of what this action does
	 * @param Handler     The delegate to execute
	 * @param ParamSchema Optional JSON Schema describing expected parameters
	 */
	void RegisterAction(
		const FString& Namespace,
		const FString& Action,
		const FString& Description,
		const FMonolithActionHandler& Handler,
		const TSharedPtr<FJsonObject>& ParamSchema = nullptr,
		const FString& Category = FString()  // Optional sub-group within namespace — defaults to uncategorized
	);

	/** Unregister all actions in a namespace (called during module shutdown) */
	void UnregisterNamespace(const FString& Namespace);

	/** Execute an action by namespace + action name */
	FMonolithActionResult ExecuteAction(const FString& Namespace, const FString& Action, const TSharedPtr<FJsonObject>& Params);

	/**
	 * Validate and normalise a param object against a registered action's schema WITHOUT
	 * dispatching it: the same declared-alias rewrite and required-param check ExecuteAction
	 * performs, and the same error text, exposed for dispatchers that invoke handlers directly.
	 *
	 * Exists for gap #72. batch_execute dispatches its sub-ops through a function-pointer
	 * table, so nothing about them ever reached ExecuteAction and neither validation nor alias
	 * rewriting ran: a sub-op naming a param wrongly reached the handler with the param simply
	 * absent, and came back with the handler's guess at what was wrong ("Emitter not found")
	 * instead of the dispatch layer's fact ("Missing required param(s): [emitter]").
	 *
	 * `Params` is normalised IN PLACE — declared aliases are rewritten to their canonical
	 * spelling, so the handler receives exactly what a top-level call would hand it.
	 *
	 * Deliberately does NOT run the K3 unknown-key check, the K4 string-decode, or any
	 * response shaping. Those mutate the result envelope, and a batch's envelope is its
	 * caller-visible contract; this call is validation only.
	 *
	 * Returns SUCCESS when the params are acceptable AND when the action is not registered at
	 * all. Failing open on an unknown action is deliberate: a dispatcher may know op spellings
	 * the registry does not, and it should answer for those with its own unknown-op error
	 * rather than have this one contradict it. Call HasAction() first if that matters.
	 */
	FMonolithActionResult ValidateActionParams(const FString& Namespace, const FString& Action, const TSharedPtr<FJsonObject>& Params) const;

	/** Get all registered namespaces */
	TArray<FString> GetNamespaces() const;

	/** Get all actions in a namespace */
	TArray<FMonolithActionInfo> GetActions(const FString& Namespace) const;

	/** Get all actions across all namespaces */
	TArray<FMonolithActionInfo> GetAllActions() const;

	/** Check if a specific action exists */
	bool HasAction(const FString& Namespace, const FString& Action) const;

	/** Get total number of registered actions */
	int32 GetActionCount() const;

	/**
	 * Survivor A (plan §3.A) — Set MCP hint annotations for a namespace dispatcher
	 * tool (e.g. `source_query`). These are serialized into `tools/list` under the
	 * dispatcher tool's `annotations` sub-object. Only namespaces whose dispatcher
	 * is audited as safe (read-only / idempotent) should call this. Defaults are
	 * preserved when no call is made — so untagged dispatchers stay defaulted.
	 *
	 * Thread-safe: takes RegistryLock internally.
	 */
	void SetDispatcherAnnotations(const FString& Namespace, const FMonolithDispatcherAnnotations& Annotations);

	/**
	 * Survivor A — Look up dispatcher annotations for a namespace. Returns a
	 * default-constructed (all-false / empty-title) struct if the namespace was
	 * never annotated. Used by HandleToolsList to decide whether to emit the
	 * MCP `annotations` block.
	 */
	FMonolithDispatcherAnnotations GetDispatcherAnnotations(const FString& Namespace) const;

	/**
	 * Survivor A (plan §3.A) — Set MCP hint annotations on an already-registered
	 * action. Used for individually-registered top-level tools (`monolith_discover`,
	 * `monolith_status`, etc.). No-op if the action is not registered (safe to
	 * call defensively at module init order boundaries).
	 *
	 * Thread-safe: takes RegistryLock internally.
	 */
	void SetActionAnnotations(
		const FString& Namespace,
		const FString& Action,
		bool bReadOnly,
		bool bDestructive,
		bool bIdempotent,
		const FString& Title);

private:
	FMonolithToolRegistry() = default;

	struct FRegisteredAction
	{
		FMonolithActionInfo Info;
		FMonolithActionHandler Handler;
	};

	/** Map of "namespace.action" → registered action */
	TMap<FString, FRegisteredAction> Actions;

	/** Map of namespace → list of action keys */
	TMap<FString, TArray<FString>> NamespaceActions;

	/** Survivor A — Map of namespace → dispatcher-level MCP hint annotations. */
	TMap<FString, FMonolithDispatcherAnnotations> DispatcherAnnotations;

	static FString MakeKey(const FString& Namespace, const FString& Action)
	{
		return Namespace + TEXT(".") + Action;
	}

	mutable FCriticalSection RegistryLock;
};
