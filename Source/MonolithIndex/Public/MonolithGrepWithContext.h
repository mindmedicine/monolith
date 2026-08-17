// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"

/**
 * The line filter behind export_asset_text's `grep_pattern`.
 *
 * WHAT IT IS, STATED because the gap log guessed wrong about it (gap #101). The original claim
 * was that a pattern containing '(' or '"' matches nothing — that it "behaves as a regex or
 * strips punctuation". READ IN SOURCE 2026-08-17: it does neither. It is, and always was, a
 * plain case-insensitive LITERAL substring test (`Lines[i].ToLower().Contains(PatternLower)`).
 * No escaping, no regex, no punctuation handling. That half of #101 is REFUTED and no code
 * changed for it.
 *
 * WHAT WAS ACTUALLY WRONG — two things, both about what the caller is told:
 *
 *  1. A ZERO MATCH WAS SILENT. It returned success with `match_count: 0` and an empty `text`,
 *     which is indistinguishable from "the property is absent". That matters more here than
 *     almost anywhere else: export_asset_text is the ONLY instrument that can see a Niagara
 *     ExposureOptions flag, and it is the instrument that produced the bExposed evidence behind
 *     I-38. "Never conclude absence from a zero match" was a rule the caller had to remember;
 *     it is now a warning the action emits.
 *
 *  2. `match_count` COUNTED LINES, NOT OCCURRENCES, under a name that says otherwise — so a
 *     pattern hitting three times on one line reported 1. DECISION, recorded so it is not
 *     re-litigated: `match_count` KEEPS its line meaning and is documented as lines, and the
 *     occurrence total is added ALONGSIDE as `occurrence_count`. Silently redefining
 *     `match_count` would change the meaning of numbers already quoted in the evidence notes
 *     (`NiagaraNodeInput` -> 6), which is the same "success-shaped answer that means something
 *     else" defect this fix exists to remove.
 *
 * Header-only and dependency-free so it can be unit-tested without an asset, an exporter or an
 * editor: Private/Tests/MonolithGrepWithContextTest.cpp.
 */
namespace MonolithGrepWithContext
{
	/** Lines of context emitted on each side of a matching line. */
	constexpr int32 DefaultContextLines = 3;

	/** Counts and the filtered text, so the caller never has to infer one from the other. */
	struct FGrepResult
	{
		/** Lines that contain at least one occurrence. This is what `match_count` reports. */
		int32 MatchingLines = 0;

		/** Total non-overlapping occurrences across all lines. Always >= MatchingLines. */
		int32 Occurrences = 0;

		/** Matching lines plus context, disjoint windows separated by a "..." marker. */
		FString Text;
	};

	/**
	 * Count non-overlapping occurrences of Needle in Haystack. Both are expected pre-lowered by
	 * the caller — the search is case-insensitive by lowering once per line rather than per
	 * occurrence. An empty Needle yields 0 rather than looping forever.
	 */
	inline int32 CountOccurrences(const FString& Haystack, const FString& Needle)
	{
		if (Needle.IsEmpty())
		{
			return 0;
		}

		int32 Count = 0;
		int32 From = 0;
		while (From <= Haystack.Len() - Needle.Len())
		{
			const int32 Found = Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
			if (Found == INDEX_NONE)
			{
				break;
			}
			++Count;
			// Non-overlapping: "aa" occurs ONCE in "aaa", not twice. Stated because the
			// alternative convention is defensible and this one is what the test asserts.
			From = Found + Needle.Len();
		}
		return Count;
	}

	/**
	 * Filter Text to the lines matching Pattern (case-insensitive literal substring), each
	 * surrounded by ContextLines of context, and report both counts.
	 */
	inline FGrepResult GrepWithContext(const FString& Text, const FString& Pattern,
		int32 ContextLines = DefaultContextLines)
	{
		FGrepResult Result;

		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

		const FString PatternLower = Pattern.ToLower();
		TArray<bool> Keep;
		Keep.Init(false, Lines.Num());

		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			const FString LineLower = Lines[i].ToLower();
			const int32 OnThisLine = CountOccurrences(LineLower, PatternLower);
			if (OnThisLine > 0)
			{
				++Result.MatchingLines;
				Result.Occurrences += OnThisLine;

				const int32 Start = FMath::Max(0, i - ContextLines);
				const int32 End = FMath::Min(Lines.Num() - 1, i + ContextLines);
				for (int32 k = Start; k <= End; ++k)
				{
					Keep[k] = true;
				}
			}
		}

		bool bPrevKept = false;
		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			if (Keep[i])
			{
				if (!bPrevKept && !Result.Text.IsEmpty())
				{
					Result.Text += TEXT("...\n");
				}
				Result.Text += Lines[i];
				Result.Text += TEXT("\n");
				bPrevKept = true;
			}
			else
			{
				bPrevKept = false;
			}
		}

		return Result;
	}
}
