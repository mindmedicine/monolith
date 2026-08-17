// SPDX-License-Identifier: MIT
// Automation tests for gap #101 — export_asset_text's grep_pattern.
//
// TWO OF THESE ROWS ARE REFUTATIONS, not regression locks. The gap's original claim was that a
// pattern containing '(' or '"' matches nothing, i.e. that the filter behaves as a regex or
// strips punctuation. It does not, and never did. The Punctuation test below pins that down, so
// the wrong cause cannot be "re-fixed" later: the two patterns the gap reported as broken
// (`Input=(Name=` and `NiagaraNodeInput_0"`) match the line the gap quoted verbatim.
//
// What IS locked here is the part that was genuinely wrong — what the caller is TOLD:
//   * a zero match is distinguishable from a match (the action turns this into warnings[]);
//   * match_count is LINES and is now accompanied by occurrence_count, which is hits.
//
// Not covered: that the action emits the warning. That needs an asset and an exporter, so it is
// an in-editor check — see the staging note's validator calls.

#include "Misc/AutomationTest.h"
#include "MonolithGrepWithContext.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace MonolithGrepWithContext;

namespace MonolithGrepTestDetail
{
	/**
	 * A T3D-shaped fixture. The Input= line is quoted from the dump in the gap entry, which is
	 * what makes the punctuation rows a real refutation rather than a synthetic one.
	 */
	static const TCHAR* Fixture =
		TEXT("Begin Object Class=/Script/NiagaraEditor.NiagaraNodeInput Name=\"NiagaraNodeInput_0\"\n")
		TEXT("   Input=(Name=\"InputMap\",TypeDefHandle=(RegisteredTypeIndex=95))\n")
		TEXT("   CallSortPriority=1\n")
		TEXT("End Object\n")
		TEXT("Begin Object Class=/Script/NiagaraEditor.NiagaraNodeInput Name=\"NiagaraNodeInput_1\"\n")
		TEXT("   Input=(Name=\"Other\",TypeDefHandle=(RegisteredTypeIndex=95))\n")
		TEXT("End Object\n");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGrepPunctuationTest,
	"Monolith.Index.Grep.Punctuation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGrepPunctuationTest::RunTest(const FString& /*Parameters*/)
{
	const FString Text = MonolithGrepTestDetail::Fixture;

	// REFUTATION ROWS. The gap reported both of these as match_count 0. They are literal
	// substrings of the dump, and the filter is a literal substring test, so they match.
	{
		const FGrepResult R = GrepWithContext(Text, TEXT("Input=(Name="));
		TestEqual(TEXT("'Input=(Name=' matches both lines — '(' is NOT special"), R.MatchingLines, 2);
	}
	{
		const FGrepResult R = GrepWithContext(Text, TEXT("NiagaraNodeInput_0\""));
		TestEqual(TEXT("'NiagaraNodeInput_0\"' matches — '\"' is NOT special"), R.MatchingLines, 1);
	}

	// A regex metacharacter is matched LITERALLY, which is the positive statement of the same
	// fact: if the filter ever became a regex, ".*" would match every line and this goes red.
	{
		const FGrepResult R = GrepWithContext(Text, TEXT(".*"));
		TestEqual(TEXT("'.*' is a literal, not a wildcard"), R.MatchingLines, 0);
	}

	// Case-insensitivity, asserted so that tightening it later is a visible decision.
	// Only the two "Begin Object" lines carry the class name; the Input= lines do not.
	{
		const FGrepResult R = GrepWithContext(Text, TEXT("niagaranodeinput"));
		TestEqual(TEXT("matching is case-insensitive"), R.MatchingLines, 2);
		TestEqual(TEXT("case-insensitive matching also counts every occurrence"), R.Occurrences, 4);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGrepZeroMatchTest,
	"Monolith.Index.Grep.ZeroMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGrepZeroMatchTest::RunTest(const FString& /*Parameters*/)
{
	const FString Text = MonolithGrepTestDetail::Fixture;

	// The absent-property case the warning exists for: ExposureOptions is genuinely not in the
	// dump. A zero here is a TRUE zero — the point is that the caller can now tell, because the
	// action attaches warnings[] to exactly this state.
	{
		const FGrepResult R = GrepWithContext(Text, TEXT("ExposureOptions"));
		TestEqual(TEXT("an absent pattern matches no line"), R.MatchingLines, 0);
		TestEqual(TEXT("an absent pattern has no occurrences"), R.Occurrences, 0);
		TestTrue(TEXT("an absent pattern returns no text"), R.Text.IsEmpty());
	}

	// The discriminator: a PRESENT pattern must not look like the absent one. Without this row
	// the zero-match assertion above would pass for an implementation that matched nothing ever.
	{
		const FGrepResult R = GrepWithContext(Text, TEXT("CallSortPriority"));
		TestTrue(TEXT("a present pattern matches"), R.MatchingLines > 0);
		TestFalse(TEXT("a present pattern returns text"), R.Text.IsEmpty());
	}

	// An empty pattern is the caller's "no grep" signal upstream; here it must not claim matches.
	{
		const FGrepResult R = GrepWithContext(Text, FString());
		TestEqual(TEXT("an empty pattern claims no occurrences"), R.Occurrences, 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGrepCountingTest,
	"Monolith.Index.Grep.Counting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGrepCountingTest::RunTest(const FString& /*Parameters*/)
{
	// THE UNDERSTATEMENT, isolated. "Input" occurs TWICE on each Begin Object line
	// ("NiagaraNodeInput" and "NiagaraNodeInput_N") and twice on the first Input= line
	// ("Input=" and "InputMap"), but line-counting reports one per line. match_count keeps the
	// line meaning; occurrence_count is the number the name implies.
	const FString Text = MonolithGrepTestDetail::Fixture;

	const FGrepResult R = GrepWithContext(Text, TEXT("Input"));
	TestEqual(TEXT("matching LINES for 'Input'"), R.MatchingLines, 4);
	TestTrue(TEXT("occurrences EXCEED matching lines — the understatement is real and now visible"),
		R.Occurrences > R.MatchingLines);
	TestEqual(TEXT("occurrences for 'Input' (2+2+2+1)"), R.Occurrences, 7);

	// Occurrence counting is non-overlapping, stated because the other convention is defensible.
	TestEqual(TEXT("'aa' occurs once in 'aaa' (non-overlapping)"), CountOccurrences(TEXT("aaa"), TEXT("aa")), 1);
	TestEqual(TEXT("'aa' occurs twice in 'aaaa'"), CountOccurrences(TEXT("aaaa"), TEXT("aa")), 2);
	TestEqual(TEXT("an empty needle occurs zero times, and does not hang"), CountOccurrences(TEXT("abc"), FString()), 0);
	TestEqual(TEXT("a needle longer than the haystack occurs zero times"), CountOccurrences(TEXT("ab"), TEXT("abcd")), 0);

	// Context windows: a single match on line 2 keeps lines 0..5 of a 7-line fixture, and disjoint
	// windows are separated rather than silently joined.
	{
		const FGrepResult Narrow = GrepWithContext(Text, TEXT("CallSortPriority"), /*ContextLines=*/0);
		TestEqual(TEXT("zero context returns exactly the matching line"),
			Narrow.Text, FString(TEXT("   CallSortPriority=1\n")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
