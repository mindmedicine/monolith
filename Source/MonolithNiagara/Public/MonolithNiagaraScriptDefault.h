// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"
#include "NiagaraCommon.h"      // ENiagaraDefaultMode
#include "NiagaraTypes.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraEditorModule.h"
#include "INiagaraEditorTypeUtilities.h"
#include "Modules/ModuleManager.h"

/**
 * Rendering a module script parameter's DECLARED DEFAULT as a string.
 *
 * 🛑 WHY THIS IS ITS OWN FILE. UNiagaraScriptVariable keeps the default in TWO places, and picking
 * the wrong one returns ZEROS while reporting success:
 *
 *   - `Variable` (NiagaraScriptVariable.h:162) — a public UPROPERTY holding type, name AND data.
 *     **This is the authoritative store.** UNiagaraNodeParameterMapGet::CreateDefaultPin populates
 *     a default pin from `Graph->GetVariable(Var)` piped through GetPinDefaultStringFromValue
 *     (NiagaraNodeParameterMapGet.cpp:130-143), and UNiagaraGraph::GetVariable returns
 *     `ScriptVariable->Variable` (NiagaraGraph.cpp:4314-4327). So this is what the editor renders.
 *
 *   - `DefaultValueVariant` (NiagaraScriptVariable.h:248) — PRIVATE, reachable only through
 *     GetDefaultValueData() (:194-201). It is allocated ZERO-FILLED by the private AllocateData()
 *     (:230-238) and populated ONLY by SetDefaultValueData (:186-192). A script variable whose
 *     default arrived by any other route therefore has a zeroed variant alongside a correct
 *     `Variable`.
 *
 * Monolith's reset_module_input_to_default read the VARIANT, and so reported stock SphereLocation's
 * `Sphere Radius` as "0.000000" against a compiled `float Constant64 = 100;`, and
 * `Non Uniform Scale` as "0.000,0.000,0.000" against a real (1,1,1) — both with success and a
 * confident `effective_value_source: "script_default"`. Measured 2026-08-18; same defect class as
 * gap #106 (a decoder answering 0 for everything it could not read). On a RECOVERY action it is
 * worse than elsewhere: someone resetting a corrupted input reads 0 and concludes the reset
 * destroyed the value.
 *
 * ⚠️ The obvious "just ask the engine" calls are NOT available: UNiagaraGraph::GetVariable and
 * ::GetDefaultMode carry no export macro (NiagaraGraph.h:503/505) and would fail to link.
 * GetScriptVariable(FName) (:373) IS NIAGARAEDITOR_API, and `Variable` is public, so the same data
 * is reached through the exported door.
 *
 * CONTRACT: absence is ABSENCE. Every path that cannot produce a real declared default returns
 * false with a reason; nothing is ever default-constructed and stringified into a zero.
 *
 * Unit-tested in Private/Tests/MonolithNiagaraScriptDefaultTest.cpp.
 */
namespace MonolithNiagaraScriptDefault
{
	/** Which store a reported default came from, or why none could be reported. */
	struct FDefaultReadResult
	{
		/** The rendered default. Only meaningful when the read returned true. */
		FString Value;

		/** "script_variable_default" | "script_variable_default_value_variant" */
		FString Source;

		/** Plain-language reason the default could not be read. Set only on failure. */
		FString AbsenceReason;

		/** Set when BOTH stores hold a value and they differ — a real inconsistency in the asset. */
		FString Disagreement;
	};

