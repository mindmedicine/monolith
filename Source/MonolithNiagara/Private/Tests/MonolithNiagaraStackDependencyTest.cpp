// SPDX-License-Identifier: MIT
// Automation tests for the decision core of niagara.validate_stack_dependencies.
//
// SCOPE, STATED HONESTLY. Only the pure rules are testable without a loaded system: the
// usage->phase mapping, the OnlyEvaluateInScriptUsage gate, the Pre/Post ordering comparison, and
// the candidate-verdict loop. Walking the ParameterMap chain, flattening the stack in execution
// order, resolving SameScript against the real output node, and the asset-registry remedy lookup
// ALL need a live UNiagaraSystem and are validated in-editor.
//
// 🛑 A GREEN RUN HERE IS NOT EVIDENCE THAT THE ACTION WORKS. It is evidence that the rules it
// applies match the engine's, which is a different and smaller claim.
//
// Every block below names the WRONG BEHAVIOUR it catches. A test that cannot fail is worse than no
// test, because it reads as coverage.

#include "Misc/AutomationTest.h"
#include "MonolithNiagaraStackDependency.h"
#include "NiagaraCommon.h"
#include "NiagaraScript.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace MonolithNiagaraStackDependency;

namespace
{
	FProviderCandidate MakeCandidate(int32 StackIndex, bool bEnabled = true, bool bCorrectVersion = true,
		bool bUsageSupportedByDependent = true)
	{
		FProviderCandidate C;
		C.StackIndex = StackIndex;
		C.bEnabled = bEnabled;
		C.bCorrectVersion = bCorrectVersion;
		C.bUsageSupportedByDependent = bUsageSupportedByDependent;
		return C;
	}

	// Verdicts are compared as their JSON-visible STRINGS rather than as enum values. Two reasons:
	// FAutomationTestBase::TestEqual has no overload for a scoped enum (it would not compile), and
	// comparing the string additionally pins the payload contract — a rename of "wrong_order" is a
	// breaking change for any caller matching on it, and would otherwise pass silently.
	FString VerdictName(EDependencyVerdict Verdict)
	{
		return FString(VerdictToString(Verdict));
	}

	// Severity is likewise compared as its payload STRING: "problem" / "question" / "satisfied" are
	// what a caller branches on, so a rename must fail here rather than in the caller.
	FString SeverityName(EDependencyVerdict Verdict)
	{
		return FString(SeverityToString(ClassifyVerdict(Verdict)));
	}

	// Same reason: an int32 comparison is the one form guaranteed to resolve for a scoped enum.
	int32 PhaseOf(ENiagaraScriptUsage Usage)
	{
		return static_cast<int32>(ConvertScriptUsageToDependencyUsage(Usage));
	}

	int32 Phase(ENiagaraModuleDependencyUsage Usage)
	{
		return static_cast<int32>(Usage);
	}
}

