// SPDX-License-Identifier: MIT
// Automation tests for the pure name-classification rules behind gaps #83 and #99.
//
// SCOPE, STATED HONESTLY. These cover the DECISION half of both fixes — what a parameter or
// ParameterMapSet pin NAME means. They do NOT cover the graph traversal that finds those names
// (CollectModuleScriptInputs / CollectModuleScriptWrites), because that needs a live
// UNiagaraGraph, a loaded asset and an editor. The traversal must be validated in-editor against
// the two regression fixtures recorded in the staging note:
//   * get_module_script_inputs on /Niagara/Modules/Update/Forces/GravityForce  -> must list Gravity
//   * get_module_output_parameters on a placed stock Collision                 -> must list real writes
//
// The tables below are deliberately built around the OLD, WRONG answers, so every row is a
// negative control: "InputMap" must NOT be an input (it was the bulk of the bogus input_count 2),
// and "OutputMap" must be excluded by TYPE rather than by name (nothing here can exclude it).

#include "Misc/AutomationTest.h"
#include "MonolithNiagaraParameterNames.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace MonolithNiagaraParameterNames;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraModuleInputNameTest,
	"Monolith.Niagara.ParameterNames.ModuleInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraModuleInputNameTest::RunTest(const FString& /*Parameters*/)
{
	struct FCase
	{
		const TCHAR* FullName;
		bool bExpectInput;
		const TCHAR* ExpectedShort;   // only meaningful when bExpectInput
		const TCHAR* Why;
	};

	const FCase Cases[] =
	{
		// The gap #83 headline: this is the parameter GravityForce was omitting.
		{ TEXT("Module.Gravity"),        true,  TEXT("Gravity"),      TEXT("the input stock GravityForce was omitting") },
		{ TEXT("Module.Amp"),            true,  TEXT("Amp"),          TEXT("ordinary module input") },
		{ TEXT("Module.Debug Draw Mode"),true,  TEXT("Debug Draw Mode"), TEXT("spaces are legal in a module input name") },
		{ TEXT("Module.A.B"),            true,  TEXT("A.B"),          TEXT("only the FIRST Module. segment is a namespace") },

		// NEGATIVE CONTROLS — every one of these was, or could be, wrongly counted as an input.
		{ TEXT("InputMap"),              false, TEXT(""),             TEXT("the parameter-map plumbing pin the old walk reported as an input") },
		{ TEXT("OutputMap"),             false, TEXT(""),             TEXT("the Output node's map pin") },
		{ TEXT("Output.Module.Result"),  false, TEXT(""),             TEXT("a module OUTPUT, not an input") },
		{ TEXT("Particles.Position"),    false, TEXT(""),             TEXT("an attribute the module reads/writes, not a settable input") },
		{ TEXT("Transient.PhysicsForce"),false, TEXT(""),             TEXT("transient scratch, not an input") },
		{ TEXT("System.Age"),            false, TEXT(""),             TEXT("engine-provided, not an input") },
		{ TEXT("Coordinate Space"),      false, TEXT(""),             TEXT("a static switch carries no Module. prefix; it is enumerated separately") },
		{ TEXT("Module."),               false, TEXT(""),             TEXT("prefix with an empty leaf names nothing") },
		{ TEXT(""),                      false, TEXT(""),             TEXT("empty") },
		{ TEXT("module.gravity"),        false, TEXT(""),             TEXT("namespaces are case-SENSITIVE; this is a different parameter") },
		{ TEXT("MyModule.Gravity"),      false, TEXT(""),             TEXT("must match the namespace, not merely contain it") },
	};

	for (const FCase& Case : Cases)
	{
		FString Short = TEXT("<untouched>");
		const bool bIsInput = IsModuleInputName(FString(Case.FullName), Short);

		TestEqual(
			FString::Printf(TEXT("IsModuleInputName('%s') (%s)"), Case.FullName, Case.Why),
			bIsInput, Case.bExpectInput);

		if (Case.bExpectInput)
		{
			TestEqual(
				FString::Printf(TEXT("IsModuleInputName('%s') short name"), Case.FullName),
				Short, FString(Case.ExpectedShort));
		}
		else
		{
			// A rejected name must not scribble on the caller's output.
			TestEqual(
				FString::Printf(TEXT("IsModuleInputName('%s') leaves OutShortName untouched"), Case.FullName),
				Short, FString(TEXT("<untouched>")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraModuleWritePinTest,
	"Monolith.Niagara.ParameterNames.ModuleWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraModuleWritePinTest::RunTest(const FString& /*Parameters*/)
{
	const FString CallName = TEXT("MyModule");

	// --- ModuleOutput: "Output.Module.X" is published as "Output.<CallName>.X" -------------
	{
		TArray<FString> Names;
		const EModuleWriteKind Kind = ClassifyModuleWritePin(TEXT("Output.Module.Result"), CallName, EStackContextKind::Particles, Names);
		TestEqual(TEXT("Output.Module.Result is a module output"), (int32)Kind, (int32)EModuleWriteKind::ModuleOutput);
		TestEqual(TEXT("Output.Module.Result resolves to exactly one name"), Names.Num(), 1);
		if (Names.Num() == 1)
		{
			TestEqual(TEXT("Output.Module.Result is renamed onto the placed call"), Names[0], FString(TEXT("Output.MyModule.Result")));
		}
	}

	// --- ModuleLocal: real write, but NOT addressable from outside --------------------------
	{
		TArray<FString> Names;
		const EModuleWriteKind Kind = ClassifyModuleWritePin(TEXT("Module.Scratch"), CallName, EStackContextKind::Particles, Names);
		TestEqual(TEXT("Module.Scratch is module-local"), (int32)Kind, (int32)EModuleWriteKind::ModuleLocal);
		TestEqual(TEXT("a module-local write exposes no caller-addressable name"), Names.Num(), 0);
	}

	// --- StackContext resolves to BOTH spellings, per stage ---------------------------------
	{
		struct FCtxCase { EStackContextKind Ctx; const TCHAR* Expected; const TCHAR* Why; };
		const FCtxCase CtxCases[] =
		{
			{ EStackContextKind::Particles, TEXT("Particles.Position"), TEXT("particle stage") },
			{ EStackContextKind::Emitter,   TEXT("Emitter.Position"),   TEXT("emitter stage") },
			{ EStackContextKind::System,    TEXT("System.Position"),    TEXT("system stage") },
		};

		for (const FCtxCase& Case : CtxCases)
		{
			TArray<FString> Names;
			const EModuleWriteKind Kind = ClassifyModuleWritePin(TEXT("StackContext.Position"), CallName, Case.Ctx, Names);
			TestEqual(FString::Printf(TEXT("StackContext.Position is a stack-context write (%s)"), Case.Why),
				(int32)Kind, (int32)EModuleWriteKind::StackContext);
			TestEqual(FString::Printf(TEXT("StackContext.Position resolves to two names (%s)"), Case.Why), Names.Num(), 2);
			if (Names.Num() == 2)
			{
				TestEqual(FString::Printf(TEXT("StackContext.Position keeps its own spelling (%s)"), Case.Why),
					Names[0], FString(TEXT("StackContext.Position")));
				TestEqual(FString::Printf(TEXT("StackContext.Position resolves against the stage (%s)"), Case.Why),
					Names[1], FString(Case.Expected));
			}
		}
	}

	// --- Direct: written under its own name --------------------------------------------------
	{
		const TCHAR* DirectNames[] = { TEXT("Particles.Position"), TEXT("Transient.PhysicsForce"), TEXT("Emitter.SpawnRate") };
		for (const TCHAR* N : DirectNames)
		{
			TArray<FString> Names;
			const EModuleWriteKind Kind = ClassifyModuleWritePin(N, CallName, EStackContextKind::Particles, Names);
			TestEqual(FString::Printf(TEXT("'%s' is a direct write"), N), (int32)Kind, (int32)EModuleWriteKind::Direct);
			TestEqual(FString::Printf(TEXT("'%s' resolves to one name"), N), Names.Num(), 1);
			if (Names.Num() == 1)
			{
				TestEqual(FString::Printf(TEXT("'%s' is written under its own name"), N), Names[0], FString(N));
			}
		}
	}

	// --- NotAWrite: the editor's affordances, not parameters ---------------------------------
	{
		const TCHAR* NonWrites[] = { TEXT("Add"), TEXT(""), TEXT("Output.Module."), TEXT("StackContext.") };
		for (const TCHAR* N : NonWrites)
		{
			TArray<FString> Names;
			const EModuleWriteKind Kind = ClassifyModuleWritePin(N, CallName, EStackContextKind::Particles, Names);
			TestEqual(FString::Printf(TEXT("'%s' is not a write"), N), (int32)Kind, (int32)EModuleWriteKind::NotAWrite);
			TestEqual(FString::Printf(TEXT("'%s' resolves to no names"), N), Names.Num(), 0);
		}
	}

	// --- The map pin is excluded by TYPE, never by name ---------------------------------------
	// Documenting the boundary: "OutputMap" is a perfectly ordinary NAME. What keeps the parameter
	// map out of the outputs list is the FNiagaraTypeDefinition check in CollectModuleScriptWrites.
	// If that check is ever dropped, gap #99's original symptom returns and this rule will not
	// catch it — which is why the in-editor Collision regression check is not optional.
	{
		TArray<FString> Names;
		const EModuleWriteKind Kind = ClassifyModuleWritePin(TEXT("OutputMap"), CallName, EStackContextKind::Particles, Names);
		TestEqual(TEXT("'OutputMap' is classified by name alone as a direct write"), (int32)Kind, (int32)EModuleWriteKind::Direct);
	}

	// --- Ordering: "Output.Module.X" must win over the "Module." rule -------------------------
	{
		TArray<FString> Names;
		const EModuleWriteKind Kind = ClassifyModuleWritePin(TEXT("Output.Module.X"), CallName, EStackContextKind::Particles, Names);
		TestNotEqual(TEXT("Output.Module.X is not mistaken for a module-local write"), (int32)Kind, (int32)EModuleWriteKind::ModuleLocal);
	}

	// --- OutNames is always reset, so a stale caller array cannot leak into an answer ---------
	{
		TArray<FString> Names;
		Names.Add(TEXT("stale"));
		ClassifyModuleWritePin(TEXT("Module.Scratch"), CallName, EStackContextKind::Particles, Names);
		TestEqual(TEXT("ClassifyModuleWritePin resets OutNames"), Names.Num(), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
