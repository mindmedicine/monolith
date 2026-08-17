// SPDX-License-Identifier: MIT
// Required-`asset_path` enforcement + the SCOPED `system_path` alias — regression tests.
//
// Two defects, one of which was introduced fixing the other.
//
// 1. The registry's required-param loop explicitly SKIPPED `asset_path`, on the stated
//    grounds that GetAssetPath() "produces a clear error message itself". It never did — it
//    returned an empty FString silently. The skip made `asset_path` de-facto OPTIONAL on
//    every schema that declared it required, so a diagnostic run against a mistyped asset
//    path was indistinguishable from a clean pass.
//
// 2. Deleting that skip needed the legacy `system_path` spelling to keep working. It was
//    first done with a UNIVERSAL rewrite applied to any schema declaring `asset_path` —
//    which silently widened the contract of all 25 namespaces (a *material* action accepted
//    `system_path`) and made every missing-param error advertise a spelling meaningful in
//    one. That is the same "silently accept an undeclared param spelling" defect the work
//    existed to kill.
//
// The alias is now DECLARED per-schema, on the ~110 Niagara schemas whose handlers carry the
// historical `system_path` fallback and on nothing else, and rides the existing K2
// ApplyAliases path. These tests pin BOTH halves: the alias still works where it is
// declared, and it is rejected where it is not — including that the two error messages
// differ, since an error that advertises a param the action does not take is the defect.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithRequiredAssetPathTestDetail
{
	/** Captures whether the handler ran at all, and what path value it received. */
	struct FCapturingHandler
	{
		FString CapturedKey = TEXT("asset_path");
		FString CapturedValue;
		bool bSawCall = false;

		FMonolithActionResult operator()(const TSharedPtr<FJsonObject>& Params)
		{
			bSawCall = true;
			if (Params.IsValid())
			{
				Params->TryGetStringField(CapturedKey, CapturedValue);
			}
			TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetBoolField(TEXT("ok"), true);
			return FMonolithActionResult::Success(R);
		}
	};

	static const TCHAR* Ns = TEXT("monolith_test_required_path");

	/** The discriminator between a context-correct error and one that over-advertises. */
	static const TCHAR* AliasHintPhrase = TEXT("may also be spelled");

	/** Schema as carried by the ~110 Niagara actions that historically took `system_path`. */
	static TSharedPtr<FJsonObject> AliasedSchema()
	{
		return FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Required path under test."), { TEXT("system_path") })
			.Build();
	}

	/** Schema as carried by every OTHER namespace — same param, no legacy spelling. */
	static TSharedPtr<FJsonObject> PlainSchema()
	{
		return FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Required path under test."))
			.Build();
	}

	static TSharedRef<FCapturingHandler> Register(const FString& Action, const TSharedPtr<FJsonObject>& Schema)
	{
		TSharedRef<FCapturingHandler> Cap = MakeShared<FCapturingHandler>();
		FMonolithToolRegistry::Get().RegisterAction(Ns, Action, TEXT("Test."),
			FMonolithActionHandler::CreateLambda(
				[Cap](const TSharedPtr<FJsonObject>& P) { return (*Cap)(P); }),
			Schema);
		return Cap;
	}
}

