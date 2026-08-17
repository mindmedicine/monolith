// SPDX-License-Identifier: MIT
// REGRESSION GUARD — a schema must DECLARE every param spelling its handler accepts.
//
// Gap #72 gave batch_execute's sub-ops the registry's required-param validation. That was
// correct, but it converted a whole class of latent schema/handler mismatches into hard
// failures: wherever a handler read several spellings for one logical param via a chained
// fallback
//
//     FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
//     if (ModuleNodeGuid.IsEmpty()) ModuleNodeGuid = Params->GetStringField(TEXT("module_name"));
//     if (ModuleNodeGuid.IsEmpty()) ModuleNodeGuid = Params->GetStringField(TEXT("module"));
//
// while its schema declared only `module_node` with no aliases, the alternate spellings had
// always been tolerated by the handler and were now rejected before reaching it. Measured
// casualty: niagara.set_module_input spelled with `module` — a documented batch spelling —
// started answering "Missing required param(s): [module_node]".
//
// The fix is to DECLARE the aliases, never to remove the handler fallbacks (~12 internal
// callers build param objects and invoke handlers directly, bypassing the registry entirely)
// and never to weaken the validation.
//
// A plugin-wide sweep found 27 such mismatches. This file pins every one of them.
//
// HOW THE ASSERTION WORKS, and why it is exact: FMonolithParamSchema::ApplyAliases rewrites
// a key ONLY if the schema declares it as an alias of a canonical param. An UNDECLARED key is
// left exactly where it is. So "supply only the alternate spelling, then observe that the
// object now carries the canonical key and no longer carries the alternate" is a direct,
// behavioural proof that the declaration exists — it cannot pass by accident, and it fails the
// moment someone deletes an alias list. Validation may still report OTHER required params as
// missing; that is irrelevant here and deliberately not asserted, because ApplyAliases runs
// before the required-param check and the rewrite is the property under test.

