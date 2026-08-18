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

	/**
	 * How much a non-satisfied verdict is worth ASSERTING.
	 *
	 * 🛑 WHY THIS EXISTS. A stack issue is about INTENT; a compile error is about VALIDITY (Tim,
	 * 2026-08-18). The engine reports both classes through one channel, and the first version of this
	 * checker inherited that conflation — it called every unmet dependency `is_problem: true`, which
	 * makes it CONFIDENTLY WRONG on a real authoring pattern:
	 *
	 *   Every force module reads what it needs and WRITES TRANSIENTS (Output.Module.* /
	 *   Transient.PhysicsForce / Transient.PhysicsDrag). Solve Forces and Velocity is what READS those
	 *   transients and integrates them into position and velocity. Remove or disable the solver and
	 *   the forces still compute their transients — they are simply never integrated. Tim's words:
	 *   "sometimes I only need those transient values to write to NDC without actually influencing the
	 *   particle's position or velocity. I sometimes use this in niagara fluids context where I want
	 *   to have additional curl noise forces but let the fluid solver do the actual solving."
	 *
	 * So an absent or switched-off provider is a LEGITIMATE STACK, and a validator that flags it gets
	 * ignored — which costs us every finding it would otherwise have got right.
	 *
	 * THE SPLIT IS PRINCIPLED, NOT A SPECIAL CASE. Nothing here knows the name
	 * `SolveForcesAndVelocity`, or any module name at all. The question asked of each verdict is:
	 * COULD AN AUTHOR HAVE PRODUCED THIS STATE ON PURPOSE, AS AN EXPRESSION OF INTENT?
	 *
	 *   - `Missing` — YES. Not adding a provider is exactly how you say "something else integrates
	 *     this" or "I want the transients only".
	 *   - `DisabledProvider` — YES, and more strongly: disabling a module is an explicit authoring
	 *     ACT, not an omission. This is Tim's case verbatim.
	 *   - `WrongOrder` — NO. The provider is present, so the intent to use it is unambiguous; only the
	 *     ordering is wrong. There is no effect an author achieves by putting it on the wrong side.
	 *   - `WrongVersion` — NO, for the same reason: a correctly-placed, enabled provider of a
	 *     disallowed version is an accident of versioning, never a statement.
	 *
	 * 🛑 CALIBRATION — AN AuthorDecision VERDICT IS "PROBABLY WRONG, CONFIRM", NOT "PROBABLY FINE".
	 * Tim, 2026-08-18: "98% of the cases we'll want the solver to be present and active. IF IT'S
	 * INACTIVE, JUST ASK." The fluids/NDC pattern is the RARE case. So the split exists to stop us
	 * ASSERTING what we cannot know — it does NOT downgrade the finding's visibility, and
	 * `is_problem: false` must never be read as "no action needed". These findings sit in `findings[]`
	 * alongside the definite ones, carry their own prominent count, and their remedy ASKS while
	 * saying plainly that the usual answer is to add or enable the provider.
	 *
	 * ⚠️ And the stakes are asymmetric: the failure mode of a genuinely unmet dependency is that the
	 * forces compute and are never integrated, so the system COMPILES CLEAN, LOOKS CORRECT AND DOES
	 * NOTHING. No instrument we have can see it. A silent 2% false-ask is cheap; a silent 98% miss is
	 * not.
	 */
	enum class EVerdictSeverity : uint8
	{
		/** The dependency is met. */
		Satisfied,
		/** Definitely wrong: the author's intent is unambiguous and the stack does not express it. */
		DefiniteProblem,
		/**
		 * Unmet and PROBABLY wrong, but it could be deliberate — so we ask instead of asserting.
		 * ⚠️ Not a lesser finding: usually it still wants fixing (see the calibration above).
		 */
		AuthorDecision
	};

	inline EVerdictSeverity ClassifyVerdict(EDependencyVerdict Verdict)
	{
		switch (Verdict)
		{
		case EDependencyVerdict::Satisfied:
			return EVerdictSeverity::Satisfied;

		// Provider present and unambiguously intended — only its placement/version is wrong.
		case EDependencyVerdict::WrongOrder:
		case EDependencyVerdict::WrongVersion:
			return EVerdictSeverity::DefiniteProblem;

		// The absence, or the deliberate switching-off, MAY ITSELF BE THE INTENT.
		case EDependencyVerdict::Missing:
		case EDependencyVerdict::DisabledProvider:
		default:
			return EVerdictSeverity::AuthorDecision;
		}
	}

	/**
	 * Stable, machine-matchable severity names for the JSON payload.
	 *
	 * ⚠️ The advisory value is "probable_problem", NOT "question" or "info". A caller sorting or
	 * filtering on this string must not be able to read the advisory class as benign — that is the
	 * whole calibration above, expressed in the one field a skimming reader actually sees.
	 */
	inline const TCHAR* SeverityToString(EVerdictSeverity Severity)
	{
		switch (Severity)
		{
		case EVerdictSeverity::Satisfied:       return TEXT("satisfied");
		case EVerdictSeverity::DefiniteProblem: return TEXT("problem");
		case EVerdictSeverity::AuthorDecision:  return TEXT("probable_problem");
		default:                                return TEXT("unknown");
		}
	}

	/**
	 * The one-line weight statement that ships INSIDE each finding, so a caller reading only the
	 * findings array (never how_to_read) still gets the calibration.
	 */
	inline const TCHAR* SeverityMeaning(EVerdictSeverity Severity)
	{
		switch (Severity)
		{
		case EVerdictSeverity::Satisfied:
			return TEXT("MET — a provider is present, enabled, correctly ordered and of an allowed version.");
		case EVerdictSeverity::DefiniteProblem:
			return TEXT("DEFINITELY WRONG — a provider IS present, so the intent to use it is unambiguous and only "
						"its placement or version is off. Fix it.");
		case EVerdictSeverity::AuthorDecision:
			return TEXT("PROBABLY WRONG — CONFIRM, DO NOT IGNORE. In the large majority of stacks this provider "
						"should be present and active, and an unmet dependency of this kind compiles clean, looks "
						"correct and silently does nothing. It is only NOT a defect if something else performs the "
						"integration or the transient values are consumed directly. is_problem is false because we "
						"cannot tell from metadata — NOT because no action is needed.");
		default:
			return TEXT("unknown");
		}
	}
}