// ---------------------------------------------------------------------------
// Test 1: required `asset_path` omitted entirely → hard error, handler never runs.
// This is defect (1) itself. Before the fix this returned Success with an empty path.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithRequiredAssetPathMissingErrorsTest,
	"Monolith.RequiredAssetPath.MissingErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRequiredAssetPathMissingErrorsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithRequiredAssetPathTestDetail;

	const FString Ac = TEXT("missing_path");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	TSharedRef<FCapturingHandler> Cap = Register(Ac, AliasedSchema());

	// Supply everything EXCEPT the path.
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("_unused"), TEXT("x"));

	FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params);

	TestFalse(TEXT("missing asset_path must FAIL, not return a plausible payload"), R.bSuccess);
	TestFalse(TEXT("handler must never be reached"), Cap->bSawCall);
	TestTrue(TEXT("error names the missing param"), R.ErrorMessage.Contains(TEXT("asset_path")));
	TestTrue(TEXT("error states nothing happened"),
		R.ErrorMessage.Contains(TEXT("NOTHING WAS READ OR CHANGED")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: `system_path` alone still works where the alias IS declared.
// This is the back-compat guarantee that makes enforcing `asset_path` safe.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithRequiredAssetPathSystemPathAliasTest,
	"Monolith.RequiredAssetPath.SystemPathAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRequiredAssetPathSystemPathAliasTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithRequiredAssetPathTestDetail;

	const FString Ac = TEXT("alias_path");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	TSharedRef<FCapturingHandler> Cap = Register(Ac, AliasedSchema());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system_path"), TEXT("/Game/FX/_Probes/NS_Test_Thing"));

	FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params);

	TestTrue(TEXT("system_path caller still succeeds where the alias is declared"), R.bSuccess);
	TestTrue(TEXT("handler was reached"), Cap->bSawCall);
	TestEqual(TEXT("handler saw the value under the canonical key"),
		Cap->CapturedValue, FString(TEXT("/Game/FX/_Probes/NS_Test_Thing")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: supplying BOTH spellings is rejected rather than silently preferring one.
// Structural, not value-based: it must fail even when the two values are identical.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithRequiredAssetPathBothSuppliedRejectedTest,
	"Monolith.RequiredAssetPath.BothSuppliedRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRequiredAssetPathBothSuppliedRejectedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithRequiredAssetPathTestDetail;

	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	// Differing values.
	{
		const FString Ac = TEXT("both_paths");
		TSharedRef<FCapturingHandler> Cap = Register(Ac, AliasedSchema());

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"),  TEXT("/Game/A"));
		Params->SetStringField(TEXT("system_path"), TEXT("/Game/B"));

		FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params);

		TestFalse(TEXT("ambiguous double-spelled path must be rejected"), R.bSuccess);
		TestFalse(TEXT("handler must never be reached"), Cap->bSawCall);
		TestTrue(TEXT("error explains the collision"), R.ErrorMessage.Contains(TEXT("collision")));
		TestTrue(TEXT("collision error names both spellings"),
			R.ErrorMessage.Contains(TEXT("asset_path")) && R.ErrorMessage.Contains(TEXT("system_path")));
	}

	// Identical values — still a collision.
	{
		const FString Ac = TEXT("both_paths_same");
		TSharedRef<FCapturingHandler> Cap = Register(Ac, AliasedSchema());

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"),  TEXT("/Game/Same"));
		Params->SetStringField(TEXT("system_path"), TEXT("/Game/Same"));

		FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params);

		TestFalse(TEXT("collision rule is structural, not value-based"), R.bSuccess);
		TestFalse(TEXT("handler must never be reached"), Cap->bSawCall);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: regression guard — a well-formed `asset_path` call still passes untouched,
// on both the aliased and the plain schema.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithRequiredAssetPathHappyPathUnchangedTest,
	"Monolith.RequiredAssetPath.HappyPathUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRequiredAssetPathHappyPathUnchangedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithRequiredAssetPathTestDetail;

	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	const FString Expected = TEXT("/Game/FX/_Probes/NS_Test_Thing");

	{
		const FString Ac = TEXT("happy_path_aliased");
		TSharedRef<FCapturingHandler> Cap = Register(Ac, AliasedSchema());

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Expected);

		FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params);
		TestTrue(TEXT("canonical caller still succeeds (aliased schema)"), R.bSuccess);
		TestTrue(TEXT("handler was reached"), Cap->bSawCall);
		TestEqual(TEXT("path passed through unchanged"), Cap->CapturedValue, Expected);
	}

	{
		const FString Ac = TEXT("happy_path_plain");
		TSharedRef<FCapturingHandler> Cap = Register(Ac, PlainSchema());

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Expected);

		FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params);
		TestTrue(TEXT("canonical caller still succeeds (plain schema)"), R.bSuccess);
		TestTrue(TEXT("handler was reached"), Cap->bSawCall);
		TestEqual(TEXT("path passed through unchanged"), Cap->CapturedValue, Expected);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: an action that declares its OWN path param (script_path) must not have a