#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithParamAliasDeclarationTestDetail
{
	/** One handler-side fallback spelling that the schema must declare. */
	struct FAliasCase
	{
		const TCHAR* Namespace;
		const TCHAR* Action;
		const TCHAR* Canonical;
		const TCHAR* Alias;
		const TCHAR* HandlerEvidence;   // file:line of the handler read that accepts it
	};

	// The complete sweep result. Every row is a measured handler fallback cross-referenced
	// against its registered schema. Keep this table in sync with the handlers: if a fallback
	// is added, add the alias AND the row.
	static const FAliasCase Cases[] =
	{
		// --- niagara: module/input identity on the stack-editing actions ------------------
		{ TEXT("niagara"), TEXT("set_module_input_value"),   TEXT("module_node"),    TEXT("module_name"),    TEXT("MonolithNiagaraActions.cpp:6924") },
		{ TEXT("niagara"), TEXT("set_module_input_value"),   TEXT("module_node"),    TEXT("module"),         TEXT("MonolithNiagaraActions.cpp:6925") },
		{ TEXT("niagara"), TEXT("set_module_input_value"),   TEXT("input"),          TEXT("input_name"),     TEXT("MonolithNiagaraActions.cpp:6927") },
		{ TEXT("niagara"), TEXT("get_script_details"),            TEXT("script_path"),  TEXT("asset_path"),   TEXT("MonolithNiagaraActions.cpp:24678") },
		{ TEXT("niagara"), TEXT("get_script_details"),            TEXT("version"),      TEXT("version_guid"), TEXT("MonolithNiagaraActions.cpp:24709") },
		{ TEXT("niagara"), TEXT("reset_module_input_to_default"), TEXT("module_node"), TEXT("module_name"),  TEXT("MonolithNiagaraActions.cpp:18623") },
		{ TEXT("niagara"), TEXT("reset_module_input_to_default"), TEXT("module_node"), TEXT("module"),      TEXT("MonolithNiagaraActions.cpp:18624") },
		{ TEXT("niagara"), TEXT("reset_module_input_to_default"), TEXT("input"),       TEXT("input_name"),  TEXT("MonolithNiagaraActions.cpp:18626") },
		{ TEXT("niagara"), TEXT("set_module_input_binding"), TEXT("module_node"),    TEXT("module_name"),    TEXT("MonolithNiagaraActions.cpp:7185") },
		{ TEXT("niagara"), TEXT("set_module_input_binding"), TEXT("module_node"),    TEXT("module"),         TEXT("MonolithNiagaraActions.cpp:7186") },
		{ TEXT("niagara"), TEXT("set_module_input_binding"), TEXT("input"),          TEXT("input_name"),     TEXT("MonolithNiagaraActions.cpp:7188") },
		{ TEXT("niagara"), TEXT("set_module_input_di"),      TEXT("module_node"),    TEXT("module_name"),    TEXT("MonolithNiagaraActions.cpp:7337") },
		{ TEXT("niagara"), TEXT("set_module_input_di"),      TEXT("module_node"),    TEXT("module"),         TEXT("MonolithNiagaraActions.cpp:7338") },
		{ TEXT("niagara"), TEXT("set_module_input_di"),      TEXT("input"),          TEXT("input_name"),     TEXT("MonolithNiagaraActions.cpp:7340") },
		{ TEXT("niagara"), TEXT("get_module_inputs"),        TEXT("module_node"),    TEXT("module_name"),    TEXT("MonolithNiagaraActions.cpp:5945") },
		{ TEXT("niagara"), TEXT("get_module_inputs"),        TEXT("module_node"),    TEXT("module"),         TEXT("MonolithNiagaraActions.cpp:5946") },
		{ TEXT("niagara"), TEXT("set_curve_value"),          TEXT("module_node"),    TEXT("module"),         TEXT("MonolithNiagaraActions.cpp:10331") },
		{ TEXT("niagara"), TEXT("set_curve_value"),          TEXT("module_node"),    TEXT("module_name"),    TEXT("MonolithNiagaraActions.cpp:10332") },
		{ TEXT("niagara"), TEXT("set_curve_value"),          TEXT("input"),          TEXT("input_name"),     TEXT("MonolithNiagaraActions.cpp:10334") },
		{ TEXT("niagara"), TEXT("set_static_switch_value"),  TEXT("module_node"),    TEXT("module_name"),    TEXT("MonolithNiagaraActions.cpp:12274") },
		{ TEXT("niagara"), TEXT("set_static_switch_value"),  TEXT("input"),          TEXT("input_name"),     TEXT("MonolithNiagaraActions.cpp:12276") },
		{ TEXT("niagara"), TEXT("get_static_switch_value"),  TEXT("module_node"),    TEXT("module_name"),    TEXT("MonolithNiagaraActions.cpp:21157") },

		// --- niagara: property name on the get/set property actions -----------------------
		{ TEXT("niagara"), TEXT("get_system_property"),      TEXT("property"),       TEXT("property_name"),  TEXT("MonolithNiagaraActions.cpp:12157") },
		{ TEXT("niagara"), TEXT("set_system_property"),      TEXT("property"),       TEXT("property_name"),  TEXT("MonolithNiagaraActions.cpp:12214") },
		{ TEXT("niagara"), TEXT("get_emitter_property"),     TEXT("property"),       TEXT("property_name"),  TEXT("MonolithNiagaraActions.cpp:18702") },
		{ TEXT("niagara"), TEXT("set_emitter_property"),     TEXT("property"),       TEXT("property_name"),  TEXT("MonolithNiagaraActions.cpp:5548") },
		{ TEXT("niagara"), TEXT("set_renderer_property"),    TEXT("property"),       TEXT("property_name"),  TEXT("MonolithNiagaraActions.cpp:10595") },

		// --- niagara: assorted -------------------------------------------------------------
		{ TEXT("niagara"), TEXT("set_renderer_mesh"),        TEXT("mesh"),           TEXT("mesh_path"),      TEXT("MonolithNiagaraActions.cpp:18298") },
		{ TEXT("niagara"), TEXT("add_renderer"),             TEXT("class"),          TEXT("renderer_class"), TEXT("MonolithNiagaraActions.cpp:10485") },
		{ TEXT("niagara"), TEXT("add_renderer"),             TEXT("class"),          TEXT("renderer_type"),  TEXT("MonolithNiagaraActions.cpp:10486") },
		{ TEXT("niagara"), TEXT("add_emitter"),              TEXT("emitter_asset"),  TEXT("emitter_path"),   TEXT("MonolithNiagaraActions.cpp:5318") },
		{ TEXT("niagara"), TEXT("add_emitter"),              TEXT("emitter_asset"),  TEXT("template"),       TEXT("MonolithNiagaraActions.cpp:5319") },
		{ TEXT("niagara"), TEXT("add_emitter"),              TEXT("emitter_asset"),  TEXT("template_path"),  TEXT("MonolithNiagaraActions.cpp:5320") },
		{ TEXT("niagara"), TEXT("duplicate_emitter"),        TEXT("source_emitter"), TEXT("emitter"),        TEXT("MonolithNiagaraActions.cpp:5466") },

		// --- ui: the schema DESCRIPTION already promised "(alias: asset_path)" -------------
		{ TEXT("ui"),      TEXT("add_widget_variable"),      TEXT("wbp_path"),       TEXT("asset_path"),     TEXT("MonolithUIRegistryActions.cpp:49") },
		{ TEXT("ui"),      TEXT("rename_widget"),            TEXT("wbp_path"),       TEXT("asset_path"),     TEXT("MonolithUIActions.cpp:1022") },

		// --- project: the handler's own error text already advertised the alternate --------
		{ TEXT("project"), TEXT("find_references"),          TEXT("asset_path"),     TEXT("package_path"),   TEXT("ProjectFindReferencesAction.cpp:11") },
		{ TEXT("project"), TEXT("get_asset_details"),        TEXT("asset_path"),     TEXT("package_path"),   TEXT("ProjectGetAssetDetailsAction.cpp:11") },
		{ TEXT("project"), TEXT("get_saved_asset_state"),    TEXT("asset_path"),     TEXT("package_path"),   TEXT("ProjectGetSavedAssetStateAction.cpp:13") },
		{ TEXT("project"), TEXT("find_by_type"),             TEXT("asset_type"),     TEXT("asset_class"),    TEXT("ProjectFindByTypeAction.cpp:11") },
	};

	static const TCHAR* SentinelValue = TEXT("MONOLITH_ALIAS_SENTINEL");
	static const TCHAR* MissingParamError = TEXT("Missing required param(s)");

	/** A system path that cannot exist, so nothing in these tests depends on content. */
	static const TCHAR* AbsentSystemPath = TEXT("/Game/FX/_Probes/NS_Test_AliasProbe_DoesNotExist");

	/**
	 * The HANDLER's own rejection of that path (HandleSetModuleInputValue, via LoadSystem,
	 * MonolithNiagaraActions.cpp:6933). It is reachable ONLY after validation accepted the op
	 * and the batch dispatched it — which is what makes it usable as a positive assertion with
	 * no asset in play.
	 */
	static const TCHAR* HandlerReachedError = TEXT("Failed to load system");

	/** LoadSystem's log line for the same miss (MonolithNiagaraActions.cpp:3180). */
	static const TCHAR* HandlerLoadLogError = TEXT("Failed to load Niagara system");

	/** batch_execute resolving its deliberately-absent batch-level asset (MonolithNiagaraActions.cpp:1188). */
	static const TCHAR* NoBatchAssetLogError = TEXT("NA_GetAssetPath: no 'asset_path'");

	/** What one sub-op of a one-op batch came back as. Every field is asserted on. */
	struct FBatchOpOutcome
	{
		bool bEnvelope = false;   // batch_execute returned a result object at all
		bool bResult0 = false;    // ...carrying results[0] as an object
		bool bOpSuccess = false;
		FString Error;
	};
}

