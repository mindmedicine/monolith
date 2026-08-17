// SPDX-License-Identifier: MIT
// GAP #110 / #111 — the warning channel: one structured member, merged on success,
// carried in the message on refusal.
//
// The defect these pin, stated as behaviour rather than as code:
//
//   1. There was NO way for a handler to attach a warning. FMonolithActionResult had
//      bSuccess / Result / ErrorMessage / ErrorCode / ErrorData and nothing else, so all 168
//      emission sites hand-rolled the channel and 58 of them landed on a singular `warning`
//      STRING that no merge reads and no documented reader looks for (gap #88).
//
//   2. A REFUSAL DESTROYED EVERY WARNING ON THE CALL. The dispatch layer computes its
//      path-rewrite and unknown-param warnings BEFORE the handler runs, then attached them
//      inside `if (ActionResult.bSuccess && ActionResult.Result.IsValid())`. One `if`, and a
//      call that both misspelled a param and failed was told only that it failed. The
//      measured symptom that started this: niagara.add_user_parameter with an unknown TYPE
//      and an unparseable DEFAULT refuses on the default and never says the type was also
//      unknown — nor that the literal was validated against `float` rather than against the
//      type the caller named.
//
// Tests follow the file-local idiom of MonolithParamKindRewriteTest.cpp: register a throwaway
// action in a private namespace, dispatch through FMonolithToolRegistry::ExecuteAction, inspect
// the result. Nothing here loads an asset or touches the editor, so nothing here can pass by
// accident on an unrelated build.
//
// WHAT EACH TEST WOULD CATCH is stated above each test. Tests 1-6 FAIL on the pre-change
// build (1-3 and 5 do not even compile there, since the member they use did not exist; 4 and 6
// fail on behaviour). Test 7 is a CHARACTERISATION test and passes on both builds — it is
// labelled as such where it lives, and it exists to MEASURE the `warnings`-as-a-number
// destruction that the audit could only predict.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithWarningChannelTestDetail
{
	static const TCHAR* Ns = TEXT("monolith_test_warning_channel");

	/** Read the response's warnings[] as strings. Empty when the key is absent or not an array. */
	static TArray<FString> GetWarnings(const FMonolithActionResult& R)
	{
		TArray<FString> Out;
		if (!R.Result.IsValid()) return Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!R.Result->TryGetArrayField(TEXT("warnings"), Arr) || !Arr) return Out;
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			FString S;
			if (V.IsValid() && V->TryGetString(S)) { Out.Add(S); }
		}
		return Out;
	}

	static bool AnyContains(const TArray<FString>& Warnings, const TCHAR* Needle)
	{
		for (const FString& W : Warnings)
		{
			if (W.Contains(Needle)) return true;
		}
		return false;
	}

	/**
	 * Register a lambda handler under the test namespace with the given schema.
	 * Templated on the callable rather than taking a TFunction so the test file depends on
	 * nothing beyond what the registry header already pulls in.
	 */
	template <typename FuncT>
	static void Register(const FString& Action, const TSharedPtr<FJsonObject>& Schema, FuncT Body)
	{
		FMonolithToolRegistry::Get().RegisterAction(Ns, Action, TEXT("Test."),
			FMonolithActionHandler::CreateLambda(Body), Schema);
	}

	/** A schema with one AssetPath-tagged param, so a backslash produces a dispatch warning. */
	static TSharedPtr<FJsonObject> AssetPathSchema()
	{
		return FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Tagged AssetPath param under test."))
			.Build();
	}

	/** A schema with one ordinary param, so any OTHER key trips the K3 unknown-param warning. */
	static TSharedPtr<FJsonObject> OneKnownParamSchema()
	{
		return FParamSchemaBuilder()
			.Optional(TEXT("known"), TEXT("string"), TEXT("The only declared param."))
			.Build();
	}
}

