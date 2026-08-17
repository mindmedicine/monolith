// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"

/**
 * Deciding WHAT A RESET MUST CLEAR, separated from the graph mutation that does it.
 *
 * WHY THIS FILE EXISTS (gap #42, applied to a WRITE action).
 * A Niagara module input value can live in TWO independent stores:
 *
 *   1. the OVERRIDE PIN on the module's ParameterMapSet node (what Monolith writes), and
 *   2. Script->RapidIterationParameters (what the editor UI writes for every
 *      rapid-iteration-eligible type — i.e. every non-static float, int, vector, colour
 *      and quat; see NA_IsRapidIterationType in MonolithNiagaraActions.cpp).
 *
 * Gap #42 measured that THE PIN WINS AT COMPILE TIME and the RI entry can go stale
 * ("A read is only really exposed if it's the first read and it has no corresponding write" —
 * NiagaraCompilationTasks.cpp:346-347). That asymmetry is exactly what makes a RESET
 * dangerous to write casually: an implementation that removes the override pin and stops
 * leaves the STALE RI ENTRY as the new winner, so the input silently snaps back to a value
 * the caller believed it had just cleared. That is a silent-wrong-value bug of the same class
 * gap #42 already cost this project once, in the other direction.
 *
 * So the rule this header encodes is blunt: A RESET CLEARS EVERY STORE THAT HOLDS SOMETHING,
 * and it reports which ones it found. Neither store is inferred from the other — their
 * presence is measured independently and recorded independently, because the whole point of
 * gap #42 is that the two disagree.
 *
 * WHAT THIS FILE DELIBERATELY DOES NOT DO.
 * It touches no asset, no graph and no editor module: it maps an OBSERVED STORE STATE to a
 * PLAN. The handler measures the state (override pin lookup + RI store lookup) and executes
 * the plan (node teardown + FNiagaraParameterStore::RemoveParameter). Keeping the decision
 * pure is what lets the two-store rule be unit-tested at all — every other part of this action
 * needs a live UNiagaraSystem.
 *
 * Unit-tested in Private/Tests/MonolithNiagaraResetInputTest.cpp.
 */
namespace MonolithNiagaraResetInput
{
	/**
	 * What the two stores actually hold, as MEASURED by the handler. Every field is an
	 * observation, never a deduction from another field.
	 */
	struct FResetInputState
	{
		/** An override pin for this input exists on the module's ParameterMapSet node. */
		bool bOverridePinExists = false;

		/**
		 * ...and something is wired INTO it — a dynamic input function call, a linked-parameter
		 * MapGet, or a UNiagaraNodeInput feeder. An override pin with no links carries a plain
		 * literal in its DefaultValue instead.
		 *
		 * Tracked separately from bOverridePinExists because the two need different teardowns:
		 * an unlinked pin is just removed, a linked one drags a whole node chain with it and
		 * must go through the gap #37 plan-first teardown.
		 */
		bool bOverridePinLinked = false;

		/** An entry for this input's rapid-iteration parameter is present in at least one
		 *  affected script's RapidIterationParameters store. */
		bool bRapidIterationEntryExists = false;
	};

	/** What the reset must do, and what it should therefore report. */
	struct FResetInputPlan
	{
		bool bRemoveOverridePin = false;
		bool bRemoveDynamicInputChain = false;
		bool bClearRapidIteration = false;

		/** True when at least one store held something, i.e. the editor would have drawn the
		 *  revert arrow. False means the row was already at its script default. */
		bool bChanged = false;

		/** The idempotent no-op case: neither store held anything. */
		bool bAlreadyAtDefault = false;
	};

	/**
	 * The two-store rule, in one place.
	 *
	 * Note what is NOT here: there is no `else`, and no early return. Each store is cleared on
	 * its OWN evidence. A reader tempted to write "if the pin existed, we're done" should read
	 * the gap #42 paragraph above — the pin winning at compile time is precisely why clearing
	 * it alone promotes the stale RI entry to being the effective value.
	 */
	inline FResetInputPlan PlanReset(const FResetInputState& State)
	{
		FResetInputPlan Plan;

		Plan.bRemoveOverridePin      = State.bOverridePinExists;
		Plan.bRemoveDynamicInputChain = State.bOverridePinExists && State.bOverridePinLinked;
		Plan.bClearRapidIteration    = State.bRapidIterationEntryExists;

		Plan.bChanged = Plan.bRemoveOverridePin || Plan.bClearRapidIteration;
		Plan.bAlreadyAtDefault = !Plan.bChanged;

		return Plan;
	}

	/**
	 * The stores this plan will clear, as stable machine-readable tokens for the response's
	 * `stores_cleared` array. Order is fixed (pin first, then rapid iteration) so a caller can
	 * compare two responses without sorting.
	 */
	inline TArray<FString> StoresCleared(const FResetInputPlan& Plan)
	{
		TArray<FString> Stores;
		if (Plan.bRemoveOverridePin)   { Stores.Add(TEXT("override_pin")); }
		if (Plan.bClearRapidIteration) { Stores.Add(TEXT("rapid_iteration")); }
		return Stores;
	}
}
