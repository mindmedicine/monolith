// SPDX-License-Identifier: MIT
// Automation tests for gap #106 — add_user_parameter silently storing (0,0,0) for a vector
// `default` while reporting "Added user parameter 'X' with default".
//
// SCOPE, STATED HONESTLY. These cover the two halves that need no asset:
//   * WHAT STRING did the caller mean (MonolithNiagaraDefaultLiteral), and
//   * does that string, through the type's OWN registered editor utilities, produce the BYTES
//     the caller asked for (the round-trip test below).
// Together those are the defect: the old decoder produced zeroed bytes and said it had not.
//
// They do NOT cover the store half — that the value survives AddParameter's editor cascade and
// reads back through get_parameter_value / get_user_parameters. That needs a live system and an
// editor, so it must be validated in-editor against the fixture in the staging note:
//   * add_user_parameter type=vector default="1,2,3" -> get_parameter_value must read (1,2,3).
//   * add_user_parameter type=position default="1,2,3" -> must not assert (see the Position
//     branch comment in HandleAddUserParameter; that path was a check() failure, not a wrong
//     value, and no unit test can reach it).
//
// Every row below is built around the OLD, WRONG answer, so the rows are negative controls: the
// shipped decoder returned a valid-but-empty FJsonObject for EVERY string and read 0 out of it,
// so a decoder that still did that would fail every Accept row here. A decoder that refused
// everything would fail them too. Neither degenerate implementation can pass.

#include "Misc/AutomationTest.h"
#include "MonolithNiagaraDefaultLiteral.h"
#include "NiagaraEditorModule.h"
#include "INiagaraEditorTypeUtilities.h"
#include "NiagaraTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithNiagaraDefaultLiteralTestDetail
{
	/** The two spellings measured landing as (0,0,0), as a JSON string value. */
	static TSharedPtr<FJsonValue> Str(const TCHAR* S)
	{
		return MakeShared<FJsonValueString>(FString(S));
	}

	static TSharedPtr<FJsonValue> Num(double N)
	{
		return MakeShared<FJsonValueNumber>(N);
	}

	/** {"x":1,"y":2,"z":3}-style object value. */
	static TSharedPtr<FJsonValue> Obj(std::initializer_list<TPair<const TCHAR*, double>> Fields)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		for (const TPair<const TCHAR*, double>& F : Fields)
		{
			O->SetNumberField(F.Key, F.Value);
		}
		return MakeShared<FJsonValueObject>(O);
	}

	static TSharedPtr<FJsonValue> Arr(std::initializer_list<double> Values)
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const double V : Values)
		{
			A.Add(MakeShared<FJsonValueNumber>(V));
		}
		return MakeShared<FJsonValueArray>(A);
	}
}

