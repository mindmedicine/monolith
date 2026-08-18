// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"
#include "NiagaraCommon.h"
#include "NiagaraScript.h"
#include "MonolithNiagaraDependencyUsage.h"

/**
 * The DECISION CORE of niagara.validate_stack_dependencies — a faithful port of the rules the
 * Niagara editor itself applies when it decides that a placed module has an unmet dependency.
 *
 * WHY A PORT RATHER THAN A CALL.
 * The engine's implementation lives in
 * NiagaraStackModuleItem.cpp:800-911 (`GenerateDependencyIssues`) and reaches its helpers through
 * FNiagaraStackGraphUtilities::DependencyUtilities. Neither is available to us:
 *
 *   1. `GenerateDependencyIssues` is a file-static in an anonymous-ish `namespace
 *      NiagaraStackModuleItemIssues` inside a .cpp — no declaration exists anywhere.
 *   2. 🛑 `namespace DependencyUtilities` carries NO export macro
 *      (NiagaraStackGraphUtilities.h:378-385 — every other function in that header that we call is
 *      spelled `NIAGARAEDITOR_API`, these three are not). Calling
 *      `DoesStackModuleProvideDependency` would COMPILE AND THEN FAIL TO LINK.
 *   3. The whole path is driven from an `FNiagaraSystemViewModel`, which Monolith never constructs
 *      — the same wall that makes UNiagaraStackFunctionInput::Reset unreachable.
 *
 * So the rules are re-derived here from the engine source, line by line, with citations. Every
 * predicate below names the engine line it mirrors so a future engine version can be re-checked
 * by reading rather than by guessing.
 *
 * 🛑 READ-ONLY. Nothing in this header or its caller mutates an asset. The engine's equivalent code
 * also GENERATES FIXES; that half is deliberately NOT ported. The reason the metadata check is
 * worth having at all is that the native GetStackIssues route needs a compile to have been
 * requested first, mis-reports its own fixes, and offers alternatives that fail with "an acceptable
 * location could not be found". A metadata-derived check needs no compile — and it must therefore
 * never acquire the ability to break something.
 *
 * Unit-tested in Private/Tests/MonolithNiagaraStackDependencyTest.cpp.
 */
namespace MonolithNiagaraStackDependency
{
	/**
	 * Which dependency PHASE a stack usage counts as, for the purpose of
	 * FNiagaraModuleDependency::OnlyEvaluateInScriptUsage.
	 *
	 * Mirrors NiagaraStackModuleItem.cpp:603-622 exactly, INCLUDING the parts that are easy to get
	 * wrong by intuition:
	 *   - EmitterSpawn and SystemSpawn are BOTH `Spawn`; EmitterUpdate and SystemUpdate are both
	 *     `Update`. A mapping that only handles the Particle* usages returns `None` for the emitter
	 *     and system stages, and since bit 0 is never set that silently DISABLES every dependency
	 *     check in those stages.
	 *   - ParticleSpawnScriptInterpolated maps to `Spawn` (it is listed alongside ParticleSpawnScript
	 *     at :613).
	 *   - Anything else — including ENiagaraScriptUsage::Module itself — is `None`.
	 */
	inline ENiagaraModuleDependencyUsage ConvertScriptUsageToDependencyUsage(ENiagaraScriptUsage ScriptUsage)
	{
		if (ScriptUsage == ENiagaraScriptUsage::ParticleEventScript)
		{
			return ENiagaraModuleDependencyUsage::Event;
		}
		if (ScriptUsage == ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			return ENiagaraModuleDependencyUsage::SimulationStage;
		}
		if (ScriptUsage == ENiagaraScriptUsage::EmitterSpawnScript
			|| ScriptUsage == ENiagaraScriptUsage::SystemSpawnScript
			|| ScriptUsage == ENiagaraScriptUsage::ParticleSpawnScriptInterpolated
			|| ScriptUsage == ENiagaraScriptUsage::ParticleSpawnScript)
		{
			return ENiagaraModuleDependencyUsage::Spawn;
		}
		if (ScriptUsage == ENiagaraScriptUsage::EmitterUpdateScript
			|| ScriptUsage == ENiagaraScriptUsage::SystemUpdateScript
			|| ScriptUsage == ENiagaraScriptUsage::ParticleUpdateScript)
		{
			return ENiagaraModuleDependencyUsage::Update;
		}
		return ENiagaraModuleDependencyUsage::None;
	}

