// SPDX-License-Identifier: MIT
// GAP #72 — batch_execute sub-ops are validated by the registry's rules.
//
// batch_execute dispatches its sub-ops through a direct function-pointer table, never through
// FMonolithToolRegistry::ExecuteAction. Neither required-param validation nor declared-alias
// rewriting therefore ran for them, and the measured consequence was an error that blamed the
// asset for the caller's typo:
//
//     niagara.list_renderers { emitter_name: "X" }                  -> "Missing required param(s): [emitter]"
//     niagara.batch_execute  { ops: [ same sub-op ] }               -> "Emitter not found"
//
// The fix borrows the registry's VALIDATION without borrowing its DISPATCH. That separation is
// deliberate and these tests pin both halves of it:
//
//   * the validation seam (FMonolithToolRegistry::ValidateActionParams) applies the same rules
//     and emits the same text as ExecuteAction — DispatchLayerErrorMatchesExecuteAction is the
//     anti-drift test for that, since two copies of one rule is how this class of defect
//     started (recurring defect pattern #1);
//   * the batch ENVELOPE is untouched. Routing sub-ops through ExecuteAction was rejected
//     because it appends its own `warnings` array to successful results and every sub-op
//     carries the `op` key that no schema declares — every step of every batch would have
//     become `warned`. OneBadOpDoesNotAbortTheBatch pins the envelope and the not-atomic
//     contract that a validation rejection must not change.
//
// The two end-to-end tests drive the REAL niagara.batch_execute and need no asset and no
// transaction: they use a batch with no asset_path and read-only ops, so a rejected op is
// rejected before any load and `list_systems` (which declares no required params) is the
// genuine success alongside it. They self-skip if the Niagara module is not loaded.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithBatchParamValidationTestDetail
{
	static const TCHAR* Ns = TEXT("monolith_test_batch_validation");

	/** The handler-side message the defect produced. It must not survive. */
	static const TCHAR* OldMisleadingError = TEXT("Emitter not found");

	/** The dispatch-layer fact that replaces it. */
	static const TCHAR* DispatchLayerError = TEXT("Missing required param(s)");

	/**
	 * INTENTIONAL production logging, not a failure. batch_execute resolves a batch-level
	 * default asset before it loops the ops (MonolithNiagaraActions.cpp:11007) and the batches
	 * below deliberately name none, so NA_GetAssetPath logs at Error level
	 * (MonolithNiagaraActions.cpp:1188). Automation captures Error-level log lines and fails the
	 * test on them, so every test that drives an asset-free batch must declare this expected.
	 */
	static const TCHAR* NoBatchAssetLogError = TEXT("NA_GetAssetPath: no 'asset_path'");

	/** Schema shaped like the 110 Niagara actions: required aliased path + a required name. */
	static TSharedPtr<FJsonObject> AliasedSchema()
	{
		return FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path."), { TEXT("system_path") })
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Build();
	}

	struct FCapturingHandler
	{
		bool bSawCall = false;
		FString SawAssetPath;

		FMonolithActionResult operator()(const TSharedPtr<FJsonObject>& Params)
		{
			bSawCall = true;
			if (Params.IsValid())
			{
				Params->TryGetStringField(TEXT("asset_path"), SawAssetPath);
			}
			TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetBoolField(TEXT("ok"), true);
			return FMonolithActionResult::Success(R);
		}
	};

	static TSharedRef<FCapturingHandler> Register(const FString& Action, const TSharedPtr<FJsonObject>& Schema)
	{
		TSharedRef<FCapturingHandler> Cap = MakeShared<FCapturingHandler>();
		FMonolithToolRegistry::Get().RegisterAction(Ns, Action, TEXT("Test."),
			FMonolithActionHandler::CreateLambda(
				[Cap](const TSharedPtr<FJsonObject>& P) { return (*Cap)(P); }),
			Schema);
		return Cap;
	}

	static TSharedPtr<FJsonValue> MakeOp(const FString& OpName, const TMap<FString, FString>& Fields)
	{
		TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("op"), OpName);
		for (const TPair<FString, FString>& F : Fields)
		{
			Op->SetStringField(F.Key, F.Value);
		}
		return MakeShared<FJsonValueObject>(Op);
	}

	/** A cheap, always-available success op: list_systems declares no required params. */
	static TSharedPtr<FJsonValue> MakeCheapSuccessOp()
	{
		TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("op"), TEXT("list_systems"));
		Op->SetStringField(TEXT("path"), TEXT("/Game/FX/_Probes"));
		Op->SetNumberField(TEXT("limit"), 1);
		return MakeShared<FJsonValueObject>(Op);
	}

	static bool NiagaraBatchAvailable()
	{
		return FMonolithToolRegistry::Get().HasAction(TEXT("niagara"), TEXT("batch_execute"));
	}

	/** Runs the real niagara.batch_execute with no batch-level asset. */
	static FMonolithActionResult RunBatch(const TArray<TSharedPtr<FJsonValue>>& Ops)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetArrayField(TEXT("operations"), Ops);
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("niagara"), TEXT("batch_execute"), P);
	}

	static const TSharedPtr<FJsonObject>* ResultAt(const FMonolithActionResult& R, int32 Index)
	{
		if (!R.Result.IsValid()) return nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!R.Result->TryGetArrayField(TEXT("results"), Arr) || !Arr) return nullptr;
		if (!Arr->IsValidIndex(Index)) return nullptr;
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!(*Arr)[Index]->TryGetObject(Obj)) return nullptr;
		return Obj;
	}
}