// stray `system_path` promoted into an `asset_path` it never declared. Guards the
// Niagara graph-op actions, which are script-addressed.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithRequiredAssetPathNonParticipatingSchemaTest,
	"Monolith.RequiredAssetPath.NonParticipatingSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRequiredAssetPathNonParticipatingSchemaTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithRequiredAssetPathTestDetail;

	const FString Ac = TEXT("script_addressed");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	// No asset_path anywhere in this schema.
	TSharedPtr<FJsonObject> Schema = FParamSchemaBuilder()
		.RequiredAssetPath(TEXT("script_path"), TEXT("Script-addressed action."))
		.Build();

	TSharedRef<FCapturingHandler> Cap = Register(Ac, Schema);
	Cap->CapturedKey = TEXT("script_path");

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("script_path"), TEXT("/Game/FX/Modules/NM_Test.NM_Test"));
	Params->SetStringField(TEXT("system_path"), TEXT("/Game/ShouldBeIgnoredAsAssetPath"));

	FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params);

	TestTrue(TEXT("script-addressed call succeeds"), R.bSuccess);
	TestEqual(TEXT("script_path untouched"),
		Cap->CapturedValue, FString(TEXT("/Game/FX/Modules/NM_Test.NM_Test")));

	// The stray system_path must NOT have been promoted to asset_path.
	FString StrayAssetPath;
	TestFalse(TEXT("system_path must not be promoted into an undeclared asset_path"),
		Params->TryGetStringField(TEXT("asset_path"), StrayAssetPath));
	return true;
}

// ---------------------------------------------------------------------------
// Test 6: THE SCOPING TEST — defect (2). An action that declares `asset_path` WITHOUT
// the legacy alias (i.e. every namespace except Niagara) must REJECT `system_path`
// rather than silently accepting it. Under the universal-rewrite implementation this
// call succeeded, which is the contract widening being closed here.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithRequiredAssetPathAliasIsScopedTest,
	"Monolith.RequiredAssetPath.AliasIsScoped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRequiredAssetPathAliasIsScopedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithRequiredAssetPathTestDetail;

	const FString Ac = TEXT("plain_asset_path");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	TSharedRef<FCapturingHandler> Cap = Register(Ac, PlainSchema());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system_path"), TEXT("/Game/FX/_Probes/NS_Test_Thing"));

	FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params);

	TestFalse(TEXT("an action that never declared system_path must REJECT it"), R.bSuccess);
	TestFalse(TEXT("handler must never be reached"), Cap->bSawCall);
	TestTrue(TEXT("error names the param that is actually missing"),
		R.ErrorMessage.Contains(TEXT("Missing required param(s): [asset_path]")));
	TestTrue(TEXT("error still states nothing happened"),
		R.ErrorMessage.Contains(TEXT("NOTHING WAS READ OR CHANGED")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 7: the missing-param error is CONTEXT-CORRECT. The alias hint appears only on
// the action that declares the alias. Asserted on the hint phrase, not on the bare
// string "system_path": the error echoes the caller's provided keys, so the literal
// token legitimately appears in both messages and would not discriminate.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithRequiredAssetPathErrorMentionsAliasOnlyWhenDeclaredTest,
	"Monolith.RequiredAssetPath.ErrorMentionsAliasOnlyWhenDeclared",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRequiredAssetPathErrorMentionsAliasOnlyWhenDeclaredTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithRequiredAssetPathTestDetail;

	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	FString AliasedError;
	FString PlainError;
	FString ScriptError;

	{
		const FString Ac = TEXT("err_aliased");
		Register(Ac, AliasedSchema());
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		AliasedError = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params).ErrorMessage;
	}
	{
		const FString Ac = TEXT("err_plain");
		Register(Ac, PlainSchema());
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		PlainError = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params).ErrorMessage;
	}
	{
		const FString Ac = TEXT("err_script");
		TSharedPtr<FJsonObject> Schema = FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("script_path"), TEXT("Script-addressed action."))
			.Build();
		Register(Ac, Schema);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		ScriptError = FMonolithToolRegistry::Get().ExecuteAction(Ns, Ac, Params).ErrorMessage;
	}

	TestTrue(TEXT("aliased action advertises the alias"),
		AliasedError.Contains(AliasHintPhrase));
	TestTrue(TEXT("aliased action names the alias by spelling"),
		AliasedError.Contains(TEXT("'system_path'")));

	TestFalse(TEXT("plain asset_path action must NOT advertise system_path"),
		PlainError.Contains(AliasHintPhrase));
	TestFalse(TEXT("plain asset_path action must not name the alias at all"),
		PlainError.Contains(TEXT("system_path")));

	TestFalse(TEXT("script_path action must NOT advertise the alias"),
		ScriptError.Contains(AliasHintPhrase));
	TestFalse(TEXT("script_path action must not name system_path"),
		ScriptError.Contains(TEXT("system_path")));

	TestNotEqual(TEXT("the two errors must actually differ"), AliasedError, PlainError);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
