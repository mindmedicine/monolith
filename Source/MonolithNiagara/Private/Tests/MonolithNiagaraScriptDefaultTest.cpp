// SPDX-License-Identifier: MIT
// Automation tests for reset_module_input_to_default's `effective_value` — specifically, WHICH of
// UNiagaraScriptVariable's two default stores it reads.
//
// WHAT SHIPPED BROKEN, AND WHY NO TEST CAUGHT IT. The reader called GetDefaultValueData(), i.e. the
// private zero-filled `DefaultValueVariant`, and returned TRUE with the zeros. Stock SphereLocation's
// `Sphere Radius` came back "0.000000" against a compiled `float Constant64 = 100;`, and
// `Non Uniform Scale` came back "0.000,0.000,0.000" against a real (1,1,1). Because the reader
// returned true, the handler's own "never report absence as 0" guard never fired.
//
// The two fixtures below are exactly those two inputs, reconstructed as real UNiagaraScriptVariable
// objects: a NON-ZERO scalar default and a NON-UNIT vector default. A test using a default of 0 or a
// zero vector would have passed against the broken code — which is precisely why the values matter.
//
// 🛑 SCOPE. These pin the store selection and the absence contract. Resolving the script variable
// from a placed module node still needs a live UNiagaraSystem and must be validated in-editor.

#include "Misc/AutomationTest.h"
#include "MonolithNiagaraScriptDefault.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraTypes.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"   // GetTransientPackage / NewObject

#if WITH_DEV_AUTOMATION_TESTS

using namespace MonolithNiagaraScriptDefault;

namespace
{
	/**
	 * A script variable in the shape the defect actually occurs in: the real default lives in
	 * `Variable`, and `DefaultValueVariant` is left as AllocateData() leaves it — ZEROED. This is
	 * not a contrived case; it is what SphereLocation's parameters look like.
	 */
	template <typename TValue>
	UNiagaraScriptVariable* MakeScriptVariable(const FNiagaraTypeDefinition& Type, const TCHAR* Name, const TValue& Value)
	{
		UNiagaraScriptVariable* ScriptVariable = NewObject<UNiagaraScriptVariable>(GetTransientPackage());
		ScriptVariable->DefaultMode = ENiagaraDefaultMode::Value;
		ScriptVariable->Variable = FNiagaraVariable(Type, FName(Name));
		ScriptVariable->Variable.AllocateData();

		// SetData rather than SetValue<T>: FNiagaraVariable::SetValue opens with
		// check(sizeof(T) == TypeDefHandle->GetSize()) (NiagaraTypes.h:1512-1517), which would ABORT
		// the process on a size mismatch instead of failing an assertion. Copying under an explicit
		// size guard keeps a future LWC/type change a readable TEST FAILURE (see the size assertions
		// in each row) rather than a crashed automation run.
		if (ScriptVariable->Variable.GetSizeInBytes() == sizeof(TValue))
		{
			ScriptVariable->Variable.SetData(reinterpret_cast<const uint8*>(&Value));
		}
		return ScriptVariable;
	}
}