using namespace MonolithNiagaraDefaultLiteralTestDetail;
using namespace MonolithNiagaraDefaultLiteral;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraDefaultLiteralJsonTest,
	"Monolith.Niagara.DefaultLiteral.Json",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraDefaultLiteralJsonTest::RunTest(const FString& /*Parameters*/)
{
	const FNiagaraTypeDefinition Vec2  = FNiagaraTypeDefinition::GetVec2Def();
	const FNiagaraTypeDefinition Vec3  = FNiagaraTypeDefinition::GetVec3Def();
	const FNiagaraTypeDefinition Vec4  = FNiagaraTypeDefinition::GetVec4Def();
	const FNiagaraTypeDefinition Color = FNiagaraTypeDefinition::GetColorDef();
	const FNiagaraTypeDefinition Quat  = FNiagaraTypeDefinition::GetQuatDef();
	const FNiagaraTypeDefinition Pos   = FNiagaraTypeDefinition::GetPositionDef();
	const FNiagaraTypeDefinition Flt   = FNiagaraTypeDefinition::GetFloatDef();
	const FNiagaraTypeDefinition Int   = FNiagaraTypeDefinition::GetIntDef();

	// Component arity is read off the type's own footprint, not a table of names.
	TestEqual(TEXT("vec2 is 2 components"),     ComponentCountForType(Vec2), 2);
	TestEqual(TEXT("vec3 is 3 components"),     ComponentCountForType(Vec3), 3);
	TestEqual(TEXT("position is 3 components"), ComponentCountForType(Pos), 3);
	TestEqual(TEXT("vec4 is 4 components"),     ComponentCountForType(Vec4), 4);
	TestEqual(TEXT("quat is 4 components"),     ComponentCountForType(Quat), 4);
	TestEqual(TEXT("color is 4 components"),    ComponentCountForType(Color), 4);
	TestEqual(TEXT("float is not composite"),   ComponentCountForType(Flt), 0);

	struct FCase
	{
		FNiagaraTypeDefinition   Type;
		TSharedPtr<FJsonValue>   Value;
		bool                     bExpectOk;
		const TCHAR*             ExpectedLiteral;   // only meaningful when bExpectOk
		const TCHAR*             Why;
	};

	const FCase Cases[] =
	{
		// --- ACCEPTED: the two spellings the gap measured landing as (0,0,0) -------------------
		{ Vec3, Str(TEXT("1,2,3")),                 true, TEXT("1,2,3"), TEXT("THE reported case — bare comma list, verbatim") },
		{ Vec3, Str(TEXT("(X=1.0,Y=2.0,Z=3.0)")),   true, TEXT("(X=1.0,Y=2.0,Z=3.0)"), TEXT("THE reported case — labelled form, passed through for #32 to parse") },
		{ Vec3, Obj({{TEXT("x"),1},{TEXT("y"),2},{TEXT("z"),3}}), true, TEXT("1,2,3"), TEXT("keyed object — the ONE spelling that used to work") },
		{ Vec3, Obj({{TEXT("X"),1},{TEXT("Y"),2},{TEXT("Z"),3}}), true, TEXT("1,2,3"), TEXT("FString map keys compare case-insensitively") },
		{ Vec3, Arr({1,2,3}),                       true, TEXT("1,2,3"), TEXT("JSON array — also zeroed before, since AsObject() ate it too") },
		{ Vec2, Str(TEXT("12.5,34.25")),            true, TEXT("12.5,34.25"), TEXT("the gap #32 Vector2f case, same decoder") },
		{ Vec2, Obj({{TEXT("x"),1},{TEXT("y"),2}}), true, TEXT("1,2"), TEXT("2-component object stops at 2") },
		{ Vec4, Arr({1,2,3,4}),                     true, TEXT("1,2,3,4"), TEXT("4-component array") },
		{ Quat, Obj({{TEXT("x"),0},{TEXT("y"),0},{TEXT("z"),0},{TEXT("w"),1}}), true, TEXT("0,0,0,1"), TEXT("quat reads the vector labels") },
		{ Color, Obj({{TEXT("r"),1},{TEXT("g"),0},{TEXT("b"),0},{TEXT("a"),1}}), true, TEXT("1,0,0,1"), TEXT("colour reads RGBA first") },
		{ Color, Obj({{TEXT("r"),1},{TEXT("g"),0},{TEXT("b"),0}}), true, TEXT("1,0,0,1"), TEXT("colour alpha is optional and defaults to 1, as before") },
		{ Color, Obj({{TEXT("x"),1},{TEXT("y"),0},{TEXT("z"),0},{TEXT("w"),1}}), true, TEXT("1,0,0,1"), TEXT("colour falls back to XYZW when RGBA is absent") },
		{ Pos,  Str(TEXT("1,2,3")),                 true, TEXT("1,2,3"), TEXT("position takes the same spellings as vec3") },

		// Scalars: numbers keep full float32 precision and integral values stay integral.
		{ Flt, Num(0.97531),        true, TEXT("0.97531"), TEXT("the float that already round-tripped must keep doing so") },
		{ Flt, Num(5.0),            true, TEXT("5"),       TEXT("integral double prints as an integer, not 5.000000") },
		{ Int, Num(2000000001.0),   true, TEXT("2000000001"), TEXT("a large int32 must not be mangled into 2e+09 by %g") },
		{ Flt, Str(TEXT("0.0001")), true, TEXT("0.0001"),  TEXT("a string number is kept VERBATIM — no reformatting, no 3-decimal truncation") },

		// --- REFUSED: each of these used to be accepted and stored as zero ----------------------
		{ Vec3, Obj({{TEXT("x"),1},{TEXT("y"),2}}), false, nullptr, TEXT("a vec3 object missing z must refuse, not default z to 0") },
		{ Vec4, Obj({{TEXT("x"),1},{TEXT("y"),2},{TEXT("z"),3}}), false, nullptr, TEXT("a vec4 object missing w must refuse — w is not optional, only colour alpha is") },
		{ Vec3, Obj({{TEXT("q"),1}}),               false, nullptr, TEXT("an object with no recognised component key names nothing") },
		{ Vec3, Arr({1,2}),                         false, nullptr, TEXT("a short array must refuse, not pad with 0") },
		{ Vec3, Arr({1,2,3,4}),                     false, nullptr, TEXT("a long array must refuse, not truncate") },
		{ Vec3, Str(TEXT("")),                      false, nullptr, TEXT("an empty string encodes no value for any type") },
		{ Vec3, MakeShared<FJsonValueNull>(),       false, nullptr, TEXT("null is not a value") },
		{ Flt,  Obj({{TEXT("x"),1},{TEXT("y"),2}}), false, nullptr, TEXT("component keys are meaningless on a scalar type") },
		{ Flt,  Arr({1,2}),                         false, nullptr, TEXT("an array is meaningless on a scalar type") },
		{ Vec3, Obj({{TEXT("x"),1},{TEXT("y"),2},{TEXT("z"),3},{TEXT("w"),4}}), false, nullptr, TEXT("a 4-key object on a 3-component type must refuse — reading the first 3 would drop w, and #32 cannot see that because '1,2,3' looks correct") },

		// A bool for a vector is accepted HERE and refused by ValidateStackInputLiteral, which is
		// the layer that knows about types: "true" is a legitimate answer to "what string did the
		// caller mean". Asserted so the split stays deliberate — the end result is still a refusal,
		// just one stage later, with the type's own parser giving the reason.
		{ Vec3, MakeShared<FJsonValueBoolean>(true), true, TEXT("true"), TEXT("bool stringifies here; the TYPE check is #32's job") },
	};

	for (const FCase& Case : Cases)
	{
		// Seeded with a sentinel, so "writes nothing on refusal" is checkable.
		FString Literal = TEXT("<<untouched>>");
		FString Error;
		const bool bOk = JsonValueToPinDefaultLiteral(Case.Value, Case.Type, Literal, Error);

		TestEqual(
			FString::Printf(TEXT("JsonValueToPinDefaultLiteral on %s (%s)"), *Case.Type.GetName(), Case.Why),
			bOk, Case.bExpectOk);

		if (Case.bExpectOk)
		{
			TestEqual(FString::Printf(TEXT("literal for %s (%s)"), *Case.Type.GetName(), Case.Why),
				Literal, FString(Case.ExpectedLiteral));
			TestTrue(FString::Printf(TEXT("accepting %s leaves no error text"), Case.Why), Error.IsEmpty());
		}
		else
		{
			// The heart of the fix: a refusal must explain itself and must not invent a value.
			TestTrue(FString::Printf(TEXT("refusing %s writes no literal"), Case.Why), Literal.IsEmpty());
			TestFalse(FString::Printf(TEXT("refusing %s explains itself"), Case.Why), Error.IsEmpty());
		}
	}

	// A string that IS a serialized object still means the object it spells (MCP double-encoding),
	// and a string that merely looks punctuated is NOT mistaken for one.
	{
		FString Literal, Error;
		TestTrue(TEXT("double-serialized object parses"),
			JsonValueToPinDefaultLiteral(Str(TEXT("{\"x\":1,\"y\":2,\"z\":3}")), Vec3, Literal, Error));
		TestEqual(TEXT("double-serialized object yields its components"), Literal, FString(TEXT("1,2,3")));

		Literal.Reset(); Error.Reset();
		TestTrue(TEXT("a labelled literal is NOT treated as JSON"),
			JsonValueToPinDefaultLiteral(Str(TEXT("(X=1,Y=2,Z=3)")), Vec3, Literal, Error));
		TestEqual(TEXT("a labelled literal survives verbatim"), Literal, FString(TEXT("(X=1,Y=2,Z=3)")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraDefaultLiteralRoundTripTest,
	"Monolith.Niagara.DefaultLiteral.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraDefaultLiteralRoundTripTest::RunTest(const FString& /*Parameters*/)
{
	// This is the assertion the gap is really about: not "what string came out" but "what BYTES
	// does the engine's own parser produce from it". The parser is resolved exactly as
	// ApplyPinDefaultLiteral resolves it, and the variable is ALLOCATED — unallocated, every
	// composite SetValueFromPinDefaultString is unable to fail, which is the mechanism behind
	// both #32 and #106.
	FNiagaraEditorModule& NiagaraEditorModule =
		FModuleManager::LoadModuleChecked<FNiagaraEditorModule>(TEXT("NiagaraEditor"));

	auto ParseToVariable = [&NiagaraEditorModule](const FNiagaraTypeDefinition& Type, const FString& Literal,
		FNiagaraVariable& OutVar) -> bool
	{
		TSharedPtr<INiagaraEditorTypeUtilities, ESPMode::ThreadSafe> Utilities =
			NiagaraEditorModule.GetTypeUtilities(Type);
		if (!Utilities.IsValid() || !Utilities->CanHandlePinDefaults())
		{
			return false;
		}
		OutVar = FNiagaraVariable(Type, NAME_None);
		OutVar.AllocateData();
		return OutVar.IsDataAllocated() && Utilities->SetValueFromPinDefaultString(Literal, OutVar);
	};

	// The exact call the gap recorded, end to end: JSON default -> literal -> bytes.
	// If this reads (0,0,0) the bug is back.
	auto CheckVec3 = [this, &ParseToVariable](const TCHAR* Label, const TSharedPtr<FJsonValue>& Value,
		const FVector3f& Expected)
	{
		const FNiagaraTypeDefinition Vec3 = FNiagaraTypeDefinition::GetVec3Def();
		FString Literal, Error;
		if (!TestTrue(FString::Printf(TEXT("%s converts to a literal"), Label),
			JsonValueToPinDefaultLiteral(Value, Vec3, Literal, Error)))
		{
			return;
		}

		FNiagaraVariable Var;
		if (!TestTrue(FString::Printf(TEXT("%s parses through the engine's own utilities"), Label),
			ParseToVariable(Vec3, Literal, Var)))
		{
			return;
		}

		const FVector3f Actual = Var.GetValue<FVector3f>();
		TestTrue(FString::Printf(TEXT("%s stores (%g,%g,%g), not the silent zero"),
			Label, Expected.X, Expected.Y, Expected.Z), Actual.Equals(Expected, UE_KINDA_SMALL_NUMBER));
		// Explicit negative control: the OLD behaviour, asserted as something that must not happen.
		TestFalse(FString::Printf(TEXT("%s is not (0,0,0)"), Label),
			Actual.Equals(FVector3f::ZeroVector, UE_KINDA_SMALL_NUMBER));
	};

	CheckVec3(TEXT("vector default \"1,2,3\""),               Str(TEXT("1,2,3")),               FVector3f(1, 2, 3));
	CheckVec3(TEXT("vector default \"(X=1.0,Y=2.0,Z=3.0)\""), Str(TEXT("(X=1.0,Y=2.0,Z=3.0)")), FVector3f(1, 2, 3));
	CheckVec3(TEXT("vector default {x,y,z}"),                 Obj({{TEXT("x"),1},{TEXT("y"),2},{TEXT("z"),3}}), FVector3f(1, 2, 3));
	CheckVec3(TEXT("vector default [1,2,3]"),                 Arr({1,2,3}),                     FVector3f(1, 2, 3));

	// Vector2f and LinearColor are the two types gap #32 measured as SILENTLY WRONG for the bare
	// comma spelling. They are here because a fix that repairs vec3 and leaves these broken is
	// the same bug with a smaller blast radius.
	{
		const FNiagaraTypeDefinition Vec2 = FNiagaraTypeDefinition::GetVec2Def();
		FString Literal, Error;
		TestTrue(TEXT("vec2 \"12.5,34.25\" converts"),
			JsonValueToPinDefaultLiteral(Str(TEXT("12.5,34.25")), Vec2, Literal, Error));

		// The bare list is NOT parseable by FVector2f::InitFromString (X=/Y= only) — this is the
		// re-labelling half, and it lives in ValidateStackInputLiteral, not here. So assert what
		// this layer owns: the digits survived. The labelled form is what must reach the bytes.
		TestEqual(TEXT("vec2 digits survive verbatim"), Literal, FString(TEXT("12.5,34.25")));

		FNiagaraVariable Var;
		if (ParseToVariable(Vec2, TEXT("(X=12.5,Y=34.25)"), Var))
		{
			const FVector2f Actual = Var.GetValue<FVector2f>();
			TestTrue(TEXT("vec2 labelled form stores (12.5,34.25)"),
				Actual.Equals(FVector2f(12.5f, 34.25f), UE_KINDA_SMALL_NUMBER));
		}
	}

	{
		const FNiagaraTypeDefinition Color = FNiagaraTypeDefinition::GetColorDef();
		FString Literal, Error;
		TestTrue(TEXT("colour object converts"),
			JsonValueToPinDefaultLiteral(Obj({{TEXT("r"),0.9375},{TEXT("g"),0.8125},{TEXT("b"),0.6875},{TEXT("a"),0.5625}}),
				Color, Literal, Error));
		TestEqual(TEXT("colour components in RGBA order"), Literal, FString(TEXT("0.9375,0.8125,0.6875,0.5625")));

		FNiagaraVariable Var;
		if (ParseToVariable(Color, TEXT("(R=0.9375,G=0.8125,B=0.6875,A=0.5625)"), Var))
		{
			const FLinearColor Actual = Var.GetValue<FLinearColor>();
			TestTrue(TEXT("colour labelled form stores the components, not (0,0,0,1)"),
				Actual.Equals(FLinearColor(0.9375f, 0.8125f, 0.6875f, 0.5625f), UE_KINDA_SMALL_NUMBER));
		}
	}

	// NiagaraMatrix4 has no registered pin-default handling (CanHandlePinDefaults() is false), so
	// there is no parser to encode a default with. ApplyPinDefaultLiteral returns false for it and
	// the action reports that instead of claiming a default it never stored (gap #41's rule).
	{
		FNiagaraVariable Var;
		TestFalse(TEXT("matrix has no pin-default parser, so no default can be written"),
			ParseToVariable(FNiagaraTypeDefinition::GetMatrix4Def(), TEXT("1,0,0,0"), Var));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
