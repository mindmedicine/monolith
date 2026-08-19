// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithEditorPreviewSceneActions.cpp
//
// Two editor:: actions for the ASSET-VIEWER PREVIEW SCENE profiles — the
// shared background / floor / environment settings every asset editor viewport
// (Niagara, PCG, Material, Static Mesh, Persona, Dataflow, TextureGraph, ...)
// reads from:
//
//   editor::get_preview_scene   — READ every UAssetViewerSettings profile plus
//                                 the ACTIVE profile index.
//   editor::set_preview_scene   — MUTATE profile fields in place and broadcast
//                                 the settings-changed event, which reaches
//                                 ALREADY-OPEN asset editors with no reopen.
//
// -----------------------------------------------------------------------------
// WHY THIS IS C++ AND NOT PYTHON / AN INI EDIT (UE 5.8 source, read 2026-08-18)
// -----------------------------------------------------------------------------
//
//  * UAssetViewerSettings::Profiles is `EditAnywhere, transient`
//    (AssetViewerSettings.h:378-380). It is NEVER config-serialised, so writing
//    [/Script/AdvancedPreviewScene.AssetViewerSettings] in any .ini is a silent
//    no-op. The config-backed classes are ULocalProfiles (h:309) and
//    USharedProfiles (h:319); the runtime array is assembled from them in
//    UAssetViewerSettings::PostInitProperties (cpp:200-262).
//
//  * The AdvancedPreviewScene module exports ZERO UFUNCTIONs, so nothing here
//    is directly callable from Python or Blueprint — property access is the
//    only lever.
//
//  * Python's property access hands out struct COPIES: mutating an element of
//    the list returned by get_editor_property('profiles') and writing it back
//    was MEASURED (2026-08-18) to read back unchanged. Native code can take a
//    REFERENCE into the array and mutate the live struct, which is the whole
//    reason these actions exist.
//
//  * UEditorPerProjectUserSettings::AssetViewerProfileIndex is UPROPERTY(Config)
//    with no EditAnywhere/BlueprintVisible (EditorPerProjectUserSettings.h:188),
//    so it is not Python-readable either. get_preview_scene reports it.
//
// -----------------------------------------------------------------------------
// HOW THE REFRESH REACHES OPEN EDITORS
// -----------------------------------------------------------------------------
//
// Every FAdvancedPreviewScene subscribes to the settings SINGLETON in its
// constructor (AdvancedPreviewScene.cpp:44-47), so one broadcast fans out to
// every preview scene alive in the process:
//
//     DefaultSettings = UAssetViewerSettings::Get();                    // :44
//     RefreshDelegate = DefaultSettings->OnAssetViewerSettingsChanged()
//         .AddRaw(this, &FAdvancedPreviewScene::OnAssetViewerSettingsRefresh);
//
// We fire that broadcast via UAssetViewerSettings::PostEditChangeProperty with
// a null-property FPropertyChangedEvent:
//
//     AssetViewerSettings.cpp:152  FName PropertyName = InPropertyChangedEvent.GetPropertyName();
//     UnrealType.h:7065-7068       GetPropertyName() -> NAME_None when Property == nullptr
//     AssetViewerSettings.cpp:166  OnAssetViewerSettingsChangedEvent.Broadcast(PropertyName);
//
// NAME_None is the MAXIMAL refresh: OnAssetViewerSettingsRefresh sets
// `bNameNone` (AdvancedPreviewScene.cpp:609) and ORs it into all four
// UpdateScene flags (:630). A null-property FPropertyChangedEvent is an
// established engine idiom (e.g. SLevelViewport.cpp:675 `DummyEvent(nullptr)`,
// CurveTableEditor.cpp:1147, ObjectTools.cpp:1462).
//
// PostEditChangeChainProperty (AssetViewerSettings.cpp:169-198) also yields
// NAME_None when the chain has no node after the active member, but it needs a
// hand-built FEditPropertyChain and UObject::PostEditChangeChainProperty runs
// archetype-instance propagation on a CDO (Obj.cpp:639-649). More moving parts,
// identical broadcast — so the simple path wins. Recorded deliberately.
//
// The three levers we expose are the reliable ones: UpdateScene applies them
// UNCONDITIONALLY at its end, regardless of the update flags
// (AdvancedPreviewScene.cpp:225-231):
//     SkyComponent->SetVisibility(Profile.bShowEnvironment, true);
//     SkyLight->SetVisibility(Profile.bUseSkyLighting, true);
//     FloorMeshComponent->SetVisibility(Profile.bShowFloor, true);
// and the flat background colour is read live per frame as
// EnvironmentColor * EnvironmentIntensity (AdvancedPreviewScene.cpp:237-241).
//
// -----------------------------------------------------------------------------
// HARD CONSTRAINTS ENCODED HERE
// -----------------------------------------------------------------------------
//
//  * NEVER add or remove Profiles entries. OnAssetViewerSettingsRefresh IGNORES
//    the update for any scene whose cached CurrentProfileIndex stops being valid
//    (AdvancedPreviewScene.cpp:607). set_preview_scene therefore REFUSES an
//    unknown `target` name rather than creating a profile.
//
//  * bShowGrid is UE_DEPRECATED(5.8) (AssetViewerSettings.h:112-115) and is
//    never referenced here.
//
//  * FPreviewSceneProfile is never COPIED in this file — the deprecated member
//    lives behind PRAGMA_DISABLE_DEPRECATION_WARNINGS in the header, and an
//    implicit copy instantiated in our TU would surface it. References only.
//
//  * `save` defaults to FALSE. UAssetViewerSettings::Save (cpp:118-146) writes
//    shared profiles through TryUpdateDefaultConfigFile into the PROJECT's
//    Config/DefaultEditor.ini — a tracked file.
// =============================================================================

