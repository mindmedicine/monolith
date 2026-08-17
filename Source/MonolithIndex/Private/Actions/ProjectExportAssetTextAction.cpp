#include "Actions/ProjectExportAssetTextAction.h"
#include "MonolithAssetUtils.h"
#include "MonolithGrepWithContext.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonValue.h"
#include "Exporters/Exporter.h"
#include "UnrealExporter.h"
#include "Misc/StringOutputDevice.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectMarks.h"

namespace
{
	// Default byte budget for a returned T3D dump (256 KB). T3D for a large,
	// fully-wired asset can be multiple MB -- the budget plus grep_pattern are
	// the safety rail (a silent mid-object truncation produces misleading grep
	// results, so we hard-error past the budget instead).
	constexpr int32 DefaultMaxBytes = 256 * 1024;

	// Absolute ceiling on the caller-supplied max_bytes. Asking past this is a
	// hard error -- the action is an inspection escape hatch, not a bulk dumper.
	constexpr int32 MaxBytesCeiling = 4 * 1024 * 1024;

	// The grep itself now lives in MonolithGrepWithContext.h so it can be unit-tested without
	// an asset or an exporter — see that header for what gap #101 was and was not.

	/** Locate a sub-object of Asset whose object name OR class name contains Filter (case-insensitive). */
	UObject* FindSubObjectByFilter(UObject* Asset, const FString& Filter, TArray<FString>& OutCandidates)
	{
		TArray<UObject*> Inners;
		GetObjectsWithOuter(Asset, Inners, /*bIncludeNestedObjects=*/true);

		const FString FilterLower = Filter.ToLower();
		UObject* Match = nullptr;
		for (UObject* Inner : Inners)
		{
			if (!Inner)
			{
				continue;
			}
			OutCandidates.Add(FString::Printf(TEXT("%s (%s)"), *Inner->GetName(), *Inner->GetClass()->GetName()));
			if (!Match)
			{
				const bool bNameHit = Inner->GetName().ToLower().Contains(FilterLower);
				const bool bClassHit = Inner->GetClass()->GetName().ToLower().Contains(FilterLower);
				if (bNameHit || bClassHit)
				{
					Match = Inner;
				}
			}
		}
		return Match;
	}
}

FString FProjectExportAssetTextAction::GetDescription()
{
	return TEXT("Universal escape hatch: export an asset to its native T3D text dump (or grepped excerpts) "
		"and return it directly. PREFER the typed read actions first -- get_node_details for Blueprint/AnimGraph "
		"nodes, inspect_chooser for chooser tables, list_graphs for graph structure. Reach for export_asset_text "
		"only when no typed action exposes the surface you need. T3D for a large asset is big: scope with "
		"object_filter and/or narrow with grep_pattern, and respect max_bytes (hard error past the budget rather "
		"than silent truncation).");
}