// ============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraScriptDefaultStoreTest,
	"Monolith.Niagara.ScriptDefault.StoreSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraScriptDefaultStoreTest::RunTest(const FString& /*Parameters*/)
{
	// --- ROW 1: the Sphere Radius fixture — a NON-ZERO scalar default ------------------------
	// CATCHES the shipped defect head-on: reading DefaultValueVariant instead of Variable. The
	// variant here is zeroed (nothing ever called SetDefaultValueData), so the broken reader
	// produces "0.000000" and reports success. Asserting the string CONTAINS "100" rather than
	// equalling it keeps the test about the STORE, not about float formatting.
	{
		UNiagaraScriptVariable* Radius = MakeScriptVariable<float>(
			FNiagaraTypeDefinition::GetFloatDef(), TEXT("Module.Sphere Radius"), 100.0f);

		// Guards the fixture itself: if Niagara's float were ever not 4 bytes the helper above would
		// silently write nothing and every assertion below would be meaningless.
		TestEqual(TEXT("fixture sanity: Niagara float is 4 bytes"),
			Radius->Variable.GetSizeInBytes(), static_cast<int32>(sizeof(float)));

		FDefaultReadResult Result;
		const bool bRead = TryRenderDeclaredDefault(Radius, FNiagaraTypeDefinition::GetFloatDef(), Result);

		TestTrue(TEXT("a declared float default IS readable"), bRead);
		TestTrue(FString::Printf(TEXT("the default reads as 100, not as 0 (got '%s')"), *Result.Value),
			Result.Value.Contains(TEXT("100")));
		TestFalse(TEXT("the reported default is not a zero"), Result.Value.StartsWith(TEXT("0")));
		TestEqual(TEXT("and it is attributed to the store the editor renders from"),
			Result.Source, FString(TEXT("script_variable_default")));
	}

	// --- ROW 2: the Non Uniform Scale fixture — a NON-UNIT vector default --------------------
	// CATCHES the same defect on a multi-component type, where the broken output
	// "0.000,0.000,0.000" was especially plausible. It also catches a reader that renders only the
	// first component, or that mixes up component order: (1,2,3) is asymmetric on purpose, so a
	// transposed or truncated render cannot pass.
	{
		UNiagaraScriptVariable* Scale = MakeScriptVariable<FVector3f>(
			FNiagaraTypeDefinition::GetVec3Def(), TEXT("Module.Non Uniform Scale"), FVector3f(1.0f, 2.0f, 3.0f));

		// Niagara's Vec3 is the SWC FVector3f, not the LWC FVector — the engine's own alias is
		// `Vector3fStruct = GetVec3Struct()` (NiagaraTypes.cpp:421). If that ever changes, this
		// assertion says so instead of the rows below failing for an unrelated-looking reason.
		TestEqual(TEXT("fixture sanity: Niagara Vec3 is FVector3f (12 bytes)"),
			Scale->Variable.GetSizeInBytes(), static_cast<int32>(sizeof(FVector3f)));

		FDefaultReadResult Result;
		const bool bRead = TryRenderDeclaredDefault(Scale, FNiagaraTypeDefinition::GetVec3Def(), Result);

		TestTrue(TEXT("a declared vector default IS readable"), bRead);
		TestTrue(FString::Printf(TEXT("component X survives (got '%s')"), *Result.Value), Result.Value.Contains(TEXT("1")));
		TestTrue(FString::Printf(TEXT("component Y survives (got '%s')"), *Result.Value), Result.Value.Contains(TEXT("2")));
		TestTrue(FString::Printf(TEXT("component Z survives (got '%s')"), *Result.Value), Result.Value.Contains(TEXT("3")));
		TestEqual(TEXT("attributed to the Variable store"), Result.Source, FString(TEXT("script_variable_default")));
	}

	// --- ROW 3: a genuine zero must still read as zero ---------------------------------------
	// The counterweight. CATCHES an over-correction that treats 0 as "unreadable" — after a bug
	// about fabricated zeros, refusing all zeros is the tempting wrong fix, and it would break
	// every input whose default really is 0.
	{
		UNiagaraScriptVariable* Zero = MakeScriptVariable<float>(
			FNiagaraTypeDefinition::GetFloatDef(), TEXT("Module.Genuinely Zero"), 0.0f);

		FDefaultReadResult Result;
		const bool bRead = TryRenderDeclaredDefault(Zero, FNiagaraTypeDefinition::GetFloatDef(), Result);

		TestTrue(TEXT("a real zero default is still reported, not suppressed"), bRead);
		TestTrue(FString::Printf(TEXT("and it reads as zero (got '%s')"), *Result.Value),
			Result.Value.Contains(TEXT("0")));
	}

	return true;
}