#include "MonolithEditorActions.h"
#include "MonolithJsonUtils.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

#include "AssetViewerSettings.h"
#include "Editor/EditorPerProjectUserSettings.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithPreviewScene, Log, All);

// ----- Local helpers ---------------------------------------------------------

namespace
{
	/**
	 * The active profile index. UPROPERTY(Config)-only, so this read is the part
	 * of get_preview_scene that no Python route can reproduce
	 * (EditorPerProjectUserSettings.h:188-189).
	 */
	static int32 GetActiveProfileIndex()
	{
		return GetMutableDefault<UEditorPerProjectUserSettings>()->AssetViewerProfileIndex;
	}

	/** FLinearColor -> [r, g, b, a]. */
	static TSharedPtr<FJsonValue> ColorToJson(const FLinearColor& Color)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(Color.R));
		Arr.Add(MakeShared<FJsonValueNumber>(Color.G));
		Arr.Add(MakeShared<FJsonValueNumber>(Color.B));
		Arr.Add(MakeShared<FJsonValueNumber>(Color.A));
		return MakeShared<FJsonValueArray>(Arr);
	}

	/**
	 * Accepts [r,g,b] / [r,g,b,a], {r,g,b,a} (or {R,G,B,A}), or an
	 * FLinearColor::InitFromString-style "(R=..,G=..,B=..,A=..)" string.
	 * Alpha defaults to the profile's existing alpha when not supplied.
	 */
	static bool ParseLinearColorField(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		FLinearColor& InOutColor,
		FString& OutError)
	{
		const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
		if (!Field.IsValid())
		{
			OutError = FString::Printf(TEXT("'%s' is present but unreadable"), FieldName);
			return false;
		}

		// Array form.
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Field->TryGetArray(Arr) && Arr)
		{
			if (Arr->Num() < 3)
			{
				OutError = FString::Printf(
					TEXT("'%s' array needs at least 3 components [r,g,b] (got %d)"), FieldName, Arr->Num());
				return false;
			}
			InOutColor.R = (float)(*Arr)[0]->AsNumber();
			InOutColor.G = (float)(*Arr)[1]->AsNumber();
			InOutColor.B = (float)(*Arr)[2]->AsNumber();
			if (Arr->Num() >= 4)
			{
				InOutColor.A = (float)(*Arr)[3]->AsNumber();
			}
			return true;
		}

		// Object form.
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Field->TryGetObject(Obj) && Obj && (*Obj).IsValid())
		{
			double Component = 0.0;
			auto ReadComponent = [&Obj, &Component](const TCHAR* Lower, const TCHAR* Upper, float& Out) -> void
			{
				if ((*Obj)->TryGetNumberField(Lower, Component) || (*Obj)->TryGetNumberField(Upper, Component))
				{
					Out = (float)Component;
				}
			};
			ReadComponent(TEXT("r"), TEXT("R"), InOutColor.R);
			ReadComponent(TEXT("g"), TEXT("G"), InOutColor.G);
			ReadComponent(TEXT("b"), TEXT("B"), InOutColor.B);
			ReadComponent(TEXT("a"), TEXT("A"), InOutColor.A);
			return true;
		}

		// String form — either "(R=..,G=..,B=..,A=..)" or a serialized JSON array.
		FString AsString;
		if (Field->TryGetString(AsString) && !AsString.IsEmpty())
		{
			FLinearColor Parsed = InOutColor;
			if (Parsed.InitFromString(AsString))
			{
				InOutColor = Parsed;
				return true;
			}

			// "[0.1, 0.2, 0.3]" — tolerated because several MCP clients stringify arrays.
			FString Trimmed = AsString.TrimStartAndEnd();
			Trimmed.RemoveFromStart(TEXT("["));
			Trimmed.RemoveFromEnd(TEXT("]"));
			TArray<FString> Parts;
			Trimmed.ParseIntoArray(Parts, TEXT(","), true);
			if (Parts.Num() >= 3)
			{
				InOutColor.R = FCString::Atof(*Parts[0].TrimStartAndEnd());
				InOutColor.G = FCString::Atof(*Parts[1].TrimStartAndEnd());
				InOutColor.B = FCString::Atof(*Parts[2].TrimStartAndEnd());
				if (Parts.Num() >= 4)
				{
					InOutColor.A = FCString::Atof(*Parts[3].TrimStartAndEnd());
				}
				return true;
			}
		}

		OutError = FString::Printf(
			TEXT("'%s' must be [r,g,b] / [r,g,b,a], {r,g,b,a}, or \"(R=..,G=..,B=..,A=..)\""),
			FieldName);
		return false;
	}

	/**
	 * Tolerant bool read — accepts a JSON bool, "true"/"false"/"1"/"0" strings,
	 * and a number. Returns false when the field is absent (bOutPresent tells
	 * absent from false).
	 */
	static bool ReadBoolField(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		bool& OutValue,
		bool& bOutPresent)
	{
		bOutPresent = false;
		const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
		if (!Field.IsValid() || Field->Type == EJson::Null)
		{
			return true;
		}

		bool bParsed = false;
		if (Field->TryGetBool(bParsed))
		{
			OutValue = bParsed;
			bOutPresent = true;
			return true;
		}

		double AsNumber = 0.0;
		if (Field->TryGetNumber(AsNumber))
		{
			OutValue = !FMath::IsNearlyZero(AsNumber);
			bOutPresent = true;
			return true;
		}

		FString AsString;
		if (Field->TryGetString(AsString))
		{
			AsString = AsString.TrimStartAndEnd();
			if (AsString.Equals(TEXT("true"), ESearchCase::IgnoreCase) || AsString == TEXT("1"))
			{
				OutValue = true;
				bOutPresent = true;
				return true;
			}
			if (AsString.Equals(TEXT("false"), ESearchCase::IgnoreCase) || AsString == TEXT("0"))
			{
				OutValue = false;
				bOutPresent = true;
				return true;
			}
		}

		return false;
	}

	/** Tolerant float read. Returns false only when present-but-unparseable. */
	static bool ReadFloatField(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		float& OutValue,
		bool& bOutPresent)
	{
		bOutPresent = false;
		const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
		if (!Field.IsValid() || Field->Type == EJson::Null)
		{
			return true;
		}

		double AsNumber = 0.0;
		if (Field->TryGetNumber(AsNumber))
		{
			OutValue = (float)AsNumber;
			bOutPresent = true;
			return true;
		}

		FString AsString;
		if (Field->TryGetString(AsString) && !AsString.TrimStartAndEnd().IsEmpty())
		{
			OutValue = FCString::Atof(*AsString.TrimStartAndEnd());
			bOutPresent = true;
			return true;
		}

		return false;
	}

	/**
	 * One profile -> JSON. Takes a CONST REFERENCE: FPreviewSceneProfile carries
	 * the UE_DEPRECATED(5.8) bShowGrid member, and copying it in this TU would
	 * instantiate the implicit copy ctor outside the header's deprecation pragma.
	 */
	static TSharedPtr<FJsonObject> ProfileToJson(const FPreviewSceneProfile& Profile, int32 Index)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("index"), Index);
		Obj->SetStringField(TEXT("profile_name"), Profile.ProfileName);
		Obj->SetBoolField(TEXT("show_environment"), Profile.bShowEnvironment);
		Obj->SetBoolField(TEXT("show_floor"), Profile.bShowFloor);
		Obj->SetBoolField(TEXT("use_sky_lighting"), Profile.bUseSkyLighting);
		Obj->SetBoolField(TEXT("post_processing_enabled"), Profile.bPostProcessingEnabled);
		Obj->SetField(TEXT("environment_color"), ColorToJson(Profile.EnvironmentColor));
		Obj->SetNumberField(TEXT("environment_intensity"), Profile.EnvironmentIntensity);
		Obj->SetNumberField(TEXT("sky_light_intensity"), Profile.SkyLightIntensity);
		Obj->SetBoolField(TEXT("shared_profile"), Profile.bSharedProfile);
		Obj->SetBoolField(TEXT("is_engine_default_profile"), Profile.bIsEngineDefaultProfile);
		return Obj;
	}

	/**
	 * The shared read path. set_preview_scene returns this AFTER writing, so its
	 * response is itself a round-trip rather than an echo of the request.
	 */
	static TSharedPtr<FJsonObject> BuildPreviewSceneState(UAssetViewerSettings* Settings)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		const int32 ActiveIndex = GetActiveProfileIndex();
		const bool bActiveValid = Settings->Profiles.IsValidIndex(ActiveIndex);

		Result->SetNumberField(TEXT("active_profile_index"), ActiveIndex);
		Result->SetStringField(TEXT("active_profile_name"),
			bActiveValid ? Settings->Profiles[ActiveIndex].ProfileName : FString());
		Result->SetBoolField(TEXT("active_profile_index_valid"), bActiveValid);
		Result->SetNumberField(TEXT("profile_count"), Settings->Profiles.Num());

		TArray<TSharedPtr<FJsonValue>> ProfileValues;
		ProfileValues.Reserve(Settings->Profiles.Num());
		for (int32 i = 0; i < Settings->Profiles.Num(); ++i)
		{
			ProfileValues.Add(MakeShared<FJsonValueObject>(ProfileToJson(Settings->Profiles[i], i)));
		}
		Result->SetArrayField(TEXT("profiles"), ProfileValues);

		return Result;
	}

	/** Comma-joined profile names, for refusal messages. */
	static FString JoinProfileNames(const UAssetViewerSettings* Settings)
	{
		TArray<FString> Names;
		Names.Reserve(Settings->Profiles.Num());
		for (const FPreviewSceneProfile& Profile : Settings->Profiles)
		{
			Names.Add(FString::Printf(TEXT("'%s'"), *Profile.ProfileName));
		}
		return FString::Join(Names, TEXT(", "));
	}
}