FMonolithActionResult FProjectExportAssetTextAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'asset_path' parameter is required"), -32602);
	}

	// Resolve max_bytes (optional). Hard-error if the caller asks past the ceiling.
	int32 MaxBytes = DefaultMaxBytes;
	double MaxBytesRaw = 0.0;
	if (Params->TryGetNumberField(TEXT("max_bytes"), MaxBytesRaw))
	{
		MaxBytes = static_cast<int32>(MaxBytesRaw);
		if (MaxBytes <= 0)
		{
			return FMonolithActionResult::Error(TEXT("'max_bytes' must be a positive integer"), -32602);
		}
		if (MaxBytes > MaxBytesCeiling)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("'max_bytes' (%d) exceeds the ceiling of %d. Scope the export with object_filter and/or "
					"grep_pattern instead of raising the budget."), MaxBytes, MaxBytesCeiling), -32602);
		}
	}

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	// Optional: scope to a sub-object by name/class substring.
	UObject* ExportRoot = Asset;
	FString ResolvedObject;
	const FString ObjectFilter = Params->GetStringField(TEXT("object_filter"));
	if (!ObjectFilter.IsEmpty())
	{
		TArray<FString> Candidates;
		UObject* SubObject = FindSubObjectByFilter(Asset, ObjectFilter, Candidates);
		if (!SubObject)
		{
			FString Hint = Candidates.Num() > 0
				? FString::Printf(TEXT(" Available sub-objects: %s"),
					*FString::Join(Candidates.Num() > 30 ? TArray<FString>(Candidates.GetData(), 30) : Candidates, TEXT(", ")))
				: FString(TEXT(" (asset has no sub-objects)"));
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("No sub-object matched object_filter '%s' in %s.%s"), *ObjectFilter, *AssetPath, *Hint));
		}
		ExportRoot = SubObject;
		ResolvedObject = FString::Printf(TEXT("%s (%s)"), *SubObject->GetName(), *SubObject->GetClass()->GetName());
	}

	// Native T3D export to string (established copy/clipboard pattern:
	// UnMarkAllObjects then ExportToOutputDevice into an FStringOutputDevice).
	UnMarkAllObjects(EObjectMark(OBJECTMARK_TagExp | OBJECTMARK_TagImp));
	FStringOutputDevice Buffer;
	const FExportObjectInnerContext Context;
	UExporter::ExportToOutputDevice(&Context, ExportRoot, /*InExporter=*/nullptr, Buffer,
		TEXT("T3D"), /*Indent=*/0, PPF_Copy, /*bInSelectedOnly=*/false);

	FString FullText = MoveTemp(Buffer);
	const int32 FullBytes = FullText.Len();

	// Optional grep narrowing.
	const FString GrepPattern = Params->GetStringField(TEXT("grep_pattern"));
	FString PayloadText = FullText;
	MonolithGrepWithContext::FGrepResult Grep;
	const bool bGrepped = !GrepPattern.IsEmpty();
	if (bGrepped)
	{
		Grep = MonolithGrepWithContext::GrepWithContext(FullText, GrepPattern);
		PayloadText = Grep.Text;
	}

	const int32 PayloadBytes = PayloadText.Len();

	// Byte-budget enforcement: hard error rather than silent truncation.
	if (PayloadBytes > MaxBytes)
	{
		FString Advice;
		if (!bGrepped)
		{
			Advice = TEXT(" Narrow it with grep_pattern, scope it with object_filter, or raise max_bytes (up to the ceiling).");
		}
		else
		{
			Advice = TEXT(" The grep_pattern still matches too much -- use a more specific pattern, add object_filter, or raise max_bytes (up to the ceiling).");
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Export payload is %d bytes, over the max_bytes budget of %d.%s"),
			PayloadBytes, MaxBytes, *Advice));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	if (!ResolvedObject.IsEmpty())
	{
		Result->SetStringField(TEXT("object"), ResolvedObject);
	}
	Result->SetNumberField(TEXT("full_bytes"), FullBytes);
	Result->SetNumberField(TEXT("returned_bytes"), PayloadBytes);
	if (bGrepped)
	{
		Result->SetStringField(TEXT("grep_pattern"), GrepPattern);
		// `match_count` is LINES, and keeps that meaning on purpose (gap #101) — numbers
		// already quoted in the evidence notes were counted this way. `occurrence_count` is
		// the total the name `match_count` sounds like it means.
		Result->SetNumberField(TEXT("match_count"), Grep.MatchingLines);
		Result->SetNumberField(TEXT("matching_lines"), Grep.MatchingLines);
		Result->SetNumberField(TEXT("occurrence_count"), Grep.Occurrences);

		// ------------------------------------------------------------------
		// GAP #101 — a zero match must SAY SO.
		//
		// It used to return success with an empty `text` and nothing else, so "the pattern is
		// absent from the dump" and "the thing is absent from the asset" were the same answer.
		// On this action that is expensive: it is the only instrument that can see a Niagara
		// ExposureOptions flag, and it is where I-38's cause was found. A caller that treats a
		// silent zero as proof of absence draws exactly the wrong conclusion.
		//
		// The advice is the procedure the validator actually used to trust its own zero match:
		// re-dump without a pattern and read the property ORDERING, because an absent property
		// is one that equals the CDO and is simply not written.
		// ------------------------------------------------------------------
		if (Grep.MatchingLines == 0)
		{
			TArray<TSharedPtr<FJsonValue>> Warnings;
			Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("grep_pattern '%s' matched NO line of the %d-byte dump, so this response proves nothing "
					 "about the asset. The pattern is a plain case-insensitive LITERAL substring — not a regex, "
					 "and nothing is escaped or stripped — so check spelling, spacing and the exact punctuation "
					 "the T3D uses. DO NOT read this as 'the property is absent': re-run without grep_pattern "
					 "(add object_filter to keep it small) and read the property ORDERING instead, since a "
					 "property equal to the class default is never written at all."),
				*GrepPattern, FullBytes)));
			Result->SetArrayField(TEXT("warnings"), Warnings);
		}
	}
	Result->SetStringField(TEXT("text"), PayloadText);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectExportAssetTextAction::GetSchema()
{
	return FParamSchemaBuilder()
		.RequiredAssetPath(TEXT("asset_path"), TEXT("Package path of the asset to export (e.g. /Game/Path/MyAsset)"))
		.Optional(TEXT("object_filter"), TEXT("string"),
			TEXT("Optional name/class substring (case-insensitive) scoping the export to a single matching sub-object instead of the whole asset"))
		.Optional(TEXT("grep_pattern"), TEXT("string"),
			TEXT("Optional case-insensitive LITERAL substring -- not a regex, nothing is escaped or stripped, so '(' and '\"' match themselves. Returns only matching lines plus 3 lines of context each side. 'match_count' counts matching LINES (so does 'matching_lines'); 'occurrence_count' counts total hits. A zero match returns a warnings[] entry: it is NOT evidence the property is absent -- re-dump without a pattern and read the property ordering instead."))
		.Optional(TEXT("max_bytes"), TEXT("number"),
			TEXT("Optional byte budget for the returned text (default 262144). Hard error if the payload exceeds it -- narrow with grep_pattern/object_filter rather than expecting silent truncation"))
		.Build();
}