// ---------------------------------------------------------------------------
// Test 1: WithWarning is additive, fluent, and ignores empty strings;
//         FormatWarningBlock says nothing when there is nothing to say.
//
// CATCHES: a WithWarning that ASSIGNS instead of appending (i.e. reproduces the singular-key
// clobber in a new place); one that stores empty strings, which would put a blank bullet on
// every response built with a conditionally-empty string; and a FormatWarningBlock that emits
// a "Warnings (0)" header for an empty list, which would append noise to every error in the
// server. The fluent chaining is enforced by the fact that this test compiles.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWarningChannelStructTest,
	"Monolith.WarningChannel.WithWarningAccumulates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWarningChannelStructTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithActionResult R = FMonolithActionResult::Success(MakeShared<FJsonObject>())
		.WithWarning(TEXT("first"))
		.WithWarning(TEXT(""))          // ignored
		.WithWarning(TEXT("second"));

	TestEqual(TEXT("two warnings retained, empty one dropped"), R.Warnings.Num(), 2);
	if (R.Warnings.Num() == 2)
	{
		TestEqual(TEXT("order preserved (first)"), R.Warnings[0], FString(TEXT("first")));
		TestEqual(TEXT("order preserved (second)"), R.Warnings[1], FString(TEXT("second")));
	}

	// Duplicates are NOT collapsed — two cautions that read alike are usually about two things.
	FMonolithActionResult D = FMonolithActionResult::Error(TEXT("nope"))
		.WithWarning(TEXT("same"))
		.WithWarning(TEXT("same"));
	TestEqual(TEXT("duplicate warnings both retained"), D.Warnings.Num(), 2);

	// WithWarnings bulk form.
	FMonolithActionResult B = FMonolithActionResult::Success(MakeShared<FJsonObject>())
		.WithWarnings({ TEXT("a"), TEXT(""), TEXT("b") });
	TestEqual(TEXT("bulk attach skips empties"), B.Warnings.Num(), 2);

	// The refusal-path renderer stays silent when empty.
	TestTrue(TEXT("empty warning list renders to an empty block"),
		FMonolithActionResult::FormatWarningBlock({}).IsEmpty());
	TestTrue(TEXT("non-empty block names the warning"),
		FMonolithActionResult::FormatWarningBlock({ TEXT("WARN_TOKEN") }).Contains(TEXT("WARN_TOKEN")));

	return true;
}