// ============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraScriptDefaultAbsenceTest,
	"Monolith.Niagara.ScriptDefault.AbsenceIsAbsence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraScriptDefaultAbsenceTest::RunTest(const FString& /*Parameters*/)
{
	// --- ROW 1: no script variable at all -----------------------------------------------------
	// CATCHES the core contract violation — answering a value for something that does not exist.
	// The handler's "never report absence as 0" guard is downstream of this boolean; if this
	// returns true, the guard is unreachable and a fabricated value ships silently. That is exactly
	// how the original defect escaped.
	{
		FDefaultReadResult Result;
		const bool bRead = TryRenderDeclaredDefault(nullptr, FNiagaraTypeDefinition::GetFloatDef(), Result);

		TestFalse(TEXT("a missing script variable yields NO value"), bRead);
		TestTrue(TEXT("the value is empty, not a synthesised zero"), Result.Value.IsEmpty());
		TestFalse(TEXT("and a reason is given, so the caller can be told WHY"), Result.AbsenceReason.IsEmpty());
	}

	// --- ROW 2: a Binding default ------------------------------------------------------------
	// A Binding default resolves at compile time and has no literal. CATCHES a reader that renders
	// the (meaningless, probably zeroed) data buffer of a non-Value parameter — which would state a
	// confident number for an input whose value is decided by something else entirely.
	{
		UNiagaraScriptVariable* Bound = NewObject<UNiagaraScriptVariable>(GetTransientPackage());
		Bound->DefaultMode = ENiagaraDefaultMode::Binding;
		Bound->Variable = FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Module.Bound"));
		Bound->Variable.AllocateData();
		Bound->Variable.SetValue<float>(7.0f);

		FDefaultReadResult Result;
		const bool bRead = TryRenderDeclaredDefault(Bound, FNiagaraTypeDefinition::GetFloatDef(), Result);

		TestFalse(TEXT("a Binding default is NOT reported as a literal"), bRead);
		TestTrue(TEXT("no value is emitted even though the buffer holds bytes"), Result.Value.IsEmpty());
		TestTrue(TEXT("the reason names the default mode"), Result.AbsenceReason.Contains(TEXT("Binding")));
	}

	// --- ROW 3: a type mismatch ---------------------------------------------------------------
	// GetScriptVariable(FName) matches on NAME ALONE, so a same-named parameter of another type can
	// be handed in. CATCHES rendering it anyway: reading a float's 4 bytes as a Vec3 would produce a
	// plausible-looking triple built partly from unrelated memory.
	{
		UNiagaraScriptVariable* Radius = MakeScriptVariable<float>(
			FNiagaraTypeDefinition::GetFloatDef(), TEXT("Module.Sphere Radius"), 100.0f);

		FDefaultReadResult Result;
		const bool bRead = TryRenderDeclaredDefault(Radius, FNiagaraTypeDefinition::GetVec3Def(), Result);

		TestFalse(TEXT("a type mismatch is refused, not rendered at the wrong width"), bRead);
		TestTrue(TEXT("no value is emitted"), Result.Value.IsEmpty());
	}

	// --- ROW 4: declared but never allocated --------------------------------------------------
	// Neither store holds data. CATCHES the exact fabrication shape: default-constructing a
	// variable of the right type and stringifying it, which yields a confident "0.000000" for an
	// input whose default is simply unknown to us.
	{
		UNiagaraScriptVariable* Empty = NewObject<UNiagaraScriptVariable>(GetTransientPackage());
		Empty->DefaultMode = ENiagaraDefaultMode::Value;
		Empty->Variable = FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Module.Unallocated"));
		// deliberately NOT AllocateData()

		FDefaultReadResult Result;
		const bool bRead = TryRenderDeclaredDefault(Empty, FNiagaraTypeDefinition::GetFloatDef(), Result);

		TestFalse(TEXT("an unallocated default is absent, not zero"), bRead);
		TestTrue(TEXT("no value is emitted"), Result.Value.IsEmpty());
		TestFalse(TEXT("a reason is given"), Result.AbsenceReason.IsEmpty());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