// ============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraStackDependencyOrderTest,
	"Monolith.Niagara.StackDependencies.Ordering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraStackDependencyOrderTest::RunTest(const FString& /*Parameters*/)
{
	const ENiagaraModuleDependencyType Post = ENiagaraModuleDependencyType::PostDependency;
	const ENiagaraModuleDependencyType Pre  = ENiagaraModuleDependencyType::PreDependency;

	// --- ROW 1: the GravityForce case, in the direction that actually matters ----------------
	// CATCHES an inverted Pre/Post comparison. This is the single most likely bug in the whole
	// action and it fails in both directions at once: a correctly-built stack (forces ABOVE the
	// solver) gets reported as broken, and the genuinely broken stack gets reported as clean. The
	// measured fixture is exactly this — GravityForce declares a POST dependency on
	// SolveForcesAndVelocity, which is WHY forces must sit above the solver.
	{
		TestTrue(TEXT("Post: provider BELOW the dependent is correct (solver under GravityForce)"),
			IsCorrectOrder(Post, /*Provider*/ 5, /*Dependent*/ 3));
		TestFalse(TEXT("Post: provider ABOVE the dependent is WRONG (solver over GravityForce)"),
			IsCorrectOrder(Post, /*Provider*/ 1, /*Dependent*/ 3));
	}

	// --- ROW 2: the mirror direction ---------------------------------------------------------
	// CATCHES an implementation that hardcodes Post semantics and ignores Type — which would still
	// pass ROW 1 completely, and would then silently mis-judge every Pre dependency in the project.
	{
		TestTrue(TEXT("Pre: provider ABOVE the dependent is correct"),
			IsCorrectOrder(Pre, /*Provider*/ 1, /*Dependent*/ 3));
		TestFalse(TEXT("Pre: provider BELOW the dependent is WRONG"),
			IsCorrectOrder(Pre, /*Provider*/ 5, /*Dependent*/ 3));
	}

	// --- ROW 3: strictness at the boundary ---------------------------------------------------
	// CATCHES `<=` / `>=`. With a non-strict comparison a module at the same index as itself
	// satisfies its own dependency, so any module that both requires and provides an id would
	// self-certify and the check would pass on a stack the editor rejects.
	{
		TestFalse(TEXT("Post: equal indices do NOT satisfy (strict >)"), IsCorrectOrder(Post, 3, 3));
		TestFalse(TEXT("Pre: equal indices do NOT satisfy (strict <)"),  IsCorrectOrder(Pre,  3, 3));
	}

	return true;
}

