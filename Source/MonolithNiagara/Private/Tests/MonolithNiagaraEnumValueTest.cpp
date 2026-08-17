// SPDX-License-Identifier: MIT
// Automation tests for gap #46 — the strict enum-VALUE resolver behind set_renderer_property
// (and therefore behind configure_ribbon, configure_subuv and import_system_spec, which all
// delegate to it).
//
// SCOPE, STATED HONESTLY. These cover the DECISION half: given a UEnum and a caller token, is the
// token a legal value and what is it worth. They do NOT cover the write half — that a refusal
// leaves the renderer's stored property byte-identical, and that a half-applied preset can no
// longer be produced. That needs a live system, a renderer and an editor, so it must be validated
// in-editor against the fixture recorded in the staging note:
//   * configure_ribbon preset=tube, then shape="ZZZ_NotAShape" -> must ERROR, and T3D must still
//     show Shape=Tube alongside TubeSubdivisions=8.
//
// Every table below is built around the OLD, WRONG answers, so the rows are negative controls:
// "ZZZ_NotAnEnum" is the token that was measured storing Alignment=Unaligned (entry 0), and the
// digitless "-" / "." / "+" rows exist because FCString::IsNumeric accepts them and Atoi64 answers
// 0 for each — the same "quietly becomes entry 0" failure by a second route.
//
// Each enum is exercised with BOTH an accepting and a refusing row. That pairing is deliberate:
// a resolver that refused everything would fail the accept rows, and one that accepted everything
// (the shipped bug) would fail the refuse rows. Neither degenerate implementation can pass.

