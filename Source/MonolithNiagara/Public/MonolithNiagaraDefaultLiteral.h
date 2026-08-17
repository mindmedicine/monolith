// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "NiagaraTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * Turning a caller-supplied JSON `default` into a pin-default LITERAL string, or refusing.
 *
 * WHY THIS FILE EXISTS (gap #106).
 * add_user_parameter decoded a composite `default` like this:
 *
 *     TSharedPtr<FJsonObject> O = AsObjectOrParseString(DefaultJV);
 *     if (O.IsValid())
 *     {
 *         FVector3f V(O->GetNumberField(TEXT("x")), O->GetNumberField(TEXT("y")), ...);
 *         NV.SetValue<FVector3f>(V);
 *         bDefaultSet = true;      // <-- reported to the caller as "with default"
 *     }
 *
 * Every line of that is wrong for a STRING default, and the failure is silent end to end:
 *
 *   1. AsObjectOrParseString(JsonValue) starts with JsonValue->AsObject(). On an
 *      FJsonValueString that returns a VALID BUT EMPTY FJsonObject rather than null — the
 *      helper's own comment says so, and the static-switch `output_vars` site guards against
 *      exactly this by testing Values.Num() > 0.
 *   2. Its fallback then calls FJsonSerializer::Deserialize(Reader, O) and IGNORES THE RETURN
 *      VALUE. Deserialize leaves OutObject untouched when it fails, so a non-JSON string such
 *      as "1,2,3" leaves the empty-but-valid object in place.
 *   3. `if (O.IsValid())` is therefore TRUE for any string at all.
 *   4. FJsonObject::GetNumberField on an absent field logs "Field x was not found", returns an
 *      FJsonValueNull, and AsNumber() answers 0.
 *
 * Result, measured 2026-08-17 on /Game/FX/_Probes/2026-08-17/NS_ParamVal_P: BOTH documented
 * spellings, "1,2,3" and "(X=1.0,Y=2.0,Z=3.0)", landed as (0,0,0) while the response said
 * "Added user parameter 'DirectVec' with default" and warnings was empty. The handler already
 * had a "failed to set default value" branch — it never fired, because bDefaultSet was true.
 *
 * WHAT THIS FILE DOES, AND DELIBERATELY DOES NOT DO.
 * It converts JSON to a literal STRING and nothing else. It does not decide whether that
 * literal is valid for the type: that is MonolithNiagaraHelpers::ValidateStackInputLiteral,
 * the gap #32 validator, which probes the caller's spelling with the type's OWN registered
 * editor utilities on an ALLOCATED FNiagaraVariable — the only arrangement in which the
 * engine's composite parsers can report failure at all. Reusing it is the point: a second
 * per-type decoder is how add_user_parameter and set_module_input_value drifted apart in the
 * first place, and the validator handles every composite type without naming any of them.
 *
 * So the split is: this header decides WHAT STRING the caller meant, #32 decides whether the
 * engine accepts it, and the type utilities write the bytes. A value that survives none of
 * those steps is REFUSED with a message, never written as zero.
 *
 * Pure: needs no asset, no editor and no NiagaraEditor module, so it is unit-tested directly
 * in Private/Tests/MonolithNiagaraDefaultLiteralTest.cpp.
 */
namespace MonolithNiagaraDefaultLiteral
{
	/**
	 * Component count implied by the type's OWN footprint — same rule as
	 * MonolithNiagaraHelpers::CompositeComponentCount, which the #32 validator uses: every
	 * composite type with a registered pin-default utility is a packed block of 4-byte
	 * components (FVector2f 8, FVector3f/FNiagaraPosition 12, FVector4f/FQuat4f/FLinearColor 16).
	 * Returns 0 when the footprint is not 2, 3 or 4 such components, i.e. when a keyed object or
	 * a JSON array is not a meaningful spelling for this type at all.
	 */
	inline int32 ComponentCountForType(const FNiagaraTypeDefinition& TypeDef)
	{
		const int32 Size = TypeDef.GetSize();
		if (Size <= 0 || (Size % 4) != 0)
		{
			return 0;
		}
		const int32 Count = Size / 4;
		return (Count >= 2 && Count <= 4) ? Count : 0;
	}

	/**
	 * A JSON number as a literal that survives the trip to float32 and back.
	 *
	 * Integral values are printed as integers — both so an int parameter gets a spelling its
	 * parser accepts ("5", not "5.000000") and so a large int32 is not mangled by %g's
	 * significant-digit limit.
	 *
	 * Everything else uses %.9g. Nine significant digits is the exact round-trip width for IEEE
	 * binary32, which is what every Niagara scalar and every composite COMPONENT is.
	 * Deliberately NOT FString::SanitizeFloat, the convention on the READ side of this file's
	 * neighbours: that is "%f"-based, so it renders 1e-8 as "0.000000" and would hand back the
	 * very silent zero this file exists to remove.
	 */
	inline FString NumberToLiteral(double Number)
	{
		// 2^53: past this a double no longer represents consecutive integers, so the integral
		// test stops meaning what it looks like it means.
		if (Number == FMath::TruncToDouble(Number) && FMath::Abs(Number) <= 9007199254740992.0)
		{
			return FString::Printf(TEXT("%lld"), static_cast<int64>(Number));
		}
		return FString::Printf(TEXT("%.9g"), Number);
	}

	/**
	 * Parse a string that may itself be a serialized JSON object. Claude Code double-serializes
	 * MCP params, so `default` can arrive as {\"x\":1,...} with escaped quotes.
	 *
	 * The RETURN VALUE of Deserialize is checked, which is the single line AsObjectOrParseString
	 * omits and the reason gap #106 was silent. Returns null for anything that is not an object,
	 * including plain literals like "1,2,3" — those are meant to travel through verbatim.
	 */
	inline TSharedPtr<FJsonObject> TryParseObjectString(const FString& InString)
	{
		FString Unescaped = InString;
		Unescaped.ReplaceInline(TEXT("\\\""), TEXT("\""), ESearchCase::CaseSensitive);
		Unescaped.ReplaceInline(TEXT("\\\\"), TEXT("\\"), ESearchCase::CaseSensitive);

		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Unescaped);
		if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid() && Parsed->Values.Num() > 0)
		{
			return Parsed;
		}
		return nullptr;
	}

	/**
	 * One component of a composite, as a literal. Numbers are formatted; strings are kept
	 * CHARACTER FOR CHARACTER so the digits the caller typed are the digits that land in the
	 * value. Anything else (bool, nested object, array, null) is refused here rather than
	 * coerced — AsNumber() on those answers 0, which is the defect.
	 */
	inline bool ComponentToLiteral(const TSharedPtr<FJsonValue>& Value, FString& OutLiteral, FString& OutWhy)
	{
		if (!Value.IsValid())
		{
			OutWhy = TEXT("is missing");
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			OutLiteral = NumberToLiteral(Value->AsNumber());
			return true;
		}
		if (Value->Type == EJson::String)
		{
			const FString Trimmed = Value->AsString().TrimStartAndEnd();
			if (Trimmed.IsEmpty())
			{
				OutWhy = TEXT("is an empty string");
				return false;
			}
			OutLiteral = Trimmed;
			return true;
		}
		OutWhy = TEXT("is not a number");
		return false;
	}

	/**
	 * A keyed object ({"x":1,"y":2,"z":3} or {"r":1,"g":0,"b":0}) to a bare comma list, which is
	 * the spelling ValidateStackInputLiteral re-labels per type as needed.
	 *
	 * A MISSING component is an error naming the key, not a zero. That is the whole point: the
	 * old decoder read x/y/z off a colour object and wrote (0,0,0) without comment. The one
	 * exception is a colour's alpha, which the previous behaviour defaulted to 1 and which the
	 * engine's own FLinearColor::InitFromString also treats as optional.
	 */
	inline bool ObjectToLiteral(const TSharedPtr<FJsonObject>& Object, const FNiagaraTypeDefinition& TypeDef,
		FString& OutLiteral, FString& OutError)
	{
		static const TCHAR* const VectorKeys[4] = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w") };
		static const TCHAR* const ColorKeys[4]  = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };

		const int32 Expected = ComponentCountForType(TypeDef);
		if (Expected == 0)
		{
			OutError = FString::Printf(
				TEXT("was given as a JSON object, but %s is not a 2-, 3- or 4-component type, so component "
					 "keys mean nothing for it."),
				*TypeDef.GetName());
			return false;
		}

		// FString map keys compare case-insensitively, so "X" and "x" both hit.
		const bool bColorType = (TypeDef == FNiagaraTypeDefinition::GetColorDef());
		const TCHAR* const* Primary   = bColorType ? ColorKeys  : VectorKeys;
		const TCHAR* const* Secondary = bColorType ? VectorKeys : ColorKeys;

		const TCHAR* const* Keys = nullptr;
		if (Object->HasField(Primary[0]))        { Keys = Primary;   }
		else if (Object->HasField(Secondary[0])) { Keys = Secondary; }

		if (!Keys)
		{
			OutError = FString::Printf(
				TEXT("was given as a JSON object with no '%s' key, so no component could be read. Use "
					 "{\"%s\":..,\"%s\":..} or a plain literal such as \"1,2,3\" / \"(%s=1,%s=2)\"."),
				Primary[0], Primary[0], Primary[1], *FString(Primary[0]).ToUpper(), *FString(Primary[1]).ToUpper());
			return false;
		}

		// Over-specified is refused, mirroring the same rule in ValidateStackInputLiteral: reading
		// the first N keys and ignoring the rest would accept {"x":1,"y":2,"z":3,"w":4} on a
		// 3-component type and silently drop w. #32 cannot catch that for us — by then the
		// literal is a well-formed "1,2,3" and looks perfectly correct.
		if (Expected < 4 && Object->HasField(Keys[Expected]))
		{
			TArray<FString> Accepted;
			for (int32 Index = 0; Index < Expected; ++Index)
			{
				Accepted.Add(Keys[Index]);
			}
			OutError = FString::Printf(
				TEXT("was given a '%s' component, but %s takes only %d (%s). Refused rather than dropping "
					 "it silently."),
				Keys[Expected], *TypeDef.GetName(), Expected, *FString::Join(Accepted, TEXT("/")));
			return false;
		}

		TArray<FString> Tokens;
		for (int32 Index = 0; Index < Expected; ++Index)
		{
			const TSharedPtr<FJsonValue> Field = Object->TryGetField(Keys[Index]);
			if (!Field.IsValid())
			{
				// Colour alpha only. Every other absent component is a refusal.
				if (bColorType && Keys == ColorKeys && Index == 3)
				{
					Tokens.Add(TEXT("1"));
					continue;
				}
				OutError = FString::Printf(
					TEXT("was given as a JSON object missing the '%s' component, and %s needs %d of them. "
						 "Refused rather than defaulting it to 0 — that is exactly how this action used to "
						 "report a value it had not stored."),
					Keys[Index], *TypeDef.GetName(), Expected);
				return false;
			}

			FString Token;
			FString Why;
			if (!ComponentToLiteral(Field, Token, Why))
			{
				OutError = FString::Printf(TEXT("has a '%s' component that %s."), Keys[Index], *Why);
				return false;
			}
			Tokens.Add(Token);
		}

		OutLiteral = FString::Join(Tokens, TEXT(","));
		return true;
	}

	/**
	 * A JSON array ([1,2,3]) to a bare comma list. Arrays were previously handled by the same
	 * AsObject() path as strings and so also landed as zeros; the arity is checked here so a
	 * short or long array is refused instead of being silently padded or truncated.
	 */
	inline bool ArrayToLiteral(const TArray<TSharedPtr<FJsonValue>>& Elements, const FNiagaraTypeDefinition& TypeDef,
		FString& OutLiteral, FString& OutError)
	{
		const int32 Expected = ComponentCountForType(TypeDef);
		if (Expected == 0)
		{
			OutError = FString::Printf(
				TEXT("was given as a JSON array, but %s is not a 2-, 3- or 4-component type."), *TypeDef.GetName());
			return false;
		}
		if (Elements.Num() != Expected)
		{
			OutError = FString::Printf(
				TEXT("was given as a JSON array of %d element(s), but %s takes exactly %d."),
				Elements.Num(), *TypeDef.GetName(), Expected);
			return false;
		}

		TArray<FString> Tokens;
		for (int32 Index = 0; Index < Elements.Num(); ++Index)
		{
			FString Token;
			FString Why;
			if (!ComponentToLiteral(Elements[Index], Token, Why))
			{
				OutError = FString::Printf(TEXT("has an element at index %d that %s."), Index, *Why);
				return false;
			}
			Tokens.Add(Token);
		}

		OutLiteral = FString::Join(Tokens, TEXT(","));
		return true;
	}

	/**
	 * The entry point: what literal did the caller mean by this `default`?
	 *
	 * On success OutLiteral is a string for ValidateStackInputLiteral to accept or refuse — it
	 * is NOT yet known to be valid for TypeDef. On failure OutError is a caller-facing clause
	 * that reads after "'default' for 'X' ...".
	 */
	inline bool JsonValueToPinDefaultLiteral(const TSharedPtr<FJsonValue>& JsonValue,
		const FNiagaraTypeDefinition& TypeDef, FString& OutLiteral, FString& OutError)
	{
		OutLiteral.Reset();
		OutError.Reset();

		if (!JsonValue.IsValid() || JsonValue->Type == EJson::Null)
		{
			OutError = TEXT("is null, which is not a value. Omit 'default' to leave the parameter at its "
				"type default, or pass a real one.");
			return false;
		}

		switch (JsonValue->Type)
		{
		case EJson::Boolean:
			OutLiteral = JsonValue->AsBool() ? TEXT("true") : TEXT("false");
			return true;

		case EJson::Number:
			OutLiteral = NumberToLiteral(JsonValue->AsNumber());
			return true;

		case EJson::Object:
			return ObjectToLiteral(JsonValue->AsObject(), TypeDef, OutLiteral, OutError);

		case EJson::Array:
			return ArrayToLiteral(JsonValue->AsArray(), TypeDef, OutLiteral, OutError);

		case EJson::String:
		{
			const FString Raw = JsonValue->AsString();

			// A double-serialized object still means the object it spells.
			if (const TSharedPtr<FJsonObject> Parsed = TryParseObjectString(Raw))
			{
				return ObjectToLiteral(Parsed, TypeDef, OutLiteral, OutError);
			}

			if (Raw.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("is an empty string, which encodes no value for any Niagara type.");
				return false;
			}

			// Verbatim — no reformatting anywhere on this path, for the reason spelled out in the
			// gap #32 block comment: re-emitting through the engine's own printers is lossy
			// (three decimals), so "0.0001,0,0" would become "0.000,0.000,0.000".
			OutLiteral = Raw;
			return true;
		}

		default:
			break;
		}

		OutError = TEXT("is not a value this action can read (expected a number, bool, string, "
			"component object or array).");
		return false;
	}
}