// ---------------------------------------------------------------------------
// Test 1: GAP #72 ITSELF, end to end through the real batch_execute.
// A sub-op with a misspelt param must fail with the DISPATCH-LAYER error, and the old
// misleading HANDLER error must be gone.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBatchSubOpMissingParamTest,
	"Monolith.BatchParamValidation.SubOpMissingParamIsDispatchError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBatchSubOpMissingParamTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithBatchParamValidationTestDetail;

	if (!NiagaraBatchAvailable())
	{
		AddInfo(TEXT("niagara.batch_execute not registered (MonolithNiagara not loaded) — skipping."));
		return true;
	}

	// Declared AFTER the skip guard: an expected error that never occurs fails the test, so it
	// must not be armed on a run where nothing executes. `0` occurrences = one or more.
	AddExpectedErrorPlain(NoBatchAssetLogError, EAutomationExpectedErrorFlags::Contains, 0);

	// The measured reproduction: `emitter_name` instead of `emitter`.
	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(MakeOp(TEXT("list_renderers"), { { TEXT("emitter_name"), TEXT("Emitter0") } }));

	const FMonolithActionResult R = RunBatch(Ops);

	// The batch CALL still returns a normal envelope — a bad sub-op is a per-op failure.
	TestTrue(TEXT("batch_execute itself still returns a result envelope"), R.bSuccess);

	const TSharedPtr<FJsonObject>* Op0 = ResultAt(R, 0);
	if (!TestTrue(TEXT("results[0] present"), Op0 != nullptr && Op0->IsValid()))
	{
		return false;
	}

	bool bOpSuccess = true;
	(*Op0)->TryGetBoolField(TEXT("success"), bOpSuccess);
	TestFalse(TEXT("the sub-op must FAIL"), bOpSuccess);

	FString OpError;
	(*Op0)->TryGetStringField(TEXT("error"), OpError);

	TestTrue(TEXT("sub-op error is now the dispatch-layer error"),
		OpError.Contains(DispatchLayerError));
	TestTrue(TEXT("sub-op error names the param the caller got wrong"),
		OpError.Contains(TEXT("emitter")));
	// The point of the gap: the handler used to blame the asset for a caller-side typo.
	TestFalse(TEXT("the old misleading handler error must NOT appear"),
		OpError.Contains(OldMisleadingError));
	TestTrue(TEXT("sub-op error states nothing happened"),
		OpError.Contains(TEXT("NOTHING WAS READ OR CHANGED")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: one bad sub-op does not change the fate of the others, and the envelope shape
// is unchanged. This is the contract the fix had to preserve: batch is NOT atomic, a
// failed step does not abort the batch, and a validation rejection is just another
// per-op failure.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBatchOneBadOpDoesNotAbortTest,
	"Monolith.BatchParamValidation.OneBadOpDoesNotAbortTheBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBatchOneBadOpDoesNotAbortTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithBatchParamValidationTestDetail;

	if (!NiagaraBatchAvailable())
	{
		AddInfo(TEXT("niagara.batch_execute not registered (MonolithNiagara not loaded) — skipping."));
		return true;
	}

	// Same intentional log line as test 1 — this batch also names no default asset.
	AddExpectedErrorPlain(NoBatchAssetLogError, EAutomationExpectedErrorFlags::Contains, 0);

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(MakeCheapSuccessOp());                                                      // 0: succeeds
	Ops.Add(MakeOp(TEXT("list_renderers"), { { TEXT("emitter_name"), TEXT("E") } }));    // 1: rejected
	Ops.Add(MakeCheapSuccessOp());                                                      // 2: succeeds

	const FMonolithActionResult R = RunBatch(Ops);

	if (!TestTrue(TEXT("batch returned a result"), R.bSuccess && R.Result.IsValid()))
	{
		return false;
	}

	// Envelope shape — every field a caller reads must still be there.
	double Total = 0, Succeeded = 0, Failed = 0;
	TestTrue(TEXT("envelope has 'total'"),     R.Result->TryGetNumberField(TEXT("total"), Total));
	TestTrue(TEXT("envelope has 'succeeded'"), R.Result->TryGetNumberField(TEXT("succeeded"), Succeeded));
	TestTrue(TEXT("envelope has 'failed'"),    R.Result->TryGetNumberField(TEXT("failed"), Failed));
	TestTrue(TEXT("envelope has 'contract'"),  R.Result->HasField(TEXT("contract")));
	TestTrue(TEXT("envelope has 'results'"),   R.Result->HasField(TEXT("results")));

	TestEqual(TEXT("all three ops were attempted — the batch did not abort at the failure"),
		(int32)Total, 3);
	TestEqual(TEXT("the two good ops still ran"),  (int32)Succeeded, 2);
	TestEqual(TEXT("exactly the bad op failed"),   (int32)Failed, 1);

	// Per-op fates, in order.
	const TSharedPtr<FJsonObject>* Op0 = ResultAt(R, 0);
	const TSharedPtr<FJsonObject>* Op1 = ResultAt(R, 1);
	const TSharedPtr<FJsonObject>* Op2 = ResultAt(R, 2);
	if (!TestTrue(TEXT("all three per-op results present"),
		Op0 && Op1 && Op2 && Op0->IsValid() && Op1->IsValid() && Op2->IsValid()))
	{
		return false;
	}

	bool b0 = false, b1 = true, b2 = false;
	(*Op0)->TryGetBoolField(TEXT("success"), b0);
	(*Op1)->TryGetBoolField(TEXT("success"), b1);
	(*Op2)->TryGetBoolField(TEXT("success"), b2);

	TestTrue(TEXT("op before the bad one succeeded"), b0);
	TestFalse(TEXT("the bad op failed"), b1);
	TestTrue(TEXT("op AFTER the bad one still ran and succeeded"), b2);

	// A rejected op must not manufacture a warning. Routing sub-ops through ExecuteAction
	// would have marked every step warned (the `op` key is an unknown param on every schema);
	// borrowing only its validation must not. Asserted on the rejected op specifically rather
	// than on the batch total, so an unrelated payload warning from a good op cannot mask or
	// fake this.
	TestFalse(TEXT("a validation rejection does not mark the step 'warned'"),
		(*Op1)->HasField(TEXT("warned")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: the alias still works for a sub-op. batch_execute injects the batch-level asset
// under the legacy spelling `system_path`; validation must accept it and hand the handler
// the canonical `asset_path`, in place.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBatchSubOpAliasAcceptedTest,
	"Monolith.BatchParamValidation.SubOpAliasAccepted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBatchSubOpAliasAcceptedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithBatchParamValidationTestDetail;

	const FString Ac = TEXT("aliased_op");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };
	Register(Ac, AliasedSchema());

	// Exactly what the batch builds: the op's own fields plus the injected legacy spelling.
	TSharedPtr<FJsonObject> SubParams = MakeShared<FJsonObject>();
	SubParams->SetStringField(TEXT("op"), Ac);                 // batch copies the op key too
	SubParams->SetStringField(TEXT("emitter"), TEXT("Emitter0"));
	SubParams->SetStringField(TEXT("system_path"), TEXT("/Game/FX/_Probes/NS_Test_Thing"));

	const FMonolithActionResult V =
		FMonolithToolRegistry::Get().ValidateActionParams(Ns, Ac, SubParams);

	TestTrue(TEXT("an injected system_path must still validate"), V.bSuccess);

	// Normalised IN PLACE, so the handler sees what a top-level call would give it.
	FString Canonical;
	TestTrue(TEXT("alias was rewritten to the canonical key"),
		SubParams->TryGetStringField(TEXT("asset_path"), Canonical));
	TestEqual(TEXT("value survived the rewrite"),
		Canonical, FString(TEXT("/Game/FX/_Probes/NS_Test_Thing")));
	TestFalse(TEXT("legacy spelling was consumed"), SubParams->HasField(TEXT("system_path")));

	// The `op` key is an unknown param for every schema. Validation must IGNORE it: treating
	// it as an error, or warning about it into the result, is what disqualified full routing.
	TestTrue(TEXT("the batch's own 'op' key must not trouble validation"),
		SubParams->HasField(TEXT("op")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: validation fails OPEN on an action the registry does not know.
// batch_execute's table carries four batch-only op spellings (set_module_input,
// set_module_binding, add_user_param, remove_user_param) that are not registered actions.
// They are mapped to their canonical action for validation; if that map ever misses one,
// the op must keep working unvalidated rather than start reporting "unknown action".
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBatchUnknownActionFailsOpenTest,
	"Monolith.BatchParamValidation.UnknownActionFailsOpen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBatchUnknownActionFailsOpenTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithBatchParamValidationTestDetail;

	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("anything"), TEXT("x"));

	const FMonolithActionResult V = FMonolithToolRegistry::Get().ValidateActionParams(
		TEXT("monolith_test_no_such_namespace"), TEXT("no_such_action"), P);

	TestTrue(TEXT("unknown action must validate as OK, not be rejected here"), V.bSuccess);

	// And the four real batch-only spellings: still not registered actions, still mapped.
	// If one of these ever becomes a registered action, the map entry is redundant — that is
	// a maintenance signal, not a failure, so this only asserts the canonical targets exist.
	if (FMonolithToolRegistry::Get().HasAction(TEXT("niagara"), TEXT("batch_execute")))
	{
		TestTrue(TEXT("canonical target set_module_input_value is registered"),
			FMonolithToolRegistry::Get().HasAction(TEXT("niagara"), TEXT("set_module_input_value")));
		TestTrue(TEXT("canonical target set_module_input_binding is registered"),
			FMonolithToolRegistry::Get().HasAction(TEXT("niagara"), TEXT("set_module_input_binding")));
		TestTrue(TEXT("canonical target add_user_parameter is registered"),
			FMonolithToolRegistry::Get().HasAction(TEXT("niagara"), TEXT("add_user_parameter")));
		TestTrue(TEXT("canonical target remove_user_parameter is registered"),
			FMonolithToolRegistry::Get().HasAction(TEXT("niagara"), TEXT("remove_user_parameter")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: ANTI-DRIFT. The sub-op path and the top-level path must produce the SAME error
// for the same mistake — that is the whole claim of gap #72, and the only thing keeping it
// true is that both call one implementation. If someone re-inlines either copy, this fails.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBatchDispatchErrorMatchesExecuteActionTest,
	"Monolith.BatchParamValidation.DispatchLayerErrorMatchesExecuteAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBatchDispatchErrorMatchesExecuteActionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithBatchParamValidationTestDetail;

	const FString Ac = TEXT("drift_check");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };
	TSharedRef<FCapturingHandler> Cap = Register(Ac, AliasedSchema());

	auto MakeParams = []()
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), TEXT("/Game/FX/_Probes/NS_Test_Thing"));
		P->SetStringField(TEXT("emitter_name"), TEXT("Emitter0"));   // the typo
		return P;
	};

	// Top-level path.
	const FMonolithActionResult Direct =
		FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, MakeParams());

	// Sub-op path.
	const FMonolithActionResult Sub =
		FMonolithToolRegistry::Get().ValidateActionParams(Ns, Ac, MakeParams());

	TestFalse(TEXT("direct call rejects the typo"), Direct.bSuccess);
	TestFalse(TEXT("sub-op path rejects the typo identically"), Sub.bSuccess);
	TestEqual(TEXT("both paths must emit the SAME message — one rule, one wording"),
		Sub.ErrorMessage, Direct.ErrorMessage);
	TestFalse(TEXT("handler was never reached by either path"), Cap->bSawCall);

	// And a well-formed call is accepted by both.
	TSharedPtr<FJsonObject> Good = MakeShared<FJsonObject>();
	Good->SetStringField(TEXT("asset_path"), TEXT("/Game/FX/_Probes/NS_Test_Thing"));
	Good->SetStringField(TEXT("emitter"), TEXT("Emitter0"));
	TestTrue(TEXT("a valid sub-op still validates"),
		FMonolithToolRegistry::Get().ValidateActionParams(Ns, Ac, Good).bSuccess);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