// ---------------------------------------------------------------------------
// Test 2: a handler's structured warning reaches the response's warnings[] on success.
//
// CATCHES: the registry ignoring FMonolithActionResult::Warnings altogether — which is exactly
// what the pre-change merge did, because it only ever read warnings out of the Result JSON.
// A handler could attach a warning and the caller would never see it.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWarningChannelSuccessMergeTest,
	"Monolith.WarningChannel.HandlerWarningReachesWarningsArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWarningChannelSuccessMergeTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithWarningChannelTestDetail;

	const FString Ac = TEXT("success_with_warning");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	Register(Ac, OneKnownParamSchema(), [](const TSharedPtr<FJsonObject>&)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		return FMonolithActionResult::Success(Obj).WithWarning(TEXT("HANDLER_WARNING_TOKEN"));
	});

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("known"), TEXT("x"));

	FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params);

	TestTrue(TEXT("call succeeded"), R.bSuccess);
	const TArray<FString> Warnings = GetWarnings(R);
	TestEqual(TEXT("exactly the handler's one warning"), Warnings.Num(), 1);
	TestTrue(TEXT("handler warning present in warnings[]"),
		AnyContains(Warnings, TEXT("HANDLER_WARNING_TOKEN")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: the three warning sources UNION rather than overwrite —
//         the handler's own warnings[] JSON, the handler's structured warnings,
//         and the dispatch layer's unknown-param warning.
//
// CATCHES: the merge overwriting a handler-written warnings[] with the framework's own list
// (the SetArrayField at the end of the merge would do exactly that if the existing array were
// not read first); and structured warnings replacing rather than joining the JSON ones. Either
// mistake loses a real caution while leaving a plausible-looking warnings[] on the wire, which
// is the failure mode hardest to notice.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWarningChannelUnionTest,
	"Monolith.WarningChannel.AllSourcesUnion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWarningChannelUnionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithWarningChannelTestDetail;

	const FString Ac = TEXT("union_sources");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	Register(Ac, OneKnownParamSchema(), [](const TSharedPtr<FJsonObject>&)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Own;
		Own.Add(MakeShared<FJsonValueString>(TEXT("FROM_HANDLER_JSON")));
		Obj->SetArrayField(TEXT("warnings"), Own);
		return FMonolithActionResult::Success(Obj).WithWarning(TEXT("FROM_HANDLER_STRUCT"));
	});

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("known"), TEXT("x"));
	Params->SetStringField(TEXT("knownn"), TEXT("typo"));   // undeclared -> dispatch warning

	FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params);

	TestTrue(TEXT("call succeeded"), R.bSuccess);
	const TArray<FString> Warnings = GetWarnings(R);
	TestEqual(TEXT("all three warnings survive"), Warnings.Num(), 3);
	TestTrue(TEXT("handler's own JSON warning survives"), AnyContains(Warnings, TEXT("FROM_HANDLER_JSON")));
	TestTrue(TEXT("handler's structured warning survives"), AnyContains(Warnings, TEXT("FROM_HANDLER_STRUCT")));
	TestTrue(TEXT("dispatch unknown-param warning survives"), AnyContains(Warnings, TEXT("knownn")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: A REFUSAL CARRIES THE DISPATCH LAYER'S WARNING. This is the blast-radius fix.
//
// CATCHES: the `if (ActionResult.bSuccess && ...)` gate that used to wrap the entire warning
// collection. Before the change this test fails outright: the path-rewrite warning is computed
// before the handler runs and is then discarded because the handler refused, so the caller is
// told the call failed and never told their path had backslashes in it — the single most
// likely reason a path-addressed action fails.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWarningChannelRefusalDispatchTest,
	"Monolith.WarningChannel.RefusalCarriesDispatchWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWarningChannelRefusalDispatchTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithWarningChannelTestDetail;

	const FString Ac = TEXT("refuse_after_rewrite");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	Register(Ac, AssetPathSchema(), [](const TSharedPtr<FJsonObject>&)
	{
		return FMonolithActionResult::Error(TEXT("REFUSED_BY_HANDLER"));
	});

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Foo\\Bar"));

	FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params);

	TestFalse(TEXT("call refused"), R.bSuccess);
	TestTrue(TEXT("original error text preserved"),
		R.ErrorMessage.Contains(TEXT("REFUSED_BY_HANDLER")));
	TestTrue(TEXT("path-rewrite warning rides along on the refusal"),
		R.ErrorMessage.Contains(TEXT("Normalised backslashes")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: A REFUSAL CARRIES THE HANDLER'S OWN WARNING.
//
// CATCHES: a fix that only rescues the dispatch layer's warnings and still drops the
// handler's. That is precisely the add_user_parameter case the validator hit: the handler
// computes "the type you named was unknown, I substituted float" and then refuses on a
// separate ground, so the caller is never told which type their literal was validated against.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWarningChannelRefusalHandlerTest,
	"Monolith.WarningChannel.RefusalCarriesHandlerWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWarningChannelRefusalHandlerTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithWarningChannelTestDetail;

	const FString Ac = TEXT("refuse_with_warning");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	Register(Ac, OneKnownParamSchema(), [](const TSharedPtr<FJsonObject>&)
	{
		return FMonolithActionResult::Error(TEXT("REFUSED_ON_DEFAULT"))
			.WithWarning(TEXT("TYPE_FELL_BACK_TO_FLOAT"));
	});

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("known"), TEXT("x"));

	FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params);

	TestFalse(TEXT("call refused"), R.bSuccess);
	TestTrue(TEXT("refusal reason preserved"), R.ErrorMessage.Contains(TEXT("REFUSED_ON_DEFAULT")));
	TestTrue(TEXT("handler warning carried in the error message"),
		R.ErrorMessage.Contains(TEXT("TYPE_FELL_BACK_TO_FLOAT")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 6: BACKWARD COMPATIBILITY. A call with nothing to warn about is byte-for-byte
//         what it was before this framework existed.
//
// CATCHES: an unconditional append that decorates every error message in the server with an
// empty warnings header, and an unconditional SetArrayField that puts `"warnings": []` on
// every successful response. Both are the obvious way to write this change and both would be
// a visible regression across all ~830 actions.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWarningChannelNoOpTest,
	"Monolith.WarningChannel.CleanCallsAreUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWarningChannelNoOpTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithWarningChannelTestDetail;

	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	// (a) Clean success — no `warnings` key at all.
	Register(TEXT("clean_ok"), OneKnownParamSchema(), [](const TSharedPtr<FJsonObject>&)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		return FMonolithActionResult::Success(Obj);
	});

	TSharedPtr<FJsonObject> P1 = MakeShared<FJsonObject>();
	P1->SetStringField(TEXT("known"), TEXT("x"));
	FMonolithActionResult R1 = FMonolithToolRegistry::Get().ExecuteAction(Ns, TEXT("clean_ok"), P1);

	TestTrue(TEXT("clean call succeeded"), R1.bSuccess);
	TestTrue(TEXT("result object present"), R1.Result.IsValid());
	if (R1.Result.IsValid())
	{
		TestFalse(TEXT("no warnings key manufactured on a clean success"),
			R1.Result->HasField(TEXT("warnings")));
	}

	// (b) Clean refusal — ErrorMessage identical to what the handler said.
	Register(TEXT("clean_fail"), OneKnownParamSchema(), [](const TSharedPtr<FJsonObject>&)
	{
		return FMonolithActionResult::Error(TEXT("REFUSED_PLAIN"));
	});

	TSharedPtr<FJsonObject> P2 = MakeShared<FJsonObject>();
	P2->SetStringField(TEXT("known"), TEXT("x"));
	FMonolithActionResult R2 = FMonolithToolRegistry::Get().ExecuteAction(Ns, TEXT("clean_fail"), P2);

	TestFalse(TEXT("clean call refused"), R2.bSuccess);
	TestEqual(TEXT("error message untouched when there is nothing to add"),
		R2.ErrorMessage, FString(TEXT("REFUSED_PLAIN")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 7: CHARACTERISATION — a NUMBER parked on the `warnings` key is destroyed by the merge.
//
// 🛑 READ THIS BEFORE TRUSTING IT: this test passes on the PRE-CHANGE build too. It is not a
// regression guard for anything I changed; it exists to MEASURE what the warning-channel audit
// could only predict, so the rename of MonolithMeshAccessibilityActions.cpp's
// `warnings` count to `warning_count` rests on an observation rather than on a reading.
//
// It reproduces mesh.validate_interactive_reach's exact shape — a count on the `warnings` key
// — and shows that ONE undeclared param is enough to replace it with an array, silently.
// If this test ever FAILS, the merge has started preserving non-array values on that key and
// the rename's justification should be revisited rather than assumed.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWarningChannelNumberKeyDestroyedTest,
	"Monolith.WarningChannel.NumberOnWarningsKeyIsDestroyed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWarningChannelNumberKeyDestroyedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithWarningChannelTestDetail;

	const FString Ac = TEXT("count_on_warnings_key");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	Register(Ac, OneKnownParamSchema(), [](const TSharedPtr<FJsonObject>&)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("warnings"), 3);   // the mesh-accessibility shape
		return FMonolithActionResult::Success(Obj);
	});

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("known"), TEXT("x"));
	Params->SetStringField(TEXT("tag"), TEXT("typo"));   // undeclared -> one dispatch warning

	FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params);

	TestTrue(TEXT("call succeeded"), R.bSuccess);
	if (!R.Result.IsValid()) { return false; }

	double AsNumber = -1.0;
	const bool bStillANumber = R.Result->TryGetNumberField(TEXT("warnings"), AsNumber);
	TestFalse(TEXT("the count is GONE — `warnings` is no longer a number"), bStillANumber);

	const TArray<FString> Warnings = GetWarnings(R);
	TestEqual(TEXT("replaced by the dispatch layer's array"), Warnings.Num(), 1);
	TestTrue(TEXT("and the array is the unknown-param warning, not the count"),
		AnyContains(Warnings, TEXT("tag")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
