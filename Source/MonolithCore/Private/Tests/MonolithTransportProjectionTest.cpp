// SPDX-License-Identifier: MIT
// TRANSPORT PROJECTION tests — the seam that had no tests at all.
//
// WHY THIS FILE EXISTS
//
// Of the 49 test files in Monolith, zero referenced FMonolithHttpServer, HandleToolsCall or
// `tools/call`. Every one of them asserted on the FMonolithActionResult STRUCT returned by
// FMonolithToolRegistry::ExecuteAction. That is sound for anything on the success path — the
// projection serialises `Result` verbatim — but it is VACUOUS for any field the projection
// drops, because the assertion passes whether or not a caller could ever observe the value.
//
// The measured consequence: `ErrorData` was asserted green in two test files for as long as
// the field has existed, while the transport's error branch carried `ErrorMessage` and nothing
// else, on all ~42 error paths that set it. One of those paths (animation.set_transition_rule)
// told the caller "See compile_errors" for diagnostics that its own rollback had already
// destroyed everywhere else. Nothing failed. Nothing could have.
//
// So these tests assert on the WIRE PAYLOAD, and they get there by calling the same two
// functions HandleToolsCall calls, in the same order:
//
//     FMonolithToolRegistry::Get().ExecuteAction(ns, action, params)   // dispatch + merges
//     FMonolithHttpServer::ProjectResultToToolCallPayload(result)     // -> content[] + isError
//
// The projection was EXTRACTED from HandleToolsCall for exactly this reason; it is not
// reimplemented here. A test that copies the projection proves only that two copies agree.
//
// NOT covered by this file: the JSON-RPC envelope (FMonolithJsonUtils::SuccessResponse) and
// HandleToolsCall's argument normalisation. Both sit outside the field-survival question.
//
// MAINTENANCE CONTRACT: if you add a member to FMonolithActionResult, add a row to
// Monolith.Transport.FieldProjectionCoverage below. Nothing here can detect a new member on
// its own — that check is a human one, and this comment is where it is recorded.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithHttpServer.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithTransportProjectionTestDetail
{
	/** Throwaway namespace for fixture actions. Torn down by ON_SCOPE_EXIT in every test. */
	static const TCHAR* Ns = TEXT("monolith_test_transport");

	/** Register a fixture action that ignores its params and returns a canned result. */
	static void Register(
		const FString& Action,
		TFunction<FMonolithActionResult()> Body,
		const TSharedPtr<FJsonObject>& Schema = nullptr)
	{
		FMonolithToolRegistry::Get().RegisterAction(Ns, Action, TEXT("Transport projection fixture."),
			FMonolithActionHandler::CreateLambda(
				[Body](const TSharedPtr<FJsonObject>&) { return Body(); }),
			Schema);
	}

	/**
	 * THE call under test. Dispatch then project — the exact pair, in the exact order,
	 * that FMonolithHttpServer::HandleToolsCall performs.
	 */
	static TSharedPtr<FJsonObject> CallAndProject(
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params = nullptr)
	{
		const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
			Ns, Action, Params.IsValid() ? Params : MakeShared<FJsonObject>());
		return FMonolithHttpServer::ProjectResultToToolCallPayload(R);
	}

	/**
	 * Pull the single text payload out of an MCP tools/call result object. Returns false
	 * with a human-readable reason in OutWhy if the envelope is not the documented shape,
	 * so a shape regression reports WHAT changed rather than just "false".
	 */
	static bool ReadWire(const TSharedPtr<FJsonObject>& Payload, FString& OutText, bool& bOutIsError, FString& OutWhy)
	{
		OutText.Reset();
		bOutIsError = false;
		OutWhy.Reset();

		if (!Payload.IsValid())
		{
			OutWhy = TEXT("projection returned a null payload");
			return false;
		}
		if (!Payload->TryGetBoolField(TEXT("isError"), bOutIsError))
		{
			OutWhy = TEXT("payload has no boolean `isError`");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Payload->TryGetArrayField(TEXT("content"), Arr) || !Arr)
		{
			OutWhy = TEXT("payload has no `content` array");
			return false;
		}
		if (Arr->Num() != 1)
		{
			OutWhy = FString::Printf(TEXT("content[] holds %d entries, expected exactly 1"), Arr->Num());
			return false;
		}
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		if (!(*Arr)[0].IsValid() || !(*Arr)[0]->TryGetObject(Entry) || !Entry || !Entry->IsValid())
		{
			OutWhy = TEXT("content[0] is not an object");
			return false;
		}
		FString Type;
		if (!(*Entry)->TryGetStringField(TEXT("type"), Type) || Type != TEXT("text"))
		{
			OutWhy = FString::Printf(TEXT("content[0].type is '%s', expected 'text'"), *Type);
			return false;
		}
		if (!(*Entry)->TryGetStringField(TEXT("text"), OutText))
		{
			OutWhy = TEXT("content[0].text is missing or not a string");
			return false;
		}
		return true;
	}

	/** Sorted key set of the envelope object — used to pin that NOTHING else is emitted. */
	static TArray<FString> PayloadKeys(const TSharedPtr<FJsonObject>& Payload)
	{
		TArray<FString> Keys;
		if (Payload.IsValid())
		{
			for (const auto& Pair : Payload->Values)
			{
				Keys.Add(MonolithKeyToString(Pair.Key));
			}
		}
		Keys.Sort();
		return Keys;
	}

	/**
	 * A result payload with the shapes that a lossy projection tends to mangle: nested
	 * object, array with mixed element types, escaped quotes and a backslash, a non-ASCII
	 * character, a zero, a negative number and an explicit null.
	 */
	static TSharedPtr<FJsonObject> MakeVerbatimProbe()
	{
		TSharedPtr<FJsonObject> Nested = MakeShared<FJsonObject>();
		Nested->SetStringField(TEXT("quoted"), TEXT("he said \"x\" and \\ that"));
		Nested->SetBoolField(TEXT("flag"), false);

		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(0));
		Arr.Add(MakeShared<FJsonValueNumber>(-1));
		Arr.Add(MakeShared<FJsonValueString>(TEXT("an em dash — survives")));

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("marker"), TEXT("VERBATIM_MARKER_7f3a"));
		Root->SetNumberField(TEXT("count"), 42);
		Root->SetObjectField(TEXT("nested"), Nested);
		Root->SetArrayField(TEXT("list"), Arr);
		Root->SetField(TEXT("nothing"), MakeShared<FJsonValueNull>());
		return Root;
	}

	/**
	 * Round-trip an object through serialise+parse. Both sides of the verbatim comparison
	 * go through this, so the two objects being compared came out of the SAME reader and
	 * cannot differ merely in how a number is internally represented. The comparison stays
	 * strict about everything that matters: dropped keys, renamed keys, truncated arrays,
	 * mangled escapes.
	 */
	static TSharedPtr<FJsonObject> Normalise(const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return nullptr;
		}
		return FMonolithJsonUtils::Parse(FMonolithJsonUtils::Serialize(Obj));
	}

	static bool JsonEquals(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		if (!A.IsValid() || !B.IsValid())
		{
			return A.IsValid() == B.IsValid();
		}
		const FJsonValueObject VA(A);
		const FJsonValueObject VB(B);
		return FJsonValue::CompareEqual(VA, VB);
	}
}