	/**
	 * Render a script variable's declared default.
	 *
	 * @param ScriptVariable  the module graph's parameter entry (may be null)
	 * @param InputType       the type the CALLER expects; a mismatch is refused rather than
	 *                        rendered, because GetScriptVariable(FName) matches on name alone and a
	 *                        same-named parameter of another type would be rendered at the wrong width
	 * @return true only when Out.Value holds a genuine declared default
	 */
	inline bool TryRenderDeclaredDefault(const UNiagaraScriptVariable* ScriptVariable,
		const FNiagaraTypeDefinition& InputType, FDefaultReadResult& Out)
	{
		Out = FDefaultReadResult();

		auto Absent = [&Out](const TCHAR* Reason) -> bool
		{
			Out.AbsenceReason = Reason;
			return false;
		};

		if (!ScriptVariable)
		{
			return Absent(TEXT("the module script declares no parameter with this name"));
		}

		if (ScriptVariable->DefaultMode != ENiagaraDefaultMode::Value)
		{
			// Binding and Custom defaults are resolved at compile time and have NO single literal.
			const UEnum* ModeEnum = StaticEnum<ENiagaraDefaultMode>();
			Out.AbsenceReason = FString::Printf(
				TEXT("the parameter's default mode is '%s', not Value — such a default is resolved at compile time "
					 "and has no single literal"),
				ModeEnum ? *ModeEnum->GetNameStringByValue(static_cast<int64>(ScriptVariable->DefaultMode))
						 : TEXT("non-Value"));
			return false;
		}

		if (ScriptVariable->Variable.GetType() != InputType)
		{
			return Absent(TEXT("the module script's parameter of this name has a different type"));
		}

		FNiagaraEditorModule& NiagaraEditorModule =
			FModuleManager::LoadModuleChecked<FNiagaraEditorModule>(TEXT("NiagaraEditor"));
		TSharedPtr<INiagaraEditorTypeUtilities, ESPMode::ThreadSafe> TypeUtilities =
			NiagaraEditorModule.GetTypeUtilities(InputType);
		if (!TypeUtilities.IsValid() || !TypeUtilities->CanHandlePinDefaults())
		{
			return Absent(TEXT("this type has no pin-default utilities, so its default cannot be rendered as a string"));
		}

		// PRIMARY — the store the engine's own default-pin population reads.
		// ⚠️ IsDataAllocated() is checked FIRST: every GetPinDefaultStringFromValue implementation
		// opens with checkf(IsDataAllocated()), so calling it unguarded takes the EDITOR DOWN rather
		// than returning a bad string.
		FString PrimaryValue;
		bool bHavePrimary = false;
		if (ScriptVariable->Variable.IsValid() && ScriptVariable->Variable.IsDataAllocated())
		{
			PrimaryValue = TypeUtilities->GetPinDefaultStringFromValue(ScriptVariable->Variable);
			bHavePrimary = !PrimaryValue.IsEmpty();
		}

		// SECONDARY — the variant, read ONLY as a cross-check. Never a silent stand-in for the
		// primary: a zeroed variant is precisely what caused the defect this file exists to prevent.
		FString VariantValue;
		bool bHaveVariant = false;
		if (const uint8* VariantData = ScriptVariable->GetDefaultValueData())
		{
			FNiagaraVariable VariantVar(InputType, ScriptVariable->Variable.GetName());
			VariantVar.AllocateData();
			VariantVar.SetData(VariantData);
			VariantValue = TypeUtilities->GetPinDefaultStringFromValue(VariantVar);
			bHaveVariant = !VariantValue.IsEmpty();
		}

		if (bHavePrimary)
		{
			Out.Value = PrimaryValue;
			Out.Source = TEXT("script_variable_default");
			if (bHaveVariant && VariantValue != PrimaryValue)
			{
				Out.Disagreement = FString::Printf(
					TEXT("the module script's two default stores DISAGREE — UNiagaraScriptVariable::Variable holds "
						 "'%s' (reported, and the store the editor renders from) while DefaultValueVariant holds '%s'"),
					*PrimaryValue, *VariantValue);
			}
			return true;
		}

		if (bHaveVariant)
		{
			// Unusual shape: variant populated while Variable is not allocated. Reported, but the
			// source says which store it came from so it is never mistaken for what the editor shows.
			Out.Value = VariantValue;
			Out.Source = TEXT("script_variable_default_value_variant");
			return true;
		}

		return Absent(TEXT("the module script declares the parameter but holds no allocated default data in either "
						   "store, so there is no default to report"));
	}
}