// ============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraStackDependencyVerdictTest,
	"Monolith.Niagara.StackDependencies.Verdict",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraStackDependencyVerdictTest::RunTest(const FString& /*Parameters*/)
{
	const ENiagaraModuleDependencyType Post = ENiagaraModuleDependencyType::PostDependency;
	const int32 DependentIndex = 3;

	// --- ROW 1: no candidates at all ---------------------------------------------------------
	// The NS_FixBtn_A shape: the solver was removed, so nothing in the stack advertises the id.
	// CATCHES the vacuous-truth inversion — an implementation whose loop simply never runs and
	// falls through to "satisfied". That single bug makes the entire action silently useless while
	// returning a confident, empty, green result.
	{
		FVerdictDetail Detail;
		const EDependencyVerdict V = EvaluateDependency(Post, DependentIndex, TArrayView<const FProviderCandidate>(), Detail);
		TestEqual(TEXT("no provider anywhere is MISSING, never satisfied"), VerdictName(V), FString(TEXT("missing")));
		TestEqual(TEXT("nothing is reported as satisfying"), Detail.SatisfyingCandidateStackIndex, INDEX_NONE);
	}

	// --- ROW 2: a good provider on the correct side ------------------------------------------
	// CATCHES an over-eager checker that reports a finding on a healthy stack. Noise here is not
	// cosmetic: the whole point of this action is to be trusted without a compile, and a validator
	// that cries wolf on stock content gets ignored.
	{
		const TArray<FProviderCandidate> Candidates = { MakeCandidate(5) };
		FVerdictDetail Detail;
		const EDependencyVerdict V = EvaluateDependency(Post, DependentIndex, Candidates, Detail);
		TestEqual(TEXT("enabled + correctly ordered + correct version is SATISFIED"), VerdictName(V), FString(TEXT("satisfied")));
		TestEqual(TEXT("the satisfying provider is identified"), Detail.SatisfyingCandidateStackIndex, 5);
	}

	// --- ROW 3: the NS_FixBtn_B shape --------------------------------------------------------
	// A provider exists but sits on the wrong side. CATCHES conflating "present" with "satisfied" —
	// i.e. a presence-only check. That implementation reports 0 findings on the wrong-order fixture
	// where the engine reports 2, and it is the most tempting shortcut available.
	{
		const TArray<FProviderCandidate> Candidates = { MakeCandidate(1) };
		FVerdictDetail Detail;
		const EDependencyVerdict V = EvaluateDependency(Post, DependentIndex, Candidates, Detail);
		TestEqual(TEXT("a present but mis-ordered provider is WRONG_ORDER, not satisfied"), VerdictName(V), FString(TEXT("wrong_order")));
		TestEqual(TEXT("the mis-ordered provider is named"), Detail.WrongOrderCandidates.Num(), 1);
		TestEqual(TEXT("...and it is the right one"), Detail.WrongOrderCandidates[0], 1);
	}

	// --- ROW 4: correctly placed but DISABLED ------------------------------------------------
	// CATCHES dropping the enabled check (NiagaraStackModuleItem.cpp:849). A disabled module does
	// not run, so the dependency is genuinely unmet — but it LOOKS present in every listing we have.
	// This is the failure mode that a human eyeballing the stack also misses.
	{
		const TArray<FProviderCandidate> Candidates = { MakeCandidate(5, /*bEnabled*/ false) };
		FVerdictDetail Detail;
		const EDependencyVerdict V = EvaluateDependency(Post, DependentIndex, Candidates, Detail);
		TestEqual(TEXT("a correctly placed but DISABLED provider does not satisfy"), VerdictName(V), FString(TEXT("disabled_provider")));
		TestEqual(TEXT("the disabled provider is named"), Detail.DisabledCandidates.Num(), 1);
	}

	// --- ROW 5: correctly placed, enabled, WRONG VERSION -------------------------------------
	// CATCHES ignoring RequiredVersion (FNiagaraModuleDependency::IsVersionAllowed, NiagaraScript.h:167).
	// A versioned module can advertise the id and still be an unusable version; skipping this check
	// turns a real error into a clean bill of health.
	{
		const TArray<FProviderCandidate> Candidates = { MakeCandidate(5, /*bEnabled*/ true, /*bCorrectVersion*/ false) };
		FVerdictDetail Detail;
		const EDependencyVerdict V = EvaluateDependency(Post, DependentIndex, Candidates, Detail);
		TestEqual(TEXT("a provider of a disallowed version does not satisfy"), VerdictName(V), FString(TEXT("wrong_version")));
		TestEqual(TEXT("the wrong-version provider is named"), Detail.WrongVersionCandidates.Num(), 1);
	}

	// --- ROW 6: the short-circuit ------------------------------------------------------------
	// A valid provider FOLLOWED by a broken one. CATCHES an implementation that categorises every
	// candidate before deciding: it would still answer "satisfied" but would also report the second
	// module as a problem, so the payload would accuse a stack the editor considers clean.
	// The engine breaks out of the loop at :855; we must too.
	{
		const TArray<FProviderCandidate> Candidates = {
			MakeCandidate(5),                              // valid — ends the scan
			MakeCandidate(1)                               // mis-ordered — must never be recorded
		};
		FVerdictDetail Detail;
		const EDependencyVerdict V = EvaluateDependency(Post, DependentIndex, Candidates, Detail);
		TestEqual(TEXT("a later broken candidate cannot spoil an earlier valid one"), VerdictName(V), FString(TEXT("satisfied")));
		TestEqual(TEXT("the scan STOPPED — the later candidate was never categorised"),
			Detail.WrongOrderCandidates.Num(), 0);
	}

	// --- ROW 7: the engine's surprising usage gate -------------------------------------------
	// A mis-ordered provider whose usage the DEPENDENT's bitmask does not support is discarded
	// rather than recorded (NiagaraStackModuleItem.cpp:858-865), leaving all three lists empty and
	// the verdict MISSING.
	// CATCHES a "reasonable" implementation that reports WRONG_ORDER here. It would disagree with
	// the editor about which fix is offered — the editor offers "add a new provider", not
	// "reorder" — and that is precisely the case where picking the wrong alternative fails with
	// "an acceptable location could not be found".
	// 🔺 Also pins the engine quirk itself: if Epic ever fixes :840 to read the PROVIDER's bitmask,
	// this row fails and tells us the port needs revisiting.
	{
		const TArray<FProviderCandidate> Candidates = {
			MakeCandidate(1, /*bEnabled*/ true, /*bCorrectVersion*/ true, /*bUsageSupportedByDependent*/ false)
		};
		FVerdictDetail Detail;
		const EDependencyVerdict V = EvaluateDependency(Post, DependentIndex, Candidates, Detail);
		TestEqual(TEXT("an unmovable mis-ordered provider yields MISSING, not WRONG_ORDER"), VerdictName(V), FString(TEXT("missing")));
		TestEqual(TEXT("...and it is NOT recorded as a reorder candidate"), Detail.WrongOrderCandidates.Num(), 0);
	}

	// --- ROW 8: else-if chain precedence -----------------------------------------------------
	// A candidate that is BOTH mis-ordered and disabled counts only as mis-ordered (:858-869).
	// CATCHES an implementation using independent `if`s, which would double-report one module in
	// two categories and make the finding read as two separate problems.
	{
		const TArray<FProviderCandidate> Candidates = { MakeCandidate(1, /*bEnabled*/ false) };
		FVerdictDetail Detail;
		const EDependencyVerdict V = EvaluateDependency(Post, DependentIndex, Candidates, Detail);
		TestEqual(TEXT("mis-ordered wins over disabled"), VerdictName(V), FString(TEXT("wrong_order")));
		TestEqual(TEXT("the module is counted once, as mis-ordered"), Detail.WrongOrderCandidates.Num(), 1);
		TestEqual(TEXT("...and NOT also as disabled"), Detail.DisabledCandidates.Num(), 0);
	}

	return true;
}

