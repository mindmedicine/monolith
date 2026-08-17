// SPDX-License-Identifier: MIT
// Automation tests for reset_module_input_to_default's TWO-STORE rule (gap #42).
//
// SCOPE, STATED HONESTLY. These cover the only part of the action that needs no editor: the
// decision of WHICH STORES A RESET MUST CLEAR, given what the handler measured. They do NOT
// cover the graph mutation, the RI store write, or the resulting compiled value — all three
// need a live UNiagaraSystem and must be validated in-editor against the fixture named in
// Docs/staging/2026-08-18-toolsmith-reset-input.md.
//
// WHY THESE ROWS CAN FAIL — which is the property that matters, and the one that was missing
// from the tests that shipped green over a broken action this morning. Every row below is
// built around a SPECIFIC WRONG IMPLEMENTATION, named in its comment. Two degenerate
// implementations are ruled out by construction:
//   * one that clears nothing ("already at default" always) fails AllStores, RapidIterationOnly,
//     OverridePinOnly and LinkedChain; and
//   * one that clears everything unconditionally fails AlreadyAtDefault and UnlinkedOverride.
// No single constant answer passes this file.

#include "Misc/AutomationTest.h"
#include "MonolithNiagaraResetInput.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace MonolithNiagaraResetInput;

namespace MonolithNiagaraResetInputTestDetail
{
	static FResetInputState State(bool bPin, bool bLinked, bool bRapidIteration)
	{
		FResetInputState S;
		S.bOverridePinExists = bPin;
		S.bOverridePinLinked = bLinked;
		S.bRapidIterationEntryExists = bRapidIteration;
		return S;
	}
}

