// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"
#include "NiagaraScript.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

/**
 * Decoding FNiagaraModuleDependency::OnlyEvaluateInScriptUsage — the bitmask that says WHERE a
 * declared stack dependency is actually enforced.
 *
 * WHY THIS IS ITS OWN FILE, AND WHY IT IS TESTED.
 * The field is declared (NiagaraScript.h:148-149) as
 *
 *     meta = (Bitmask, BitmaskEnum = "/Script/Niagara.ENiagaraModuleDependencyUsage")
 *     int32 OnlyEvaluateInScriptUsage;
 *
 * and the enum it indexes (NiagaraScript.h:108-120) opens with a HIDDEN sentinel:
 *
 *     None UMETA(Hidden),   // index 0 — "Default entry to catch invalid usages"
 *     Spawn,                // index 1
 *     Update,               // index 2
 *     Event,                // index 3
 *     SimulationStage       // index 4
 *
 * 🛑 **So the first REAL usage lives at bit 1, not bit 0.** The engine's own default constructor
 * (NiagaraScript.h:159-162) confirms the shift by writing
 * `1 << (int32)Spawn | 1 << (int32)Update | 1 << (int32)Event | 1 << (int32)SimulationStage`
 * — i.e. 0b11110 == 30, with bit 0 never set.
 *
 * The obvious hand-rolled decoder ("bit 0 is the first entry") is therefore off by one, and it
 * fails in the worst possible way: it still produces a plausible-looking list. Stock GravityForce
 * declares Spawn|Update|Event (0b1110 == 14); a shifted decoder renders that as
 * None|Spawn|Update and reports a dependency enforced in a phase it is not. **That would be a
 * silent-wrong-answer about the exact thing this reader exists to explain — why a module must sit
 * above the solver, and why the offered fix differs between Spawn and Update.**
 *
 * The decode is driven off StaticEnum rather than a hardcoded table, so it stays correct if Epic
 * adds a usage; the unit test pins the bit POSITIONS against the engine's own default mask so a
 * silent re-ordering cannot pass unnoticed.
 *
 * Unit-tested in Private/Tests/MonolithNiagaraDependencyUsageTest.cpp.
 */
namespace MonolithNiagaraDependencyUsage
{
	/**
	 * Readable names for every usage bit set in Bitmask.
	 *
	 * Bit N corresponds to enum VALUE N — never to the Nth visible entry. The `None` sentinel is
	 * skipped because it is `UMETA(Hidden)` and means "invalid", not a phase; if its bit is somehow
	 * set, that is reported through UnexpectedBits rather than as a usage.
	 */
	inline TArray<FString> DecodeUsageBitmask(int32 Bitmask, int32* OutUnexpectedBits = nullptr)
	{
		TArray<FString> Usages;
		int32 Recognised = 0;

		if (const UEnum* UsageEnum = StaticEnum<ENiagaraModuleDependencyUsage>())
		{
			// NumEnums() includes the implicit _MAX entry, hence the -1.
			const int32 EntryCount = UsageEnum->NumEnums() - 1;
			for (int32 Index = 0; Index < EntryCount; ++Index)
			{
				const int64 Value = UsageEnum->GetValueByIndex(Index);
				if (Value < 0 || Value >= 32)
				{
					continue;
				}
				if (static_cast<ENiagaraModuleDependencyUsage>(Value) == ENiagaraModuleDependencyUsage::None)
				{
					continue;   // the hidden "invalid" sentinel, never a phase
				}

				const int32 Bit = 1 << static_cast<int32>(Value);
				Recognised |= Bit;
				if ((Bitmask & Bit) != 0)
				{
					Usages.Add(UsageEnum->GetNameStringByIndex(Index));
				}
			}
		}

		if (OutUnexpectedBits)
		{
			// Anything set that no visible enum entry claims — including bit 0. Reported rather than
			// dropped: a bit we cannot name is a fact about the asset, not noise.
			*OutUnexpectedBits = Bitmask & ~Recognised;
		}
		return Usages;
	}

	/**
	 * The mask the engine's FNiagaraModuleDependency constructor installs — every real phase and
	 * nothing else. Used by the test as an independent cross-check on the bit positions, and by the
	 * reader to tell "explicitly set to everything" from "left at the default".
	 */
	inline int32 AllUsagesMask()
	{
		return (1 << static_cast<int32>(ENiagaraModuleDependencyUsage::Spawn))
			 | (1 << static_cast<int32>(ENiagaraModuleDependencyUsage::Update))
			 | (1 << static_cast<int32>(ENiagaraModuleDependencyUsage::Event))
			 | (1 << static_cast<int32>(ENiagaraModuleDependencyUsage::SimulationStage));
	}
}
