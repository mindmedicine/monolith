// SPDX-License-Identifier: MIT
// Automation tests for get_script_details' decode of
// FNiagaraModuleDependency::OnlyEvaluateInScriptUsage.
//
// SCOPE. This is the one part of the script-details reader that is pure logic and therefore
// testable without an asset: turning the bitmask into phase names. Reading the script asset,
// selecting the version, and the field-by-field projection all need a loaded UNiagaraScript and
// must be validated in-editor (see Docs/staging/2026-08-18-toolsmith-script-details.md).
//
// WHY THIS IS THE PART WORTH PINNING. ENiagaraModuleDependencyUsage opens with
// `None UMETA(Hidden)` at index 0, so the first REAL phase is bit 1. A decoder written on the
// natural assumption "bit 0 is the first entry" is off by one and STILL RETURNS A PLAUSIBLE LIST —
// stock GravityForce's Spawn|Update|Event would read as None|Spawn|Update. That is a
// silent-wrong-answer about the precise thing the reader exists to explain (why forces must sit
// above the solver, and why the offered fix differs per stage), so it gets a dedicated test.

#include "Misc/AutomationTest.h"
#include "MonolithNiagaraDependencyUsage.h"
#include "NiagaraScript.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace MonolithNiagaraDependencyUsage;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraDependencyUsageDecodeTest,
	"Monolith.Niagara.ScriptDetails.DependencyUsageDecode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraDependencyUsageDecodeTest::RunTest(const FString& /*Parameters*/)
{
	const int32 SpawnBit   = 1 << static_cast<int32>(ENiagaraModuleDependencyUsage::Spawn);
	const int32 UpdateBit  = 1 << static_cast<int32>(ENiagaraModuleDependencyUsage::Update);
	const int32 EventBit   = 1 << static_cast<int32>(ENiagaraModuleDependencyUsage::Event);
	const int32 SimStgBit  = 1 << static_cast<int32>(ENiagaraModuleDependencyUsage::SimulationStage);

	// --- ROW 1: bit 0 is NOT a usage -----------------------------------------------------
	// CATCHES the off-by-one directly. `None` is enum value 0 and `UMETA(Hidden)`; the engine
	// never sets bit 0. A decoder that indexes VISIBLE ENTRIES instead of enum VALUES would
	// report Spawn here.
	{
		int32 Unexpected = 0;
		const TArray<FString> Usages = DecodeUsageBitmask(1 /* bit 0 only */, &Unexpected);
		TestEqual(TEXT("bit 0 names no phase — it is the hidden 'None' sentinel"), Usages.Num(), 0);
		TestEqual(TEXT("bit 0 is surfaced as an unexpected bit, not silently dropped"), Unexpected, 1);
	}

	// --- ROW 2: stock GravityForce's actual declaration -----------------------------------
	// Tim's measured screenshot: "Only Evaluate in Script Usage: Spawn | Update | Event".
	// CATCHES both the shift and any accidental inclusion of SimulationStage. Under an
	// off-by-one decoder this same input yields None|Spawn|Update — three entries, plausible,
	// and wrong about every one of them.
	{
		int32 Unexpected = 0;
		const TArray<FString> Usages = DecodeUsageBitmask(SpawnBit | UpdateBit | EventBit, &Unexpected);
		TestEqual(TEXT("Spawn|Update|Event decodes to exactly three phases"), Usages.Num(), 3);
		TestTrue(TEXT("Spawn present"),  Usages.Contains(TEXT("Spawn")));
		TestTrue(TEXT("Update present"), Usages.Contains(TEXT("Update")));
		TestTrue(TEXT("Event present"),  Usages.Contains(TEXT("Event")));
		TestFalse(TEXT("SimulationStage NOT present — it was not set"), Usages.Contains(TEXT("SimulationStage")));
		TestEqual(TEXT("no unexpected bits in a well-formed mask"), Unexpected, 0);
	}

	// --- ROW 3: the engine's own default constructor value --------------------------------
	// FNiagaraModuleDependency() sets Spawn|Update|Event|SimulationStage (NiagaraScript.h:159-162).
	// CATCHES a decoder that drops the LAST enum entry — the classic `NumEnums()` fencepost, where
	// forgetting that NumEnums() counts the implicit _MAX makes SimulationStage vanish. That bug
	// would under-report a dependency's reach and is invisible on the three-phase common case.
	{
		int32 Unexpected = 0;
		const TArray<FString> Usages = DecodeUsageBitmask(AllUsagesMask(), &Unexpected);
		TestEqual(TEXT("the engine default mask decodes to all four phases"), Usages.Num(), 4);
		TestTrue(TEXT("SimulationStage survives the NumEnums fencepost"), Usages.Contains(TEXT("SimulationStage")));
		TestEqual(TEXT("the engine's own default contains no unnameable bits"), Unexpected, 0);
	}

	// --- ROW 4: AllUsagesMask agrees with the engine's literal ----------------------------
	// An INDEPENDENT cross-check on the bit positions themselves: 0b11110 == 30, bit 0 clear.
	// CATCHES a silent re-ordering of ENiagaraModuleDependencyUsage in a future engine version —
	// the decoder would still "work" and every reported phase would be wrong. Asserting the
	// literal is deliberate: it is the one thing that cannot re-derive itself from the enum.
	{
		TestEqual(TEXT("Spawn|Update|Event|SimulationStage is 0b11110 (30)"), AllUsagesMask(), 30);
		TestEqual(TEXT("bit 0 is clear in the engine default"), AllUsagesMask() & 1, 0);
		TestEqual(TEXT("Spawn is bit 1"), SpawnBit, 2);
		TestEqual(TEXT("SimulationStage is bit 4"), SimStgBit, 16);
	}

	// --- ROW 5: empty mask -----------------------------------------------------------------
	// A dependency restricted to nothing. CATCHES a decoder that falls back to "all phases" when
	// the mask is 0 — which would turn "enforced nowhere" into "enforced everywhere", the most
	// consequential possible inversion for an ordering rule.
	{
		int32 Unexpected = 0;
		const TArray<FString> Usages = DecodeUsageBitmask(0, &Unexpected);
		TestEqual(TEXT("an empty mask decodes to NO phases, not to all of them"), Usages.Num(), 0);
		TestEqual(TEXT("an empty mask has no unexpected bits"), Unexpected, 0);
	}

	// --- ROW 6: a bit no enum entry claims --------------------------------------------------
	// CATCHES silent truncation of data we cannot name. A high bit set by a future engine (or by
	// corruption) must be REPORTED, not dropped — the reader's job is to say what the asset
	// declares, including the parts it does not understand.
	{
		int32 Unexpected = 0;
		const TArray<FString> Usages = DecodeUsageBitmask(SpawnBit | (1 << 20), &Unexpected);
		TestEqual(TEXT("known bits still decode alongside an unknown one"), Usages.Num(), 1);
		TestTrue(TEXT("Spawn still reported"), Usages.Contains(TEXT("Spawn")));
		TestEqual(TEXT("the unknown bit is reported rather than discarded"), Unexpected, 1 << 20);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