using namespace MonolithNiagaraResetInputTestDetail;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraResetInputTwoStoreTest,
	"Monolith.Niagara.ResetInput.TwoStores",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraResetInputTwoStoreTest::RunTest(const FString& /*Parameters*/)
{
	// --- ROW 1: BOTH stores hold a value ------------------------------------------------
	// CATCHES: the gap #42 trap named in the brief — an implementation that removes the
	// override pin and returns, leaving the RI entry behind. Because the pin wins at compile
	// time, that stale entry becomes the new effective value and the input silently snaps back
	// to a number the caller believes was just cleared. An `else if` between the two stores
	// fails here; so does any early return after the pin.
	{
		const FResetInputPlan Plan = PlanReset(State(/*pin*/ true, /*linked*/ true, /*ri*/ true));
		TestTrue(TEXT("both stores: override pin is removed"), Plan.bRemoveOverridePin);
		TestTrue(TEXT("both stores: rapid iteration is ALSO cleared (gap #42 — clearing only the "
			"pin promotes the stale RI entry to being the effective value)"), Plan.bClearRapidIteration);
		TestTrue(TEXT("both stores: reported as changed"), Plan.bChanged);
		TestFalse(TEXT("both stores: not already at default"), Plan.bAlreadyAtDefault);

		const TArray<FString> Stores = StoresCleared(Plan);
		TestEqual(TEXT("both stores: two stores reported"), Stores.Num(), 2);
		TestEqual(TEXT("both stores: pin reported first"), Stores[0], FString(TEXT("override_pin")));
		TestEqual(TEXT("both stores: rapid iteration reported second"), Stores[1], FString(TEXT("rapid_iteration")));
	}

	// --- ROW 2: RI ONLY, no override pin ------------------------------------------------
	// This is the ORDINARY shape for a human-authored float/vector/colour: the editor's
	// SetLocalValue removes the override pin and writes RapidIterationParameters instead
	// (NiagaraStackFunctionInput.cpp:2309-2322), so an input a human edited in the stack UI has
	// NO override pin at all.
	// CATCHES: the write-side twin of the gap #42 read bug — an implementation that treats
	// "no override pin" as "nothing is overridden", reports already-at-default, and changes
	// nothing. That would make the action a silent no-op on exactly the inputs a human is most
	// likely to want reset, which is the recovery case this action exists for.
	{
		const FResetInputPlan Plan = PlanReset(State(/*pin*/ false, /*linked*/ false, /*ri*/ true));
		TestFalse(TEXT("RI only: no override pin to remove"), Plan.bRemoveOverridePin);
		TestTrue(TEXT("RI only: rapid iteration is cleared"), Plan.bClearRapidIteration);
		TestTrue(TEXT("RI only: reported as changed — 'no override pin' does NOT mean unset"), Plan.bChanged);
		TestFalse(TEXT("RI only: not already at default"), Plan.bAlreadyAtDefault);
		TestEqual(TEXT("RI only: exactly one store cleared"), StoresCleared(Plan).Num(), 1);
		TestEqual(TEXT("RI only: that store is rapid_iteration"), StoresCleared(Plan)[0], FString(TEXT("rapid_iteration")));
	}

	// --- ROW 3: override pin ONLY, no RI entry ------------------------------------------
	// The shape Monolith's own set_module_input_value produces (it writes the pin, never RI),
	// and the shape of any non-RI type (bool, enum, DI).
	// CATCHES: an implementation that reports `rapid_iteration` as cleared unconditionally —
	// i.e. claims to have cleared a store that held nothing. That is a false evidence claim in
	// a response whose entire job is to say which stores were touched, and it would make the
	// two-store guarantee unfalsifiable from the outside.
	{
		const FResetInputPlan Plan = PlanReset(State(/*pin*/ true, /*linked*/ false, /*ri*/ false));
		TestTrue(TEXT("pin only: override pin is removed"), Plan.bRemoveOverridePin);
		TestFalse(TEXT("pin only: rapid iteration NOT reported cleared — there was no entry"), Plan.bClearRapidIteration);
		TestTrue(TEXT("pin only: reported as changed"), Plan.bChanged);
		TestEqual(TEXT("pin only: exactly one store cleared"), StoresCleared(Plan).Num(), 1);
		TestEqual(TEXT("pin only: that store is override_pin"), StoresCleared(Plan)[0], FString(TEXT("override_pin")));
	}

	// --- ROW 4: NEITHER store holds anything --------------------------------------------
	// The already-at-default case. The action answers this as a NO-OP SUCCESS with a warning
	// (justified in the staging note: reset is a recovery tool and idempotence is a virtue
	// there), but whichever way that call goes, the PLAN must say nothing needs clearing.
	// CATCHES: an implementation that reports changed:true having done nothing — the response
	// would assert a mutation that never happened, and a caller diffing on `changed` would
	// believe a stuck input had been recovered when it had not. Also catches any attempt to
	// remove an override pin that does not exist (a null deref in the handler).
	{
		const FResetInputPlan Plan = PlanReset(State(/*pin*/ false, /*linked*/ false, /*ri*/ false));
		TestFalse(TEXT("already default: nothing to remove from the graph"), Plan.bRemoveOverridePin);
		TestFalse(TEXT("already default: no chain to remove"), Plan.bRemoveDynamicInputChain);
		TestFalse(TEXT("already default: no rapid iteration entry to clear"), Plan.bClearRapidIteration);
		TestFalse(TEXT("already default: NOT reported as changed"), Plan.bChanged);
		TestTrue(TEXT("already default: flagged as the idempotent no-op case"), Plan.bAlreadyAtDefault);
		TestEqual(TEXT("already default: no stores reported cleared"), StoresCleared(Plan).Num(), 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraResetInputChainTest,
	"Monolith.Niagara.ResetInput.DynamicInputChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraResetInputChainTest::RunTest(const FString& /*Parameters*/)
{
	// --- ROW 5: override pin exists but is UNLINKED --------------------------------------
	// A plain literal parked in the pin's DefaultValue. Removing the pin is the whole job;
	// there are no feeder nodes.
	// CATCHES: an implementation that reports a dynamic-input chain removal (and a node count)
	// for a pin that never had one. Tim's observed semantics distinguish these two cases
	// explicitly — the dynamic input node "and ALL its child inputs are gone" is what the
	// caller is told happened, so claiming it for a bare literal is a fabricated detail in the
	// one field a caller would use to decide whether to re-inspect the graph.
	{
		const FResetInputPlan Plan = PlanReset(State(/*pin*/ true, /*linked*/ false, /*ri*/ false));
		TestTrue(TEXT("unlinked override: pin still removed"), Plan.bRemoveOverridePin);
		TestFalse(TEXT("unlinked override: NO dynamic-input chain claimed"), Plan.bRemoveDynamicInputChain);
	}

	// --- ROW 6: override pin LINKED to a feeder chain -------------------------------------
	// Tim's measured before/after: Velocity Speed bound to a Random Range Float dynamic input
	// with its own Minimum/Maximum children. One click removes the node AND its children.
	// CATCHES: an implementation that removes only the override pin and leaves the dynamic
	// input node chain orphaned in the graph. Those orphans are exactly the "dead_dynamic_input"
	// leaks audit_stack_wiring reports and that remove_dynamic_input REFUSES to clean up
	// (gap #37, unrooted teardown disabled) — so a reset that leaks them creates graph garbage
	// no action in the namespace can subsequently remove.
	{
		const FResetInputPlan Plan = PlanReset(State(/*pin*/ true, /*linked*/ true, /*ri*/ false));
		TestTrue(TEXT("linked override: pin removed"), Plan.bRemoveOverridePin);
		TestTrue(TEXT("linked override: the feeder chain is removed with it, not orphaned"),
			Plan.bRemoveDynamicInputChain);
		TestTrue(TEXT("linked override: reported as changed"), Plan.bChanged);
	}

	// --- ROW 7: a link with no pin is INCOHERENT and must not invent work -----------------
	// bOverridePinLinked is only meaningful when a pin exists. If the handler ever measured
	// this pair (it should not), the plan must not schedule a chain teardown rooted at a pin
	// that is not there.
	// CATCHES: reading bOverridePinLinked without the bOverridePinExists conjunction — which
	// in the handler would mean entering the gap #37 teardown with a null entry pin, the exact
	// UNROOTED shape that path refuses precisely because it destroys live nodes.
	{
		const FResetInputPlan Plan = PlanReset(State(/*pin*/ false, /*linked*/ true, /*ri*/ false));
		TestFalse(TEXT("linked-without-pin: no chain teardown is scheduled"), Plan.bRemoveDynamicInputChain);
		TestFalse(TEXT("linked-without-pin: no pin removal is scheduled"), Plan.bRemoveOverridePin);
		TestTrue(TEXT("linked-without-pin: treated as already at default"), Plan.bAlreadyAtDefault);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