	/**
	 * Is a declared dependency enforced AT ALL in this stage?
	 *
	 * Mirrors NiagaraStackModuleItem.cpp:624-628: `AllowedUsageBitmask & (1 << (int32)Usage)`.
	 *
	 * ⚠️ The shift is over the ENUM VALUE, so the first real phase is bit 1 — see
	 * MonolithNiagaraDependencyUsage.h for the full account of that hazard. The consequence here is
	 * benign-looking and important: an unmapped usage yields `None` == 0, and testing bit 0 of a
	 * well-formed mask is always false, so unmapped stages are silently NOT CHECKED rather than
	 * wrongly checked. That is the engine's behaviour and we reproduce it, but it is the reason a
	 * caller is told which stages were skipped instead of being left to assume full coverage.
	 */
	inline bool IsUsageAllowed(ENiagaraScriptUsage ModuleUsage, int32 AllowedUsageBitmask)
	{
		const ENiagaraModuleDependencyUsage Usage = ConvertScriptUsageToDependencyUsage(ModuleUsage);
		return (AllowedUsageBitmask & (1 << static_cast<int32>(Usage))) != 0;
	}

	/**
	 * Pre vs Post — THE ordering rule, and the reason a stack has an order at all.
	 *
	 * Mirrors NiagaraStackModuleItem.cpp:845-847:
	 *     Pre  : provider index <  dependent index   (provider ABOVE)
	 *     Post : provider index >  dependent index   (provider BELOW)
	 *
	 * Both comparisons are STRICT. Stock GravityForce declares a POST dependency on
	 * SolveForcesAndVelocity, which is exactly why forces must sit ABOVE the solver.
	 */
	inline bool IsCorrectOrder(ENiagaraModuleDependencyType Type, int32 ProviderStackIndex, int32 DependentStackIndex)
	{
		return (Type == ENiagaraModuleDependencyType::PreDependency  && ProviderStackIndex < DependentStackIndex)
			|| (Type == ENiagaraModuleDependencyType::PostDependency && ProviderStackIndex > DependentStackIndex);
	}

	/**
	 * One module in the flattened stack that ADVERTISES the required id (i.e. its
	 * ProvidedDependencies contains it) and has already passed the SameScript/AllScripts filter.
	 *
	 * The caller does the provider search (it needs live nodes); this struct carries only the four
	 * facts the verdict actually turns on, which is what makes the verdict testable without an asset.
	 */
	struct FProviderCandidate
	{
		/** Index into the flattened, execution-ordered module list for the scope. */
		int32 StackIndex = INDEX_NONE;

		/** NiagaraStackModuleItem.cpp:849 — GetDesiredEnabledState() == ENodeEnabledState::Enabled. */
		bool bEnabled = true;

		/** NiagaraStackModuleItem.cpp:848/789-798 — FNiagaraModuleDependency::IsVersionAllowed. */
		bool bCorrectVersion = true;

		/**
		 * NiagaraStackModuleItem.cpp:850 — `ContainsEquivilentUsage(SupportedUsages, provider usage)`
		 * where SupportedUsages comes from the **DEPENDENT** module's ModuleUsageBitmask (:840, read
		 * off the SOURCE module's script data at :815).
		 *
		 * 🔺 That is almost certainly an engine BUG: the comment at :860 says "We can only reorder a
		 * module if it supports being moved to the usage of the target module", which wants the
		 * PROVIDER's bitmask. We mirror the engine anyway — the goal is to agree with what the editor
		 * reports, not to be right where it is wrong — and we flag it in the payload. It affects only
		 * the CLASSIFICATION of a finding (WrongOrder vs Missing), never whether one is emitted, so
		 * issue COUNTS are unaffected either way.
		 */
		bool bUsageSupportedByDependent = true;
	};

	/** What we concluded about one declared dependency of one placed module. */
	enum class EDependencyVerdict : uint8
	{
		/** A provider exists, is enabled, is on the correct side, and is an allowed version. */
		Satisfied,
		/** No usable provider AND nothing that could be fixed by reordering/enabling/versioning. */
		Missing,
		/** A provider exists but is on the wrong side of the dependent module. */
		WrongOrder,
		/** A provider exists in the right place but is disabled. */
		DisabledProvider,
		/** A provider exists in the right place but its version is outside RequiredVersion. */
		WrongVersion
	};

