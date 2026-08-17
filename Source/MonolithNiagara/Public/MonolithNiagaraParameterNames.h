// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"

/**
 * Pure NAME-classification rules shared by the Niagara parameter readers.
 *
 * WHY THIS FILE EXISTS (gaps #83 and #99).
 * Both gaps were the same conceptual error made twice: an action asked a NAMED NODE what a
 * module exposes, when Epic keeps that information on the PARAMETER MAP.
 *
 *   * get_module_script_inputs enumerated UNiagaraNodeInput nodes. A module's inputs are not
 *     input nodes at all — they are Module.* parameters READ out of a ParameterMapGet. On stock
 *     GravityForce that answered input_count 2 (the InputMap plumbing pin plus the Coordinate
 *     Space static switch) and omitted Module.Gravity entirely, with no warning.
 *   * get_module_output_parameters enumerated the Output node's pins. A module's outputs are the
 *     WRITE pins on its ParameterMapSet; the Output node of a module script carries only the map
 *     itself, so the action reported output_count 1 / OutputMap for every module ever passed to
 *     it — including Epic's own Collision — and that reads like a real answer.
 *
 * The traversal that finds those pins needs a live UNiagaraGraph. The DECISION about what a pin
 * name means does not, so it lives here: pure, header-inline, and unit-testable with no editor,
 * no asset and no graph (see Private/Tests/MonolithNiagaraParameterNamesTest.cpp).
 *
 * These rules are the ones BuildStackWriterIndex has always applied when attributing stack
 * writes; they were lifted out rather than copied so the writer index and the module-output
 * reader cannot drift apart — the same lesson gap #77 taught about rename_user_parameter.
 */
namespace MonolithNiagaraParameterNames
{
	/** Which context a StackContext.* write resolves into, decided by the stage that runs it. */
	enum class EStackContextKind : uint8
	{
		Particles,
		Emitter,
		System
	};

	/** What a ParameterMapSet write pin's NAME means. */
	enum class EModuleWriteKind : uint8
	{
		/** "Module.X" — module-local scratch. Real, but not addressable from outside the module. */
		ModuleLocal,
		/** "Output.Module.X" — published to callers as "Output.<CallName>.X". */
		ModuleOutput,
		/** "StackContext.X" — resolves to BOTH "StackContext.X" and "<Particles|Emitter|System>.X". */
		StackContext,
		/** Anything else ("Particles.Position", "Transient.PhysicsForce", ...) — written as itself. */
		Direct,
		/** The map spine, the dynamic "Add" pin, or an unnamed pin. Not a write. */
		NotAWrite
	};

	/**
	 * True when FullName is a module INPUT, i.e. lives in the Module.* namespace.
	 * OutShortName receives the name with the prefix stripped, which is what the stack UI shows
	 * and what set_module_input_value expects.
	 *
	 * Deliberately case-SENSITIVE: Niagara namespaces are case-sensitive, and "module.x" is a
	 * different parameter from "Module.X" rather than a spelling of it.
	 */
	inline bool IsModuleInputName(const FString& FullName, FString& OutShortName)
	{
		// "Output.Module.X" is a module OUTPUT and must not be mistaken for an input; it fails
		// this test because it starts with "Output.", not "Module.".
		if (!FullName.StartsWith(TEXT("Module."), ESearchCase::CaseSensitive))
		{
			return false;
		}
		const FString Leaf = FullName.Mid(7); // len("Module.")
		if (Leaf.IsEmpty())
		{
			return false;
		}
		OutShortName = Leaf;
		return true;
	}

	/** Convenience overload for callers that only need the yes/no. */
	inline bool IsModuleInputName(const FString& FullName)
	{
		FString Unused;
		return IsModuleInputName(FullName, Unused);
	}

	/**
	 * Classify one ParameterMapSet write pin by name and produce the parameter name(s) it writes.
	 *
	 * OutNames is emptied first and is left EMPTY for ModuleLocal and NotAWrite — a module-local
	 * write is a real write that simply has no externally addressable name, and callers must
	 * report that distinction rather than dropping the pin silently.
	 *
	 * CallName is the PLACED module's function name, which is what turns "Output.Module.X" into
	 * the caller-visible "Output.<CallName>.X". Pass an empty CallName only when no placed call is
	 * known; the prefix then degrades to "Output..X", so callers should treat an empty CallName as
	 * a reason to report the raw pin name instead.
	 */
	inline EModuleWriteKind ClassifyModuleWritePin(
		const FString& PinName,
		const FString& CallName,
		EStackContextKind Context,
		TArray<FString>& OutNames)
	{
		OutNames.Reset();

		// The dynamic "Add" pin is the editor's affordance for creating a new write, never a
		// write itself. An unnamed pin cannot address anything.
		if (PinName.IsEmpty() || PinName == TEXT("Add"))
		{
			return EModuleWriteKind::NotAWrite;
		}

		// Order matters: "Output.Module." must be tested before "Module." would ever match, and
		// before the Direct fallback claims it as a plain "Output.*" name.
		if (PinName.StartsWith(TEXT("Output.Module."), ESearchCase::CaseSensitive))
		{
			const FString Leaf = PinName.Mid(14); // len("Output.Module.")
			if (Leaf.IsEmpty())
			{
				return EModuleWriteKind::NotAWrite;
			}
			OutNames.Add(FString::Printf(TEXT("Output.%s.%s"), *CallName, *Leaf));
			return EModuleWriteKind::ModuleOutput;
		}

		if (PinName.StartsWith(TEXT("Module."), ESearchCase::CaseSensitive))
		{
			return EModuleWriteKind::ModuleLocal;
		}

		if (PinName.StartsWith(TEXT("StackContext."), ESearchCase::CaseSensitive))
		{
			const FString Leaf = PinName.Mid(13); // len("StackContext.")
			if (Leaf.IsEmpty())
			{
				return EModuleWriteKind::NotAWrite;
			}
			// Both spellings are real: the module writes StackContext.X, and the stage it runs in
			// makes that the same parameter as <Context>.X for every reader downstream.
			OutNames.Add(PinName);
			const TCHAR* Prefix =
				(Context == EStackContextKind::System)  ? TEXT("System")  :
				(Context == EStackContextKind::Emitter) ? TEXT("Emitter") : TEXT("Particles");
			OutNames.Add(FString::Printf(TEXT("%s.%s"), Prefix, *Leaf));
			return EModuleWriteKind::StackContext;
		}

		OutNames.Add(PinName);
		return EModuleWriteKind::Direct;
	}

	/** Human-readable kind, used in the readers' JSON so a caller can see WHY a name resolved. */
	inline const TCHAR* LexToStringWriteKind(EModuleWriteKind Kind)
	{
		switch (Kind)
		{
		case EModuleWriteKind::ModuleLocal:  return TEXT("module_local");
		case EModuleWriteKind::ModuleOutput: return TEXT("module_output");
		case EModuleWriteKind::StackContext: return TEXT("stack_context");
		case EModuleWriteKind::Direct:       return TEXT("direct");
		default:                             return TEXT("not_a_write");
		}
	}
}