// ---------------------------------------------------------------------------
// Test 1: SUCCESS carries `Result` VERBATIM.
//
// CATCHES: a projection that summarises, truncates, re-keys, flattens or
// re-escapes the handler's result object; one that emits `{}` when a result is
// present; one that inverts or omits `isError` on the success path. This is the
// assertion that licenses the rest of the suite to keep testing the struct on
// the success path — if it ever fails, ~40 files' worth of success assertions
// stop predicting the wire and we want to know immediately.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithTransportSuccessResultVerbatimTest,
	"Monolith.Transport.SuccessResultVerbatim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithTransportSuccessResultVerbatimTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithTransportProjectionTestDetail;

	const FString Ac = TEXT("verbatim");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	Register(Ac, [] { return FMonolithActionResult::Success(MakeVerbatimProbe()); });

	const TSharedPtr<FJsonObject> Payload = CallAndProject(Ac);

	FString Text, Why;
	bool bIsError = true;
	const bool bShapeOk = ReadWire(Payload, Text, bIsError, Why);
	TestTrue(FString::Printf(TEXT("wire envelope is the documented shape (%s)"), *Why), bShapeOk);
	if (!bShapeOk)
	{
		return false;
	}

	TestFalse(TEXT("a successful action must project isError=false"), bIsError);

	const TSharedPtr<FJsonObject> Actual = FMonolithJsonUtils::Parse(Text);
	TestTrue(TEXT("content[0].text parses back as a JSON object"), Actual.IsValid());

	const TSharedPtr<FJsonObject> Expected = Normalise(MakeVerbatimProbe());
	TestTrue(TEXT("Result reaches the wire VERBATIM — no key dropped, renamed or reshaped"),
		JsonEquals(Expected, Actual));

	// Named checks so a failure says WHICH shape broke rather than just "not equal".
	if (Actual.IsValid())
	{
		FString Marker;
		TestTrue(TEXT("top-level string survives"), Actual->TryGetStringField(TEXT("marker"), Marker));
		TestEqual(TEXT("top-level string is unmodified"), Marker, FString(TEXT("VERBATIM_MARKER_7f3a")));

		const TSharedPtr<FJsonObject>* Nested = nullptr;
		TestTrue(TEXT("nested object survives"), Actual->TryGetObjectField(TEXT("nested"), Nested) && Nested);

		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		const bool bList = Actual->TryGetArrayField(TEXT("list"), List) && List;
		TestTrue(TEXT("array survives"), bList);
		if (bList)
		{
			TestEqual(TEXT("array is not truncated"), List->Num(), 3);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: the envelope is EXACTLY `content` + `isError`.
//
// CATCHES: anyone "fixing" a dropped field by inventing a sibling key on the
// tools/call result (`data`, `error_code`, `warnings`), which is a wire shape no
// MCP client reads — the failure mode this whole change set exists to avoid. Also
// catches content[] gaining or losing entries, and content[0] ceasing to be a
// `{type:"text"}` block. Asserted on BOTH branches because they are written
// separately and only one of them is ever exercised by hand.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithTransportEnvelopeShapeTest,
	"Monolith.Transport.EnvelopeShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithTransportEnvelopeShapeTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithTransportProjectionTestDetail;

	const FString AcOk   = TEXT("shape_ok");
	const FString AcFail = TEXT("shape_fail");
	const FString AcNull = TEXT("shape_null");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	Register(AcOk,   [] { return FMonolithActionResult::Success(MakeShared<FJsonObject>()); });
	Register(AcFail, [] { return FMonolithActionResult::Error(TEXT("refused")); });
	Register(AcNull, [] { return FMonolithActionResult::Success(nullptr); });

	TArray<FString> ExpectedKeys = { TEXT("content"), TEXT("isError") };
	ExpectedKeys.Sort();

	for (const FString& Ac : { AcOk, AcFail, AcNull })
	{
		const TSharedPtr<FJsonObject> Payload = CallAndProject(Ac);
		const TArray<FString> Keys = PayloadKeys(Payload);

		TestEqual(FString::Printf(TEXT("'%s': envelope carries exactly 2 keys (got [%s])"),
			*Ac, *FString::Join(Keys, TEXT(", "))), Keys.Num(), ExpectedKeys.Num());
		TestTrue(FString::Printf(TEXT("'%s': envelope keys are exactly {content, isError} (got [%s])"),
			*Ac, *FString::Join(Keys, TEXT(", "))), Keys == ExpectedKeys);

		FString Text, Why;
		bool bIsError = false;
		TestTrue(FString::Printf(TEXT("'%s': content[] is one text block (%s)"), *Ac, *Why),
			ReadWire(Payload, Text, bIsError, Why));
	}

	// A Success(nullptr) carrying no warnings must still serialise to exactly "{}" —
	// this is the documented back-compat behaviour of the null-result branch.
	{
		FString Text, Why;
		bool bIsError = true;
		if (ReadWire(CallAndProject(AcNull), Text, bIsError, Why))
		{
			TestEqual(TEXT("Success(nullptr) projects as the literal {}"), Text, FString(TEXT("{}")));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: FAILURE carries `ErrorMessage`, and carries NOTHING ELSE structured.
//
// CATCHES: a projection that replaces the handler's message with a generic
// string, truncates it, or forgets `isError=true`. The negative half catches the
// opposite drift: `ErrorCode` appearing on the wire. That number is an in-process
// contract only (FPipelineAdapter, MonolithNiagaraQueryLibrary read it) and ~31
// assertions across 8 test files pin it — this test is what stops those from
// being silently reinterpreted as a caller-visible contract without review.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithTransportFailureCarriesErrorMessageTest,
	"Monolith.Transport.FailureCarriesErrorMessage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithTransportFailureCarriesErrorMessageTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithTransportProjectionTestDetail;

	const FString Ac = TEXT("plain_refusal");
	const FString Sentinel = TEXT("REFUSAL_SENTINEL_c19d — refused for a reason only this string knows");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	Register(Ac, [Sentinel]
	{
		return FMonolithActionResult::Error(Sentinel, FMonolithJsonUtils::ErrInvalidParams);
	});

	FString Text, Why;
	bool bIsError = false;
	const bool bShapeOk = ReadWire(CallAndProject(Ac), Text, bIsError, Why);
	TestTrue(FString::Printf(TEXT("wire envelope is the documented shape (%s)"), *Why), bShapeOk);
	if (!bShapeOk)
	{
		return false;
	}

	TestTrue(TEXT("a refused action must project isError=true"), bIsError);

	// Equality, not Contains: with no warnings and no ErrorData attached, the message must
	// arrive byte-for-byte. Anything appended unconditionally would break this, which is
	// the point — the append is supposed to be conditional.
	TestEqual(TEXT("ErrorMessage reaches the wire unmodified"), Text, Sentinel);

	TestFalse(TEXT("ErrorCode must NOT appear on the wire (in-process contract only)"),
		Text.Contains(TEXT("-32602")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: FAILURE carries WARNINGS INLINE — the 54ab635 behaviour, end to end.
//
// CATCHES: the exact regression 54ab635 fixed — re-gating warning collection on
// `bSuccess` in ExecuteAction, which destroyed every warning a refused call had
// produced. Both origins are asserted because they die differently:
//   * HANDLER-origin (Result::WithWarning) dies if the refusal branch stops
//     folding Warnings into ErrorMessage;
//   * DISPATCH-origin (the K3 unknown-param soft-warn) dies if the collection is
//     moved back inside the success branch — it is computed BEFORE the handler
//     runs, so that gate is precisely what used to discard it.
// It also catches a transport that stops carrying ErrorMessage's suffix.
//
// Until this file existed, this behaviour was proved only by two hand-made live
// MCP calls, and by no test at all.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithTransportFailureCarriesWarningsInlineTest,
	"Monolith.Transport.FailureCarriesWarningsInline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithTransportFailureCarriesWarningsInlineTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithTransportProjectionTestDetail;

	const FString Ac = TEXT("refuse_with_warnings");
	const FString W1 = TEXT("WARN_SENTINEL_a1 coerced the input");
	const FString W2 = TEXT("WARN_SENTINEL_b2 and also this");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	// A schema is required for the K3 unknown-param soft-warn to fire at all.
	TSharedPtr<FJsonObject> Schema = FParamSchemaBuilder()
		.Optional(TEXT("known"), TEXT("string"), TEXT("A declared param."))
		.Build();

	Register(Ac, [W1, W2]
	{
		return FMonolithActionResult::Error(TEXT("REFUSED_SENTINEL_d4"))
			.WithWarning(W1)
			.WithWarning(W2);
	}, Schema);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("known"), TEXT("x"));
	Params->SetStringField(TEXT("typoed_param"), TEXT("y"));

	FString Text, Why;
	bool bIsError = false;
	const bool bShapeOk = ReadWire(CallAndProject(Ac, Params), Text, bIsError, Why);
	TestTrue(FString::Printf(TEXT("wire envelope is the documented shape (%s)"), *Why), bShapeOk);
	if (!bShapeOk)
	{
		return false;
	}

	TestTrue(TEXT("still a refusal"), bIsError);
	TestTrue(TEXT("the refusal reason is still first in the message"),
		Text.StartsWith(TEXT("REFUSED_SENTINEL_d4")));

	TestTrue(TEXT("handler warning 1 SURVIVES the refusal and reaches the wire"), Text.Contains(W1));
	TestTrue(TEXT("handler warning 2 SURVIVES the refusal and reaches the wire"), Text.Contains(W2));
	TestTrue(TEXT("the warning block is labelled, not silently concatenated"),
		Text.Contains(TEXT("Warnings also raised on this call")));

	// Dispatch-origin warning: computed before the handler ran, and therefore the exact
	// thing the old `if (bSuccess)` gate threw away.
	if (!FMonolithParamSchema::IsStrictParamsEnabled())
	{
		TestTrue(TEXT("the PRE-HANDLER unknown-param warning also survives a refusal"),
			Text.Contains(TEXT("typoed_param")));
	}
	else
	{
		AddWarning(TEXT("STRICT_PARAMS=1: skipped the dispatch-origin warning assertion (the call is rejected before the handler)"));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: SUCCESS carries warnings as a structured `warnings[]` ON THE WIRE.
//
// CATCHES: the success-path half of the same channel — the merge failing to
// attach Result::Warnings, response shaping stripping the key back out from
// under it, or the warnings landing somewhere a client cannot see. Asserted on
// the PARSED wire text rather than on the struct, because "it is on the struct"
// is exactly the claim that turned out not to mean anything.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithTransportSuccessCarriesWarningsArrayTest,
	"Monolith.Transport.SuccessCarriesWarningsArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithTransportSuccessCarriesWarningsArrayTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithTransportProjectionTestDetail;

	const FString Ac = TEXT("succeed_with_warning");
	const FString W  = TEXT("WARN_SENTINEL_e5 the value was clamped");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	Register(Ac, [W]
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		return FMonolithActionResult::Success(Obj).WithWarning(W);
	});

	FString Text, Why;
	bool bIsError = true;
	const bool bShapeOk = ReadWire(CallAndProject(Ac), Text, bIsError, Why);
	TestTrue(FString::Printf(TEXT("wire envelope is the documented shape (%s)"), *Why), bShapeOk);
	if (!bShapeOk)
	{
		return false;
	}

	TestFalse(TEXT("still a success"), bIsError);

	const TSharedPtr<FJsonObject> Parsed = FMonolithJsonUtils::Parse(Text);
	TestTrue(TEXT("wire text parses"), Parsed.IsValid());

	bool bFound = false;
	if (Parsed.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Parsed->TryGetArrayField(TEXT("warnings"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString S;
				if (V.IsValid() && V->TryGetString(S) && S == W) { bFound = true; break; }
			}
		}
	}
	TestTrue(TEXT("the handler's warning reaches the wire inside warnings[]"), bFound);
	return true;
}

// ---------------------------------------------------------------------------
// Test 6: FAILURE carries `ErrorData` INLINE.
//
// CATCHES: the original defect, directly. Delete the FormatErrorDataBlock append
// in ExecuteAction's refusal branch and this test goes red — where before, the
// two test files asserting on `ErrorData` stayed green with the field being
// destroyed one layer down. Also pins that the payload is emitted as parseable
// JSON rather than prose, which is the only reason text is an acceptable
// carrier for it, and that an error WITHOUT ErrorData gains no empty block.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithTransportFailureCarriesErrorDataTest,
	"Monolith.Transport.FailureCarriesErrorData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithTransportFailureCarriesErrorDataTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithTransportProjectionTestDetail;

	const FString Ac     = TEXT("refuse_with_data");
	const FString AcBare = TEXT("refuse_without_data");
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	Register(Ac, []
	{
		// Mirrors the shape of animation.set_transition_rule's compile-error payload —
		// the one site whose diagnostics exist nowhere else once it has rolled back.
		TArray<TSharedPtr<FJsonValue>> ErrArr;
		ErrArr.Add(MakeShared<FJsonValueString>(TEXT("COMPILE_SENTINEL_f6: Variable 'Foo' not found")));
		ErrArr.Add(MakeShared<FJsonValueString>(TEXT("COMPILE_SENTINEL_g7: Pin type mismatch")));
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("compile_errors"), ErrArr);

		return FMonolithActionResult::Error(TEXT("Rule compiled with errors — rolled back."))
			.WithErrorData(Data);
	});
	Register(AcBare, [] { return FMonolithActionResult::Error(TEXT("BARE_SENTINEL_h8")); });

	FString Text, Why;
	bool bIsError = false;
	const bool bShapeOk = ReadWire(CallAndProject(Ac), Text, bIsError, Why);
	TestTrue(FString::Printf(TEXT("wire envelope is the documented shape (%s)"), *Why), bShapeOk);
	if (!bShapeOk)
	{
		return false;
	}

	TestTrue(TEXT("still a refusal"), bIsError);
	TestTrue(TEXT("the ErrorData key name reaches the wire"), Text.Contains(TEXT("compile_errors")));
	TestTrue(TEXT("ErrorData entry 1 reaches the wire — it exists nowhere else"),
		Text.Contains(TEXT("COMPILE_SENTINEL_f6")));
	TestTrue(TEXT("ErrorData entry 2 reaches the wire"),
		Text.Contains(TEXT("COMPILE_SENTINEL_g7")));

	// Machine-recoverable, not merely present: the payload must sit in a json fence a
	// client can scan for, and what it lifts out must parse.
	const int32 FenceStart = Text.Find(TEXT("```json"));
	TestTrue(TEXT("ErrorData is emitted inside a ```json fence"), FenceStart != INDEX_NONE);
	if (FenceStart != INDEX_NONE)
	{
		const int32 BodyStart = FenceStart + 8; // past "```json\n"
		const int32 FenceEnd = Text.Find(TEXT("```"), ESearchCase::CaseSensitive, ESearchDir::FromStart, BodyStart);
		TestTrue(TEXT("the json fence is closed"), FenceEnd != INDEX_NONE);
		if (FenceEnd != INDEX_NONE && FenceEnd > BodyStart)
		{
			const FString Body = Text.Mid(BodyStart, FenceEnd - BodyStart).TrimStartAndEnd();
			const TSharedPtr<FJsonObject> Lifted = FMonolithJsonUtils::Parse(Body);
			TestTrue(TEXT("the fenced payload parses back to JSON"), Lifted.IsValid());
			if (Lifted.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
				const bool bArr = Lifted->TryGetArrayField(TEXT("compile_errors"), Arr) && Arr;
				TestTrue(TEXT("the lifted payload still has compile_errors[]"), bArr);
				if (bArr)
				{
					TestEqual(TEXT("both diagnostics survived, not just the first"), Arr->Num(), 2);
				}
			}
		}
	}

	// Negative control: no ErrorData means no block. Without this, FormatErrorDataBlock
	// could start emitting an empty stub on every error in the system and nothing would
	// notice.
	{
		FString BareText, BareWhy;
		bool bBareIsError = false;
		if (ReadWire(CallAndProject(AcBare), BareText, bBareIsError, BareWhy))
		{
			TestEqual(TEXT("an error with no ErrorData is byte-for-byte unchanged"),
				BareText, FString(TEXT("BARE_SENTINEL_h8")));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 7: FIELD PROJECTION COVERAGE — the drift detector.
//
// One row per FMonolithActionResult member, each planting a unique sentinel and
// declaring whether it must reach the wire. This is the generalisation of the
// bug: a field set on the result and absent from the wire.
//
// CATCHES: any currently-carried field silently ceasing to reach a caller, and
// any currently-dropped field silently starting to (an unreviewed wire change).
// DOES NOT CATCH, and cannot: a NEW member added to the struct without a row
// here. C++ gives no runtime enumeration of members, so that check is the
// maintenance contract at the top of this file plus the note in the projection.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithTransportFieldProjectionCoverageTest,
	"Monolith.Transport.FieldProjectionCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithTransportFieldProjectionCoverageTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithTransportProjectionTestDetail;

	struct FRow
	{
		const TCHAR* Field;
		FString      Sentinel;
		bool         bExpectedOnWire;
		const TCHAR* Why;
		TFunction<FMonolithActionResult()> Make;
	};

	const FString SResult   = TEXT("COVER_RESULT_k1");
	const FString SErrMsg   = TEXT("COVER_ERRMSG_k2");
	const FString SWarnOk   = TEXT("COVER_WARN_OK_k3");
	const FString SWarnFail = TEXT("COVER_WARN_FAIL_k4");
	const FString SErrData  = TEXT("COVER_ERRDATA_k5");

	const TArray<FRow> Rows = {
		{ TEXT("Result (success)"), SResult, true,
		  TEXT("serialised verbatim into content[0].text"),
		  [SResult] {
			  TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			  O->SetStringField(TEXT("probe"), SResult);
			  return FMonolithActionResult::Success(O);
		  } },

		{ TEXT("Warnings (success)"), SWarnOk, true,
		  TEXT("merged into the result's warnings[] by ExecuteAction"),
		  [SWarnOk] {
			  return FMonolithActionResult::Success(MakeShared<FJsonObject>()).WithWarning(SWarnOk);
		  } },

		{ TEXT("ErrorMessage (failure)"), SErrMsg, true,
		  TEXT("the ONLY field the error branch of the projection reads"),
		  [SErrMsg] { return FMonolithActionResult::Error(SErrMsg); } },

		{ TEXT("Warnings (failure)"), SWarnFail, true,
		  TEXT("appended to ErrorMessage by FormatWarningBlock — gap #110/#111"),
		  [SWarnFail] {
			  return FMonolithActionResult::Error(TEXT("refused")).WithWarning(SWarnFail);
		  } },

		{ TEXT("ErrorData (failure)"), SErrData, true,
		  TEXT("appended to ErrorMessage by FormatErrorDataBlock; before that it was DROPPED"),
		  [SErrData] {
			  TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
			  D->SetStringField(TEXT("probe"), SErrData);
			  return FMonolithActionResult::Error(TEXT("refused")).WithErrorData(D);
		  } },

		// Deliberately ABSENT. ErrorCode is read in-process by FPipelineAdapter and
		// MonolithNiagaraQueryLibrary, and by no MCP client. If this row ever flips,
		// the wire contract changed and the ~31 ErrorCode assertions elsewhere in the
		// suite changed meaning with it.
		{ TEXT("ErrorCode (failure)"), TEXT("-32602"), false,
		  TEXT("in-process contract only; never projected"),
		  [] { return FMonolithActionResult::Error(TEXT("refused"), FMonolithJsonUtils::ErrInvalidParams); } },
	};

	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };

	int32 Index = 0;
	for (const FRow& Row : Rows)
	{
		const FString Ac = FString::Printf(TEXT("cover_%d"), Index++);
		Register(Ac, Row.Make);

		FString Text, Why;
		bool bIsError = false;
		if (!ReadWire(CallAndProject(Ac), Text, bIsError, Why))
		{
			AddError(FString::Printf(TEXT("%s: envelope malformed (%s)"), Row.Field, *Why));
			continue;
		}

		const bool bOnWire = Text.Contains(Row.Sentinel);
		if (Row.bExpectedOnWire)
		{
			TestTrue(FString::Printf(
				TEXT("%s must reach the wire (%s) — a caller cannot act on a field that stops here"),
				Row.Field, Row.Why), bOnWire);
		}
		else
		{
			TestFalse(FString::Printf(
				TEXT("%s must NOT reach the wire (%s) — its appearance is an unreviewed wire change"),
				Row.Field, Row.Why), bOnWire);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