	/** Per-category candidate indices, so a caller can report WHICH module is the problem. */
	struct FVerdictDetail
	{
		TArray<int32> WrongOrderCandidates;
		TArray<int32> DisabledCandidates;
		TArray<int32> WrongVersionCandidates;
		int32 SatisfyingCandidateStackIndex = INDEX_NONE;
	};

	/**
	 * The verdict. A line-for-line port of NiagaraStackModuleItem.cpp:836-899.
	 *
	 * Three behaviours here are NOT what a reasonable independent implementation would choose, and
	 * all three are load-bearing for agreeing with the editor:
	 *
	 *   1. **The scan SHORT-CIRCUITS** (:852-856). The first candidate that is enabled + correctly
	 *      ordered + correctly versioned ends the loop, so later broken candidates are never
	 *      categorised. An implementation that classifies everything and then decides would report
	 *      spurious detail on a stack the editor considers clean.
	 *   2. **A wrong-order candidate is DISCARDED, not recorded, when `bUsageSupportedByDependent`
	 *      is false** (:858-865). If that is the only candidate, all three lists stay empty and the
	 *      verdict is `Missing` — the editor then offers "add a new provider" rather than "reorder".
	 *      So "Missing" does NOT strictly mean "nothing provides this id".
	 *   3. **The checks are an else-if CHAIN** (:858-873), so a candidate that is both disabled and
	 *      wrongly ordered counts ONLY as wrongly ordered.
	 *
	 * An empty candidate list yields `Missing`, never `Satisfied` — the vacuous-truth inversion that
	 * would make the whole checker silently useless.
	 */
	inline EDependencyVerdict EvaluateDependency(
		ENiagaraModuleDependencyType Type,
		int32 DependentStackIndex,
		TArrayView<const FProviderCandidate> Candidates,
		FVerdictDetail& OutDetail)
	{
		OutDetail = FVerdictDetail();

		for (const FProviderCandidate& Candidate : Candidates)
		{
			const bool bCorrectOrder = IsCorrectOrder(Type, Candidate.StackIndex, DependentStackIndex);

			// :852-856 — first fully-valid provider wins and STOPS the scan.
			if (Candidate.bEnabled && bCorrectOrder && Candidate.bCorrectVersion)
			{
				OutDetail.SatisfyingCandidateStackIndex = Candidate.StackIndex;
				return EDependencyVerdict::Satisfied;
			}

			if (!bCorrectOrder)
			{
				// :861-864 — recorded ONLY if the engine believes it could be moved.
				if (Candidate.bUsageSupportedByDependent)
				{
					OutDetail.WrongOrderCandidates.Add(Candidate.StackIndex);
				}
			}
			else if (!Candidate.bEnabled)
			{
				OutDetail.DisabledCandidates.Add(Candidate.StackIndex);
			}
			else if (!Candidate.bCorrectVersion)
			{
				OutDetail.WrongVersionCandidates.Add(Candidate.StackIndex);
			}
		}

		// :879-897 — precedence follows the engine's own fix-generation order.
		if (OutDetail.WrongOrderCandidates.Num() > 0)
		{
			return EDependencyVerdict::WrongOrder;
		}
		if (OutDetail.DisabledCandidates.Num() > 0)
		{
			return EDependencyVerdict::DisabledProvider;
		}
		if (OutDetail.WrongVersionCandidates.Num() > 0)
		{
			return EDependencyVerdict::WrongVersion;
		}
		return EDependencyVerdict::Missing;
	}

	/** Stable, machine-matchable verdict names for the JSON payload. */
	inline const TCHAR* VerdictToString(EDependencyVerdict Verdict)
	{
		switch (Verdict)
		{
		case EDependencyVerdict::Satisfied:        return TEXT("satisfied");
		case EDependencyVerdict::Missing:          return TEXT("missing");
		case EDependencyVerdict::WrongOrder:       return TEXT("wrong_order");
		case EDependencyVerdict::DisabledProvider: return TEXT("disabled_provider");
		case EDependencyVerdict::WrongVersion:     return TEXT("wrong_version");
		default:                                   return TEXT("unknown");
		}
	}
}