// ---------------------------------------------------------------------------
// Test 1: every declared alias in the sweep actually rewrites. One assertion group per
// mismatch found — 35 alias rows covering the 27 (action, canonical) mismatches.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithParamAliasDeclaredTest,
	"Monolith.ParamAliasDeclaration.EveryHandlerFallbackSpellingIsDeclared",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamAliasDeclaredTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithParamAliasDeclarationTestDetail;

	int32 Checked = 0, Skipped = 0;

	for (const FAliasCase& C : Cases)
	{
		if (!FMonolithToolRegistry::Get().HasAction(C.Namespace, C.Action))
		{
			// Owning module not loaded in this run — skip rather than fake a pass.
			AddInfo(FString::Printf(TEXT("SKIP %s.%s (not registered in this run)"), C.Namespace, C.Action));
			++Skipped;
			continue;
		}

		// Supply ONLY the alternate spelling.
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(C.Alias, SentinelValue);

		FMonolithToolRegistry::Get().ValidateActionParams(C.Namespace, C.Action, P);

		const FString Where = FString::Printf(
			TEXT("%s.%s: '%s' must be a declared alias of '%s' (handler accepts it at %s)"),
			C.Namespace, C.Action, C.Alias, C.Canonical, C.HandlerEvidence);

		FString Rewritten;
		const bool bHasCanonical = P->TryGetStringField(C.Canonical, Rewritten);

		TestTrue(*FString::Printf(TEXT("%s — canonical key present after ApplyAliases"), *Where),
			bHasCanonical);
		if (bHasCanonical)
		{
			TestEqual(*FString::Printf(TEXT("%s — value survived the rewrite"), *Where),
				Rewritten, FString(SentinelValue));
		}
		TestFalse(*FString::Printf(TEXT("%s — alternate spelling was consumed"), *Where),
			P->HasField(C.Alias));

		++Checked;
	}

	AddInfo(FString::Printf(TEXT("alias rows checked: %d, skipped: %d, total: %d"),
		Checked, Skipped, (int32)UE_ARRAY_COUNT(Cases)));
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: supplying an alias must NOT be reported as a missing required param. This is the
// exact regression: the message the caller actually saw was
// "Missing required param(s): [module_node]" while supplying `module`.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithParamAliasNotReportedMissingTest,
	"Monolith.ParamAliasDeclaration.AliasIsNotReportedAsMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamAliasNotReportedMissingTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithParamAliasDeclarationTestDetail;

	if (!FMonolithToolRegistry::Get().HasAction(TEXT("niagara"), TEXT("set_module_input_value")))
	{
		AddInfo(TEXT("niagara not registered — skipping."));
		return true;
	}

	// Every accepted spelling of the module identity, fully populated otherwise.
	const TCHAR* ModuleSpellings[] = { TEXT("module_node"), TEXT("module_name"), TEXT("module") };
	const TCHAR* InputSpellings[]  = { TEXT("input"), TEXT("input_name") };

	for (const TCHAR* ModuleKey : ModuleSpellings)
	{
		for (const TCHAR* InputKey : InputSpellings)
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), TEXT("/Game/FX/_Probes/NS_Test_AliasProbe"));
			P->SetStringField(TEXT("emitter"), TEXT("ProbeEmitter"));
			P->SetStringField(ModuleKey, TEXT("InitializeParticle"));
			P->SetStringField(InputKey, TEXT("Lifetime"));
			P->SetStringField(TEXT("value"), TEXT("2.75"));

			const FMonolithActionResult V = FMonolithToolRegistry::Get()
				.ValidateActionParams(TEXT("niagara"), TEXT("set_module_input_value"), P);

			TestTrue(*FString::Printf(
					TEXT("set_module_input_value must validate with module spelled '%s' and input spelled '%s'"),
					ModuleKey, InputKey),
				V.bSuccess);
			TestFalse(*FString::Printf(
					TEXT("'%s'/'%s' must not be reported as a missing required param"), ModuleKey, InputKey),
				V.ErrorMessage.Contains(MissingParamError));
		}
	}

	// ...and the check still BITES when the param is genuinely absent. Widening the accepted
	// spellings must not have widened it into accepting nothing at all.
	TSharedPtr<FJsonObject> Missing = MakeShared<FJsonObject>();
	Missing->SetStringField(TEXT("asset_path"), TEXT("/Game/FX/_Probes/NS_Test_AliasProbe"));
	Missing->SetStringField(TEXT("emitter"), TEXT("ProbeEmitter"));
	Missing->SetStringField(TEXT("input"), TEXT("Lifetime"));
	Missing->SetStringField(TEXT("value"), TEXT("2.75"));      // no module spelling at all

	const FMonolithActionResult MissingResult = FMonolithToolRegistry::Get()
		.ValidateActionParams(TEXT("niagara"), TEXT("set_module_input_value"), Missing);

	TestFalse(TEXT("a genuinely absent module identity must still FAIL"), MissingResult.bSuccess);
	TestTrue(TEXT("...with the gap #72 missing-param error"),
		MissingResult.ErrorMessage.Contains(MissingParamError));
	TestTrue(TEXT("...naming the canonical param"),
		MissingResult.ErrorMessage.Contains(TEXT("module_node")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: END TO END through the real batch_execute, because that is the path that
// regressed. `set_module_input` is one of the four batch-only op spellings, mapped to the
// set_module_input_value schema for validation.
//
// ⚠️ THE FIRST VERSION OF THIS TEST COULD NOT FAIL, AND THIS IS THE REWRITE.
// It put the absent asset at the BATCH level. batch_execute refuses such a batch up front
// (MonolithNiagaraActions.cpp:11031) and returns an Error with NO envelope, so every call came
// back as the sentinel string "<no envelope>", and every positive assertion was of the form
// TestFalse(sentinel.Contains("Missing required param(s)")) — true on ANY build, including one
// with all 35 alias declarations deleted. Only the negative control could fail, and it did.
// A test that reports success without checking anything is the same defect class it was
// written to guard against.
//
// The rewrite makes DISPATCH ACTUALLY HAPPEN while staying asset-free:
//   * the batch names NO batch-level asset, so there is nothing for batch_execute to refuse
//     up front (it logs NA_GetAssetPath's Error instead — declared expected below);
//   * each op names its OWN asset_path, which satisfies the schema's required path and is
//     then handed to the handler, which fails at LoadSystem with a fixed, known string.
//
// So each spelling now has a POSITIVE subject: the op must come back carrying the HANDLER's
// error, which is unreachable unless validation accepted that spelling. Every vacuity route
// is now itself an assertion — a missing envelope, a missing results[0] and an empty error
// are failures, not passes — and the expected-error count on the handler's log line is a
// second, independent guard: if a spelling stops dispatching, that log fires fewer than six
// times and the test fails on that alone.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBatchSetModuleInputSpellingsTest,
	"Monolith.ParamAliasDeclaration.BatchSetModuleInputAcceptsEverySpelling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBatchSetModuleInputSpellingsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithParamAliasDeclarationTestDetail;

	if (!FMonolithToolRegistry::Get().HasAction(TEXT("niagara"), TEXT("batch_execute")))
	{
		AddInfo(TEXT("niagara.batch_execute not registered — skipping."));
		return true;
	}

	// Armed AFTER the skip guard, deliberately: an expected error that never occurs is itself
	// a failure, so these must not be declared on a run where nothing executes.
	//
	// (1) The batch names no default asset — that is the design of this test, and batch_execute
	//     says so at Error level. `0` occurrences means "one or more": the exact count is
	//     incidental noise here.
	AddExpectedErrorPlain(NoBatchAssetLogError, EAutomationExpectedErrorFlags::Contains, 0);
	// (2) EXACTLY the six ops below must reach the handler and fail at LoadSystem. This count
	//     is load-bearing, not noise suppression: it fails if a spelling stops dispatching.
	AddExpectedErrorPlain(HandlerLoadLogError, EAutomationExpectedErrorFlags::Contains, 6);

	auto RunOneOpBatch = [](const TSharedPtr<FJsonObject>& Op) -> FBatchOpOutcome
	{
		FBatchOpOutcome Out;

		TArray<TSharedPtr<FJsonValue>> Ops;
		Ops.Add(MakeShared<FJsonValueObject>(Op));

		// NO batch-level asset: an absent one here is refused before any op runs, which is
		// exactly how the previous version of this test made itself unable to fail.
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetArrayField(TEXT("operations"), Ops);

		const FMonolithActionResult R = FMonolithToolRegistry::Get()
			.ExecuteAction(TEXT("niagara"), TEXT("batch_execute"), P);

		if (!R.Result.IsValid()) return Out;
		Out.bEnvelope = true;

		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!R.Result->TryGetArrayField(TEXT("results"), Arr) || !Arr || !Arr->IsValidIndex(0))
		{
			return Out;
		}
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!(*Arr)[0]->TryGetObject(Obj) || !Obj) return Out;
		Out.bResult0 = true;

		(*Obj)->TryGetBoolField(TEXT("success"), Out.bOpSuccess);
		(*Obj)->TryGetStringField(TEXT("error"), Out.Error);
		return Out;
	};

	// The anti-vacuity guard. The old version funnelled all three of these conditions into a
	// sentinel STRING which then satisfied a "does not contain" assertion; here each one is an
	// assertion in its own right, so a build where nothing executes FAILS instead of passing.
	auto CheckTheOpWasActuallyExercised = [this](const FBatchOpOutcome& O, const FString& Where)
	{
		TestTrue(*(Where + TEXT(" — batch_execute returned an envelope")), O.bEnvelope);
		TestTrue(*(Where + TEXT(" — envelope carried results[0]")), O.bResult0);
		TestFalse(*(Where + TEXT(" — results[0] carried an error to assert on")), O.Error.IsEmpty());
	};

	const TCHAR* ModuleSpellings[] = { TEXT("module_node"), TEXT("module_name"), TEXT("module") };
	const TCHAR* InputSpellings[]  = { TEXT("input"), TEXT("input_name") };

	for (const TCHAR* ModuleKey : ModuleSpellings)
	{
		for (const TCHAR* InputKey : InputSpellings)
		{
			TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
			Op->SetStringField(TEXT("op"), TEXT("set_module_input"));   // batch-only spelling
			Op->SetStringField(TEXT("asset_path"), AbsentSystemPath);   // the op names its OWN asset
			Op->SetStringField(TEXT("emitter"), TEXT("ProbeEmitter"));
			Op->SetStringField(ModuleKey, TEXT("InitializeParticle"));
			Op->SetStringField(InputKey, TEXT("Lifetime"));
			Op->SetStringField(TEXT("value"), TEXT("2.75"));

			const FBatchOpOutcome O = RunOneOpBatch(Op);
			const FString Where = FString::Printf(
				TEXT("batch set_module_input with '%s'/'%s'"), ModuleKey, InputKey);

			CheckTheOpWasActuallyExercised(O, Where);

			TestFalse(*(Where + FString::Printf(
					TEXT(" — must get PAST validation (error was: %s)"), *O.Error)),
				O.Error.Contains(MissingParamError));

			// THE POSITIVE HALF, and the whole point of the rewrite: the handler's own error
			// cannot be produced unless validation accepted this spelling and the op dispatched.
			// If the alias declaration for '%s' is removed, this string becomes
			// "Missing required param(s): [module_node]" and BOTH assertions fail.
			TestTrue(*(Where + FString::Printf(
					TEXT(" — must have REACHED the handler (error was: %s)"), *O.Error)),
				O.Error.Contains(HandlerReachedError));
		}
	}

	// The negative control: omit the module identity entirely — everything else present — and
	// the dispatch layer must still refuse, or this test would pass on a build where validation
	// was simply removed. It also pins NA_GetBatchOpSchemaNames: naming `module_node` proves the
	// batch-only op spelling `set_module_input` was validated against set_module_input_value's
	// schema. If that mapping is lost, validation fails open, the op dispatches, and the error
	// becomes the handler's instead.
	{
		TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("op"), TEXT("set_module_input"));
		Op->SetStringField(TEXT("asset_path"), AbsentSystemPath);
		Op->SetStringField(TEXT("emitter"), TEXT("ProbeEmitter"));
		Op->SetStringField(TEXT("input"), TEXT("Lifetime"));
		Op->SetStringField(TEXT("value"), TEXT("2.75"));      // no module spelling at all

		const FBatchOpOutcome O = RunOneOpBatch(Op);
		const FString Where = TEXT("batch set_module_input with NO module identity");

		CheckTheOpWasActuallyExercised(O, Where);

		TestFalse(*(Where + TEXT(" — the op must FAIL")), O.bOpSuccess);
		TestTrue(*(Where + FString::Printf(
				TEXT(" — must still be refused by the dispatch layer (error was: %s)"), *O.Error)),
			O.Error.Contains(MissingParamError));
		TestTrue(*(Where + TEXT(" — naming the canonical param, i.e. validated against "
				"set_module_input_value's schema")),
			O.Error.Contains(TEXT("module_node")));
		TestFalse(*(Where + TEXT(" — and the handler must NEVER have been reached")),
			O.Error.Contains(HandlerReachedError));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