// ============================================================================================
// SEVERITY — the intent-vs-validity split (2026-08-18).
//
// Every row here FAILS against the version of this action written on 2026-08-17, which called every
// unmet dependency `is_problem: true`. That checker is confidently wrong on a real authoring
// pattern: force modules write TRANSIENTS (Output.Module.* / Transient.PhysicsForce) and Solve
// Forces and Velocity merely INTEGRATES them. Remove or disable the solver and the forces still
// compute their transients — which is exactly what you want when a Niagara fluids solver does the
// solving, or when the values are consumed directly (written to an NDC) without ever touching this
// particle's position or velocity. Tim, 2026-08-18.
//
// 🛑 The split must stay derivable from the VERDICT, never from a module name. Nothing in this file
// or in ClassifyVerdict mentions SolveForcesAndVelocity, and a fix that special-cased it would pass
// these rows while remaining wrong for every other dependency id in the project.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraStackDependencySeverityTest,
	"Monolith.Niagara.StackDependencies.Severity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraStackDependencySeverityTest::RunTest(const FString& /*Parameters*/)
{
	const ENiagaraModuleDependencyType Post = ENiagaraModuleDependencyType::PostDependency;
	const int32 DependentIndex = 3;

	// --- ROW 1: NO PROVIDER AT ALL is a QUESTION ---------------------------------------------
	// The NS_FixBtn_A / fluids shape. CATCHES the pre-2026-08-18 behaviour directly: it reported
	// is_problem=true here, so a correct fluids stack (curl noise forces, no solver, the fluid sim
	// integrating) came back flagged as broken. That is the failure mode that gets a validator
	// ignored — and an ignored validator loses us every finding it would have got right.
	{
		FVerdictDetail Detail;
		const EDependencyVerdict V = EvaluateDependency(Post, DependentIndex, TArrayView<const FProviderCandidate>(), Detail);
		TestEqual(TEXT("missing is still the VERDICT — the split changes severity, not the rules"),
			VerdictName(V), FString(TEXT("missing")));
		TestEqual(TEXT("a missing provider is a PROBABLE problem we must ask about, not an asserted defect"),
			SeverityName(V), FString(TEXT("probable_problem")));
		TestFalse(TEXT("...so is_problem must be FALSE"), ClassifyVerdict(V) == EVerdictSeverity::DefiniteProblem);
	}

	// --- ROW 2: a correctly placed but DISABLED provider is a QUESTION ------------------------
	// Tim's case verbatim: disabling the solver keeps the forces' transients and stops the
	// integration. Disabling is an explicit authoring ACT, so it is a stronger statement of intent
	// than mere absence. CATCHES both the old blanket is_problem AND a half-fix that special-cased
	// only `missing` — the tempting shortcut, since "a provider is right there" reads like a defect.
	{
		const TArray<FProviderCandidate> Candidates = { MakeCandidate(5, /*bEnabled*/ false) };
		FVerdictDetail Detail;
		const EDependencyVerdict V = EvaluateDependency(Post, DependentIndex, Candidates, Detail);
		TestEqual(TEXT("the verdict is unchanged"), VerdictName(V), FString(TEXT("disabled_provider")));
		TestEqual(TEXT("a disabled provider is a PROBABLE problem we must ask about"),
			SeverityName(V), FString(TEXT("probable_problem")));
		TestFalse(TEXT("...so is_problem must be FALSE"), ClassifyVerdict(V) == EVerdictSeverity::DefiniteProblem);
	}

	// --- ROW 3: a MIS-ORDERED provider is still a PROBLEM -------------------------------------
	// The NS_FixBtn_B shape. CATCHES the OVER-correction — demoting everything unmet to advisory,
	// which would leave the action asserting nothing at all and make problem_count permanently 0.
	// The provider is PRESENT, so the intent to use it is unambiguous and there is no effect an
	// author achieves by putting it on the wrong side. Nothing legitimate about it.
	{
		const TArray<FProviderCandidate> Candidates = { MakeCandidate(1) };
		FVerdictDetail Detail;
		const EDependencyVerdict V = EvaluateDependency(Post, DependentIndex, Candidates, Detail);
		TestEqual(TEXT("the verdict is unchanged"), VerdictName(V), FString(TEXT("wrong_order")));
		TestEqual(TEXT("a present-but-mis-ordered provider is DEFINITELY a problem"),
			SeverityName(V), FString(TEXT("problem")));
		TestTrue(TEXT("...so is_problem must be TRUE"), ClassifyVerdict(V) == EVerdictSeverity::DefiniteProblem);
	}

	// --- ROW 4: wrong version follows the same principle as wrong order ----------------------
	// A correctly placed, enabled provider of a disallowed version is an accident of versioning,
	// never a statement of intent. CATCHES a fix that enumerated only `wrong_order` as a problem and
	// let everything else fall through to advisory — the same silent-disarmament bug as ROW 3, but
	// on the verdict people forget exists.
	{
		const TArray<FProviderCandidate> Candidates = { MakeCandidate(5, /*bEnabled*/ true, /*bCorrectVersion*/ false) };
		FVerdictDetail Detail;
		const EDependencyVerdict V = EvaluateDependency(Post, DependentIndex, Candidates, Detail);
		TestEqual(TEXT("a disallowed-version provider is DEFINITELY a problem"),
			SeverityName(V), FString(TEXT("problem")));
	}

	// --- ROW 5: the two flags are mutually exclusive, and satisfied is neither -----------------
	// CATCHES a payload where a caller filtering on `needs_author_decision` also picks up hard
	// problems (or vice versa) — i.e. the distinction existing in name only. A satisfied finding
	// (returned when include_satisfied=true) must set NEITHER flag.
	{
		const TArray<EDependencyVerdict> AllVerdicts = {
			EDependencyVerdict::Satisfied, EDependencyVerdict::Missing, EDependencyVerdict::WrongOrder,
			EDependencyVerdict::DisabledProvider, EDependencyVerdict::WrongVersion };
		for (const EDependencyVerdict V : AllVerdicts)
		{
			const EVerdictSeverity S = ClassifyVerdict(V);
			const bool bIsProblem = S == EVerdictSeverity::DefiniteProblem;
			const bool bNeedsDecision = S == EVerdictSeverity::AuthorDecision;
			TestFalse(FString::Printf(TEXT("%s is never both a problem and a question"), VerdictToString(V)),
				bIsProblem && bNeedsDecision);
		}
		TestEqual(TEXT("satisfied carries neither flag"),
			SeverityName(EDependencyVerdict::Satisfied), FString(TEXT("satisfied")));
	}

	// --- ROW 6: the engine-quirk case lands on the SAFE side ----------------------------------
	// A mis-ordered provider the engine refuses to offer a reorder for yields the verdict `missing`
	// (Verdict ROW 7), and therefore a QUESTION — even though a provider is physically present.
	// That is a deliberate consequence of mirroring the engine, recorded rather than papered over:
	// it errs toward ASKING where we cannot be sure, which is the direction that cannot make the
	// checker confidently wrong. CATCHES anyone "improving" severity by re-deriving it from the
	// candidate list instead of from the verdict, which would silently un-mirror the engine.
	{
		const TArray<FProviderCandidate> Candidates = {
			MakeCandidate(1, /*bEnabled*/ true, /*bCorrectVersion*/ true, /*bUsageSupportedByDependent*/ false)
		};
		FVerdictDetail Detail;
		const EDependencyVerdict V = EvaluateDependency(Post, DependentIndex, Candidates, Detail);
		TestEqual(TEXT("the unmovable mis-ordered provider is still MISSING"), VerdictName(V), FString(TEXT("missing")));
		TestEqual(TEXT("...and therefore asked about, erring toward asking"),
			SeverityName(V), FString(TEXT("probable_problem")));
	}

	// --- ROW 7: the ADVISORY MUST NOT READ AS BENIGN ------------------------------------------
	// Tim's calibration, 2026-08-18: "98% of the cases we'll want the solver to be present and
	// active. if it's inactive, just ask." The split exists so we stop ASSERTING what we cannot
	// know — it does NOT downgrade the finding.
	// CATCHES the over-correction in its subtler form: a payload that keeps the split but words it
	// away, so a caller skimming severity or severity_meaning concludes "no action needed". A name
	// like "info"/"advisory"/"ok", or a meaning string that omits the weight, is a real regression
	// because these findings USUALLY still want fixing — and the failure mode they describe compiles
	// clean and does nothing.
	{
		const FString AdvisoryName = SeverityName(EDependencyVerdict::Missing);
		TestFalse(TEXT("the advisory severity is not named as benign (info)"),  AdvisoryName.Contains(TEXT("info")));
		TestFalse(TEXT("...nor as merely advisory"), AdvisoryName.Contains(TEXT("advisory")));
		TestTrue(TEXT("...it names the word PROBLEM, so a skimming caller cannot mistake it"),
			AdvisoryName.Contains(TEXT("problem")));

		const FString Meaning = FString(SeverityMeaning(EVerdictSeverity::AuthorDecision));
		TestTrue(TEXT("the per-finding meaning states it is PROBABLY WRONG"),
			Meaning.Contains(TEXT("PROBABLY WRONG")));
		TestTrue(TEXT("...and explicitly denies the 'no action needed' reading"),
			Meaning.Contains(TEXT("is_problem is false because we cannot tell")));
	}

	return true;
}