// =============================================================================
// editor::get_preview_scene
// =============================================================================

FMonolithActionResult FMonolithEditorActions::HandleGetPreviewScene(
	const TSharedPtr<FJsonObject>& Params)
{
	// UAssetViewerSettings::Get() is GetMutableDefault<> — a CDO singleton, the
	// same object every FAdvancedPreviewScene holds (AssetViewerSettings.cpp:87-111).
	UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
	if (!Settings)
	{
		return FMonolithActionResult::Error(TEXT("UAssetViewerSettings::Get() returned null"));
	}

	TSharedPtr<FJsonObject> Result = BuildPreviewSceneState(Settings);
	Result->SetBoolField(TEXT("success"), true);

	if (!Settings->Profiles.IsValidIndex(GetActiveProfileIndex()))
	{
		FMonolithJsonUtils::AddWarning(Result, FString::Printf(
			TEXT("AssetViewerProfileIndex (%d) is out of range for %d profiles — every live preview scene falls back to index 0 (AdvancedPreviewScene.cpp:50)."),
			GetActiveProfileIndex(), Settings->Profiles.Num()));
	}

	return FMonolithActionResult::Success(Result);
}

// =============================================================================
// editor::set_preview_scene
// =============================================================================

FMonolithActionResult FMonolithEditorActions::HandleSetPreviewScene(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Params object is null"),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
	if (!Settings)
	{
		return FMonolithActionResult::Error(TEXT("UAssetViewerSettings::Get() returned null"));
	}
	if (Settings->Profiles.Num() == 0)
	{
		return FMonolithActionResult::Error(
			TEXT("UAssetViewerSettings has no profiles — nothing to mutate."));
	}

	// --- Parse the mutation payload BEFORE touching anything, so a bad param
	// --- cannot leave a half-applied profile behind.
	bool bShowEnvironment = false, bHasShowEnvironment = false;
	if (!ReadBoolField(Params, TEXT("show_environment"), bShowEnvironment, bHasShowEnvironment))
	{
		return FMonolithActionResult::Error(
			TEXT("'show_environment' must be a bool (or \"true\"/\"false\")"),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	bool bShowFloor = false, bHasShowFloor = false;
	if (!ReadBoolField(Params, TEXT("show_floor"), bShowFloor, bHasShowFloor))
	{
		return FMonolithActionResult::Error(
			TEXT("'show_floor' must be a bool (or \"true\"/\"false\")"),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	float EnvironmentIntensity = 1.0f;
	bool bHasEnvironmentIntensity = false;
	if (!ReadFloatField(Params, TEXT("environment_intensity"), EnvironmentIntensity, bHasEnvironmentIntensity))
	{
		return FMonolithActionResult::Error(
			TEXT("'environment_intensity' must be a number"),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	const bool bHasEnvironmentColor = Params->HasField(TEXT("environment_color"));

	bool bSave = false, bHasSave = false;
	if (!ReadBoolField(Params, TEXT("save"), bSave, bHasSave))
	{
		return FMonolithActionResult::Error(TEXT("'save' must be a bool"),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	const bool bAnyMutation =
		bHasShowEnvironment || bHasShowFloor || bHasEnvironmentIntensity || bHasEnvironmentColor;

	if (!bAnyMutation && !bSave)
	{
		// Never a silent no-op: say so instead of returning a success that did nothing.
		return FMonolithActionResult::Error(
			TEXT("No mutating parameter supplied (show_environment, show_floor, environment_color, environment_intensity) and save=false — nothing to do. Use editor::get_preview_scene to read the current state."),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	// --- Resolve target profile indices. NEVER creates a profile: shrinking or
	// --- growing Profiles makes every scene whose cached CurrentProfileIndex
	// --- falls out of range IGNORE the refresh (AdvancedPreviewScene.cpp:607).
	FString Target = TEXT("active");
	Params->TryGetStringField(TEXT("target"), Target);
	Target = Target.TrimStartAndEnd();
	if (Target.IsEmpty())
	{
		Target = TEXT("active");
	}

	TArray<int32> TargetIndices;
	if (Target.Equals(TEXT("all"), ESearchCase::IgnoreCase))
	{
		for (int32 i = 0; i < Settings->Profiles.Num(); ++i)
		{
			TargetIndices.Add(i);
		}
	}
	else if (Target.Equals(TEXT("active"), ESearchCase::IgnoreCase))
	{
		const int32 ActiveIndex = GetActiveProfileIndex();
		if (!Settings->Profiles.IsValidIndex(ActiveIndex))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("AssetViewerProfileIndex (%d) is out of range for %d profiles. Pass target=\"all\" or an explicit profile name. Available: %s"),
				ActiveIndex, Settings->Profiles.Num(), *JoinProfileNames(Settings)));
		}
		TargetIndices.Add(ActiveIndex);
	}
	else
	{
		for (int32 i = 0; i < Settings->Profiles.Num(); ++i)
		{
			if (Settings->Profiles[i].ProfileName.Equals(Target, ESearchCase::IgnoreCase))
			{
				TargetIndices.Add(i);
			}
		}
		if (TargetIndices.Num() == 0)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("No preview-scene profile named '%s'. This action MUTATES profiles and never creates one (adding/removing entries makes open preview scenes ignore the refresh — AdvancedPreviewScene.cpp:607). Available: %s"),
				*Target, *JoinProfileNames(Settings)),
				FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	// --- Apply, IN PLACE. `Profile` is a reference into Settings->Profiles;
	// --- this is the whole reason the action is native (Python hands out copies).
	int32 ChangedFieldCount = 0;
	TArray<TSharedPtr<FJsonValue>> TargetReport;

	for (const int32 Index : TargetIndices)
	{
		FPreviewSceneProfile& Profile = Settings->Profiles[Index];

		TArray<FString> ChangedFields;

		if (bHasShowEnvironment && Profile.bShowEnvironment != bShowEnvironment)
		{
			Profile.bShowEnvironment = bShowEnvironment;
			ChangedFields.Add(TEXT("show_environment"));
		}
		if (bHasShowFloor && Profile.bShowFloor != bShowFloor)
		{
			Profile.bShowFloor = bShowFloor;
			ChangedFields.Add(TEXT("show_floor"));
		}
		if (bHasEnvironmentIntensity && !FMath::IsNearlyEqual(Profile.EnvironmentIntensity, EnvironmentIntensity))
		{
			Profile.EnvironmentIntensity = EnvironmentIntensity;
			ChangedFields.Add(TEXT("environment_intensity"));
		}
		if (bHasEnvironmentColor)
		{
			FLinearColor NewColor = Profile.EnvironmentColor; // seeds alpha when caller sends rgb only
			FString ColorError;
			if (!ParseLinearColorField(Params, TEXT("environment_color"), NewColor, ColorError))
			{
				return FMonolithActionResult::Error(ColorError, FMonolithJsonUtils::ErrInvalidParams);
			}
			if (!Profile.EnvironmentColor.Equals(NewColor))
			{
				Profile.EnvironmentColor = NewColor;
				ChangedFields.Add(TEXT("environment_color"));
			}
		}

		ChangedFieldCount += ChangedFields.Num();

		TArray<TSharedPtr<FJsonValue>> ChangedFieldValues;
		for (const FString& Field : ChangedFields)
		{
			ChangedFieldValues.Add(MakeShared<FJsonValueString>(Field));
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), Index);
		Entry->SetStringField(TEXT("profile_name"), Profile.ProfileName);
		Entry->SetArrayField(TEXT("changed_fields"), ChangedFieldValues);
		TargetReport.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// --- Broadcast. See the file header for why this path and not the chain one.
	bool bBroadcast = false;
	if (bAnyMutation)
	{
		// UAssetViewerSettings::PostEditChangeProperty writes the ACTIVE profile's
		// soft-object paths back from its soft pointers (cpp:157 ->
		// FPreviewSceneProfile::PostEditChangeProperty, cpp:54-60). If those
		// pointers were never resolved, that write-back would blank the stored
		// paths. LoadProfileObjects() resolves them from the paths first — the
		// same call the engine makes in Get() (cpp:98-101) and in the scene
		// constructor (AdvancedPreviewScene.cpp:53). No-op when already loaded.
		const int32 ActiveIndex = GetActiveProfileIndex();
		if (Settings->Profiles.IsValidIndex(ActiveIndex))
		{
			Settings->Profiles[ActiveIndex].LoadProfileObjects();
		}

		FPropertyChangedEvent ChangedEvent(nullptr, EPropertyChangeType::ValueSet);
		Settings->PostEditChangeProperty(ChangedEvent);
		bBroadcast = true;

		UE_LOG(LogMonolithPreviewScene, Log,
			TEXT("set_preview_scene: mutated %d profile(s), %d field(s) changed; broadcast NAME_None."),
			TargetIndices.Num(), ChangedFieldCount);
	}

	// --- Optional persistence. Writes the project's Config/DefaultEditor.ini.
	if (bSave)
	{
		Settings->Save(/*bWarnIfFail=*/true);
		UE_LOG(LogMonolithPreviewScene, Log,
			TEXT("set_preview_scene: UAssetViewerSettings::Save() — shared profiles written to the project Config/DefaultEditor.ini."));
	}

	// --- Round-trip: re-read through the same path get_preview_scene uses.
	TSharedPtr<FJsonObject> Result = BuildPreviewSceneState(Settings);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("target"), Target);
	Result->SetArrayField(TEXT("targets"), TargetReport);
	Result->SetNumberField(TEXT("target_count"), TargetIndices.Num());
	Result->SetNumberField(TEXT("changed_field_count"), ChangedFieldCount);
	Result->SetBoolField(TEXT("broadcast"), bBroadcast);
	Result->SetStringField(TEXT("broadcast_property"), bBroadcast ? TEXT("NAME_None") : TEXT(""));
	Result->SetBoolField(TEXT("saved"), bSave);

	if (bAnyMutation && ChangedFieldCount == 0)
	{
		FMonolithJsonUtils::AddWarning(Result,
			TEXT("Every requested value already matched — no field changed. The refresh was still broadcast."));
	}
	if (bSave)
	{
		FMonolithJsonUtils::AddWarning(Result,
			TEXT("save=true wrote the project's Config/DefaultEditor.ini ([/Script/AdvancedPreviewScene.SharedProfiles]) and the per-user Saved/Config/.../Editor.ini. DefaultEditor.ini is a tracked project-settings file."));
	}

	return FMonolithActionResult::Success(Result);
}