#include "Misc/AutomationTest.h"
#include "MonolithNiagaraEnumValue.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraRibbonRendererProperties.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace MonolithNiagaraEnumValue;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraEnumValueStringTest,
	"Monolith.Niagara.EnumValue.String",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraEnumValueStringTest::RunTest(const FString& /*Parameters*/)
{
	const UEnum* Alignment = StaticEnum<ENiagaraSpriteAlignment>();
	const UEnum* Shape     = StaticEnum<ENiagaraRibbonShapeMode>();

	if (!TestNotNull(TEXT("ENiagaraSpriteAlignment is reflected"), Alignment)) { return false; }
	if (!TestNotNull(TEXT("ENiagaraRibbonShapeMode is reflected"), Shape))     { return false; }

	struct FCase
	{
		const UEnum* Enum;
		const TCHAR* Raw;
		bool         bExpectOk;
		int64        ExpectedValue;   // only meaningful when bExpectOk
		const TCHAR* Why;
	};

	const FCase Cases[] =
	{
		// --- ACCEPTED: the spellings a caller may reasonably use --------------------------------
		{ Alignment, TEXT("Unaligned"),        true,  (int64)ENiagaraSpriteAlignment::Unaligned,       TEXT("short name, entry 0 — must still be reachable ON PURPOSE") },
		{ Alignment, TEXT("VelocityAligned"),  true,  (int64)ENiagaraSpriteAlignment::VelocityAligned, TEXT("short name, the positive control from the measurement") },
		{ Alignment, TEXT("Automatic"),        true,  (int64)ENiagaraSpriteAlignment::Automatic,       TEXT("last real entry, sits just before _MAX") },
		{ Alignment, TEXT("ENiagaraSpriteAlignment::CustomAlignment"), true, (int64)ENiagaraSpriteAlignment::CustomAlignment, TEXT("fully-qualified form") },
		{ Alignment, TEXT("1"),                true,  1,                                              TEXT("a bare integer the enum DOES declare") },
		{ Alignment, TEXT("0"),                true,  0,                                              TEXT("explicit 0 is legal; only IMPLICIT 0 was the bug") },
		// Case-insensitivity is INHERITED, not introduced: EGetByNameFlags::None resolves with
		// ENameCase::IgnoreCase (Enum.cpp, UEnum::GetIndexByNameString ~:953). Asserted so that
		// tightening it later is a deliberate, visible decision rather than a silent regression.
		{ Alignment, TEXT("velocityaligned"),  true,  (int64)ENiagaraSpriteAlignment::VelocityAligned, TEXT("lookup is case-insensitive, unchanged by this fix") },
		{ Shape,     TEXT("Tube"),             true,  (int64)ENiagaraRibbonShapeMode::Tube,           TEXT("the value preset=tube applies") },

		// --- REFUSED: every one of these used to be stored as entry 0 ----------------------------
		{ Alignment, TEXT("ZZZ_NotAnEnum_RB"), false, 0, TEXT("the exact token measured storing Alignment=Unaligned with no warning") },
		{ Alignment, TEXT(""),                 false, 0, TEXT("empty token names nothing") },
		{ Alignment, TEXT("-"),                false, 0, TEXT("IsNumeric accepts it and Atoi64 answers 0 — the digitless trap") },
		{ Alignment, TEXT("."),                false, 0, TEXT("same trap, second spelling") },
		{ Alignment, TEXT("+"),                false, 0, TEXT("same trap, third spelling") },
		{ Alignment, TEXT("99"),               false, 0, TEXT("an integer the enum does not declare must not invent an entry") },
		{ Shape,     TEXT("ZZZ_NotAShape_RB"), false, 0, TEXT("the token that reverted a tube preset to Plane while TubeSubdivisions=8 survived") },
		{ Shape,     TEXT("VelocityAligned"),  false, 0, TEXT("a name valid on a DIFFERENT enum must not resolve here") },
	};

	for (const FCase& Case : Cases)
	{
		// Seeded with a value no enum here declares, so "untouched on refusal" is checkable.
		int64 Value = -777;
		FString Error;
		const bool bOk = ResolveEnumValueStrict(Case.Enum, FString(Case.Raw), Value, Error);

		TestEqual(
			FString::Printf(TEXT("ResolveEnumValueStrict('%s') on %s (%s)"),
				Case.Raw, *Case.Enum->GetName(), Case.Why),
			bOk, Case.bExpectOk);

		if (Case.bExpectOk)
		{
			TestEqual(FString::Printf(TEXT("'%s' resolves to the declared value"), Case.Raw), Value, Case.ExpectedValue);
			TestTrue(FString::Printf(TEXT("'%s' succeeds with no error text"), Case.Raw), Error.IsEmpty());
		}
		else
		{
			// The heart of the fix: a refusal must not write anything, anywhere.
			TestEqual(FString::Printf(TEXT("'%s' leaves OutValue untouched"), Case.Raw), Value, (int64)-777);
			TestFalse(FString::Printf(TEXT("'%s' explains itself"), Case.Raw), Error.IsEmpty());
			TestTrue(
				FString::Printf(TEXT("'%s' error names the enum"), Case.Raw),
				Error.Contains(Case.Enum->GetName()));
			// A refusal that does not say what IS valid just moves the guesswork to the caller.
			TestTrue(
				FString::Printf(TEXT("'%s' error lists a real alternative"), Case.Raw),
				Error.Contains(Case.Enum->GetNameStringByIndex(0)));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraEnumValueNumericTest,
	"Monolith.Niagara.EnumValue.Numeric",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraEnumValueNumericTest::RunTest(const FString& /*Parameters*/)
{
	const UEnum* Alignment = StaticEnum<ENiagaraSpriteAlignment>();
	if (!TestNotNull(TEXT("ENiagaraSpriteAlignment is reflected"), Alignment)) { return false; }

	struct FCase { double Raw; bool bExpectOk; int64 Expected; const TCHAR* Why; };

	const FCase Cases[] =
	{
		{ 0.0,   true,  0, TEXT("JSON number 0 is a legal, explicit entry") },
		{ 2.0,   true,  2, TEXT("JSON numbers arrive as doubles even when integral") },
		{ 99.0,  false, 0, TEXT("out of range must refuse, not clamp") },
		{ -1.0,  false, 0, TEXT("negative out of range must refuse") },
		{ 1.5,   false, 0, TEXT("non-integral must refuse, not truncate to 1") },
	};

	for (const FCase& Case : Cases)
	{
		int64 Value = -777;
		FString Error;
		const bool bOk = ResolveEnumValueStrictNumeric(Alignment, Case.Raw, Value, Error);

		TestEqual(FString::Printf(TEXT("ResolveEnumValueStrictNumeric(%f) (%s)"), Case.Raw, Case.Why), bOk, Case.bExpectOk);

		if (Case.bExpectOk)
		{
			TestEqual(FString::Printf(TEXT("%f resolves to the declared value"), Case.Raw), Value, Case.Expected);
		}
		else
		{
			TestEqual(FString::Printf(TEXT("%f leaves OutValue untouched"), Case.Raw), Value, (int64)-777);
			TestFalse(FString::Printf(TEXT("%f explains itself"), Case.Raw), Error.IsEmpty());
		}
	}

	// A null enum is a programming error upstream, not a licence to write something.
	{
		int64 Value = -777;
		FString Error;
		TestFalse(TEXT("a null UEnum refuses"), ResolveEnumValueStrictNumeric(nullptr, 0.0, Value, Error));
		TestEqual(TEXT("a null UEnum writes nothing"), Value, (int64)-777);
		TestFalse(TEXT("a null UEnum explains itself"), Error.IsEmpty());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraEnumValueDescribeTest,
	"Monolith.Niagara.EnumValue.Describe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraEnumValueDescribeTest::RunTest(const FString& /*Parameters*/)
{
	const UEnum* Alignment = StaticEnum<ENiagaraSpriteAlignment>();
	if (!TestNotNull(TEXT("ENiagaraSpriteAlignment is reflected"), Alignment)) { return false; }

	const FString Described = DescribeValidValues(Alignment);

	// Positive: every real entry must be offered, or the refusal message misleads.
	TestTrue(TEXT("offers Unaligned"),       Described.Contains(TEXT("Unaligned")));
	TestTrue(TEXT("offers VelocityAligned"), Described.Contains(TEXT("VelocityAligned")));
	TestTrue(TEXT("offers CustomAlignment"), Described.Contains(TEXT("CustomAlignment")));
	TestTrue(TEXT("offers Automatic"),       Described.Contains(TEXT("Automatic")));

	// Negative: UHT's autogenerated sentinel is storable but is not an answer to
	// "what should I have said". If this row ever goes red the message has started
	// recommending a value no caller means.
	TestFalse(TEXT("does not offer the _MAX sentinel"), Described.Contains(TEXT("_MAX")));

	TestTrue(TEXT("a null enum describes nothing rather than crashing"), DescribeValidValues(nullptr).IsEmpty());

	// Sentinel classification, exercised directly so the rule above cannot pass by accident.
	TestTrue (TEXT("_MAX is a sentinel"),          IsSentinelEnumName(TEXT("ENiagaraSpriteAlignment_MAX")));
	TestTrue (TEXT("_NUM is a sentinel"),          IsSentinelEnumName(TEXT("Something_NUM")));
	TestFalse(TEXT("Automatic is not a sentinel"), IsSentinelEnumName(TEXT("Automatic")));
	TestFalse(TEXT("_MAXValue is not a sentinel"), IsSentinelEnumName(TEXT("_MAXValue")));

	// ENiagaraSpriteAlignment is an ordinary enum, not a flags enum — so combinations
	// must NOT be waved through. This is the guard that keeps IsStorableEnumValue honest.
	TestFalse(TEXT("ENiagaraSpriteAlignment is not a Bitflags enum"), IsBitflagsEnum(Alignment));
	TestTrue (TEXT("a declared value is storable"),      IsStorableEnumValue(Alignment, 1));
	TestFalse(TEXT("an undeclared value is not storable"), IsStorableEnumValue(Alignment, 99));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