// ============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraStackDependencyUsageGateTest,
	"Monolith.Niagara.StackDependencies.UsageGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraStackDependencyUsageGateTest::RunTest(const FString& /*Parameters*/)
{
	const int32 SpawnBit  = 1 << static_cast<int32>(ENiagaraModuleDependencyUsage::Spawn);
	const int32 UpdateBit = 1 << static_cast<int32>(ENiagaraModuleDependencyUsage::Update);
	const int32 EventBit  = 1 << static_cast<int32>(ENiagaraModuleDependencyUsage::Event);

	// --- ROW 1: the phase mapping, including the stages intuition forgets --------------------
	// CATCHES a mapping that only handles the Particle* usages. Because `None` is 0 and bit 0 is
	// never set in a well-formed mask, an unmapped stage evaluates to "not enforced" — so this bug
	// SILENTLY DISABLES every dependency check in the emitter and system stages while the action
	// still returns a confident zero-findings result for them.
	{
		TestEqual(TEXT("EmitterSpawn counts as the Spawn phase"),
			PhaseOf(ENiagaraScriptUsage::EmitterSpawnScript), Phase(ENiagaraModuleDependencyUsage::Spawn));
		TestEqual(TEXT("SystemUpdate counts as the Update phase"),
			PhaseOf(ENiagaraScriptUsage::SystemUpdateScript), Phase(ENiagaraModuleDependencyUsage::Update));
		TestEqual(TEXT("interpolated particle spawn still counts as Spawn"),
			PhaseOf(ENiagaraScriptUsage::ParticleSpawnScriptInterpolated), Phase(ENiagaraModuleDependencyUsage::Spawn));
		TestEqual(TEXT("particle event maps to Event"),
			PhaseOf(ENiagaraScriptUsage::ParticleEventScript), Phase(ENiagaraModuleDependencyUsage::Event));
		TestEqual(TEXT("simulation stage maps to SimulationStage"),
			PhaseOf(ENiagaraScriptUsage::ParticleSimulationStageScript), Phase(ENiagaraModuleDependencyUsage::SimulationStage));
	}

	// --- ROW 2: a usage that is not a stack phase at all --------------------------------------
	// CATCHES a mapping with a permissive default (e.g. falling back to Spawn). ENiagaraScriptUsage::
	// Module is the usage of the module ASSET itself, never of a stage; treating it as a phase would
	// make the gate pass in contexts the engine never evaluates.
	{
		TestEqual(TEXT("the Module usage is not a stack phase"),
			PhaseOf(ENiagaraScriptUsage::Module), Phase(ENiagaraModuleDependencyUsage::None));
	}

	// --- ROW 3: the gate itself ---------------------------------------------------------------
	// GravityForce's measured mask is Spawn|Update|Event. CATCHES the bit-0 off-by-one from the
	// other side: under a shifted decode, testing ParticleUpdate against this mask reads the wrong
	// bit and the dependency is evaluated in the wrong stages.
	{
		const int32 GravityForceMask = SpawnBit | UpdateBit | EventBit;
		TestTrue(TEXT("Spawn|Update|Event IS enforced in particle update"),
			IsUsageAllowed(ENiagaraScriptUsage::ParticleUpdateScript, GravityForceMask));
		TestTrue(TEXT("...and in particle spawn"),
			IsUsageAllowed(ENiagaraScriptUsage::ParticleSpawnScript, GravityForceMask));
		TestFalse(TEXT("...but NOT in a simulation stage, which the mask omits"),
			IsUsageAllowed(ENiagaraScriptUsage::ParticleSimulationStageScript, GravityForceMask));
	}

	// --- ROW 4: an empty mask -----------------------------------------------------------------
	// CATCHES a gate that falls back to "enforced everywhere" when the mask is 0. That inversion
	// turns "enforced nowhere" into findings on every stage of every system — the loudest possible
	// false positive, and one that would be blamed on the assets rather than on us.
	{
		TestFalse(TEXT("an empty mask enforces the dependency NOWHERE (spawn)"),
			IsUsageAllowed(ENiagaraScriptUsage::ParticleSpawnScript, 0));
		TestFalse(TEXT("an empty mask enforces the dependency NOWHERE (update)"),
			IsUsageAllowed(ENiagaraScriptUsage::ParticleUpdateScript, 0));
	}

	// --- ROW 5: bit 0 is not a phase ----------------------------------------------------------
	// A mask of exactly 1 sets only the hidden `None` sentinel. CATCHES a gate indexing VISIBLE enum
	// entries instead of enum VALUES, which would read bit 0 as Spawn and enforce a dependency that
	// the asset restricts to nothing.
	{
		TestFalse(TEXT("a mask of only bit 0 does not enforce Spawn"),
			IsUsageAllowed(ENiagaraScriptUsage::ParticleSpawnScript, 1));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
