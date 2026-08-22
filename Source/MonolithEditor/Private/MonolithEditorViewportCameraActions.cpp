// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithEditorViewportCameraActions.cpp
//
// editor:: actions for EDITOR VIEWPORT CLIENTS — including the per-asset-editor
// PREVIEW viewports (Niagara, Material, Static Mesh, PCG, Persona, ...) that no
// other route can reach.
//
//   editor::list_viewports       — READ-ONLY enumeration of every live
//                                  FEditorViewportClient in the process, with
//                                  its identification keys and its true camera
//                                  state.
//
//   editor::set_viewport_camera  — MOVE one of those cameras: what to look at,
//                                  how far away, from which angle. Returns the
//                                  SAME shape list_viewports returns, re-read
//                                  after the write.
//
// -----------------------------------------------------------------------------
// WHY THIS IS C++ AND NOT PYTHON (UE 5.8 source, read 2026-08-21)
// -----------------------------------------------------------------------------
//
//  * GEditor->GetAllViewportClients() is not a UFUNCTION and
//    FEditorViewportClient is not a UObject, so there is NO reflection entry
//    point — not via Python, not via the native toolset registry.
//
//  * The AdvancedPreviewScene module and NiagaraEditor/Private/Widgets export
//    ZERO UFUNCTIONs, so the widget side is unreachable too.
//
//  * The per-editor preview scene was previously believed unreachable
//    ("there is no programmatic route to the per-editor scene object").
//    That is FALSE for native code: every FEditorViewportClient registers
//    ITSELF in a global engine-owned list from its base constructor —
//        ViewIndex = GEditor->AddViewportClients(this);
//    (EditorViewportClient.cpp:601, removed in the dtor at :681, so the list
//    never dangles), and the list is publicly readable through
//    UEditorEngine::GetAllViewportClients() (EditorEngine.h:718-719, header-inline
//    so no export symbol is needed; backing array at EditorEngine.h:2883).
//
// -----------------------------------------------------------------------------
// 🛑 THE REASON THIS ACTION EXISTS: IN ORBIT MODE, CAMERA READ-BACK LIES
// -----------------------------------------------------------------------------
//
// Preview viewports are in ORBIT MODE BY DEFAULT (Niagara reads it from the
// asset and the struct default is `bUseOrbitMode = true`,
// NiagaraSystemEditorData.h:64; SNiagaraSystemViewport.cpp:676-690).
//
// In orbit mode the rendered camera is NOT ViewLocation. From
// FEditorViewportClient::CalcViewRotationMatrix (EditorViewportClient.cpp:7392-7406):
//
//     if (bUsingOrbitCamera)
//         return FTranslationMatrix(ViewTransform.GetLocation()) * ViewTransform.ComputeOrbitMatrix();
//
// and the scene view's world->view matrix is FTranslationMatrix(-ViewOrigin) *
// ViewRotationMatrix with ViewOrigin == ViewLocation (EditorViewportClient.cpp:1141).
// The two translations CANCEL, so the effective world->view matrix in orbit mode
// is exactly ComputeOrbitMatrix() (EditorViewportClient.cpp:395-404):
//
//     FTransform(-LookAt)
//   * FTransform(FRotator(0, Yaw, 0))
//   * FTransform(FRotator(0, 0, Pitch))
//   * FTransform(FVector(0, (ViewLocation - LookAt).Size(), 0))
//
// Three consequences, all of which defeat a naive read-back:
//   1. ViewLocation contributes ONLY its DISTANCE from LookAt. Its direction is
//      discarded entirely.
//   2. ROLL is discarded — only Yaw and Pitch are used.
//   3. GetViewLocation() returns verbatim whatever SetViewLocation() stored
//      (EditorViewportClient.h:562-567). So a write->read round-trip succeeds
//      PERFECTLY over a camera that renders from somewhere else.
//
// Therefore this action reports `effective_camera_location` /
// `effective_camera_rotation`, RECOMPUTED from the orbit matrix — never an echo
// of `view_location`. That field is the whole point of the action: it is the
// only value here that corresponds to what is actually rendered.
//
// The inversion idiom (OrbitMatrix -> .GetOrigin() / .Rotator()) is the engine's
// own, from FEditorViewportClient::SetLookAtLocation(.., bRecalculateView=true)
// at EditorViewportClient.h:541-546. We use FMatrix::Inverse() rather than the
// engine's InverseFast(): Inverse() returns Identity for a degenerate matrix and
// VectorMatrixInverse "will return false and fill identity for non-invertible
// matrices" (Matrix.inl:384-401), so it cannot assert. InverseFast() carries a
// checkSlow. A read-only diagnostic must never be able to take the editor down.
//
// -----------------------------------------------------------------------------
// IDENTIFICATION — two independent keys, both reported
// -----------------------------------------------------------------------------
//
//  * Key A, `widget_type`: GetEditorViewportWidget() (EditorViewportClient.h:1299)
//    -> SWidget::GetType() (SWidget.h:1559, SLATECORE_API). The backing
//    TypeOfWidget is assigned unconditionally, outside any #if
//    (SWidget.cpp:1398-1400), so it is present in the editor build. Yields
//    literals like "SNiagaraSystemViewport" / "SMaterialEditorViewport".
//    Needs no private header and no cast.
//
//  * Key B, `preview_assets`: GetPreviewScene()->GetWorld() (PreviewScene.h:92)
//    then match components in that world. This identifies the viewport for a
//    NAMED asset, not merely "a Niagara viewport".
//    ⚠️ Key B MUST go through the WORLD, never a widget cast:
//    SNiagaraSystemViewport.h lives in NiagaraEditor/Private/Widgets/ and is not
//    includable from a plugin, so GetPreviewComponent() is unreachable here.
//
// -----------------------------------------------------------------------------
// THE MUTATOR'S CONTRACT (set_viewport_camera) — WHY IT IS SHAPED THIS WAY
// -----------------------------------------------------------------------------
//
// Because of the orbit trap above, the PRIMARY interface is deliberately NOT
// "put the camera here". It is: WHAT TO LOOK AT (look_at), HOW FAR AWAY
// (distance), and FROM WHAT ANGLE (a named preset, or raw yaw/pitch).
//
//  * THE ORBIT PATH IS THE ONLY PLACEMENT PATH:
//        SetViewRotation(NewRotation);                  // h:521-525
//        SetViewLocationForOrbiting(LookAt, Distance);  // h:1192, cpp:876-884
//    In that ORDER, because SetViewLocationForOrbiting derives its direction
//    from the CURRENT ViewRotation (cpp:878-882). It is correct in BOTH modes:
//    orbiting, it sets the pivot and the radius that ComputeOrbitMatrix reads;
//    not orbiting, it places ViewLocation at LookAt - Distance * Forward(Rot),
//    which is the camera looking at LookAt from Distance away.
//
//  * A RAW `location` IS REFUSED WHILE THE VIEWPORT IS ORBITING. In orbit mode
//    SetViewLocation() cannot place the camera — only |ViewLocation - LookAt|
//    survives (cpp:401) — yet GetViewLocation() returns it verbatim, so the
//    caller round-trips perfectly over a camera that moved only radially. We
//    will not accept a value we know will not be honoured, and we will NOT
//    silently ToggleOrbitCamera(false) to make it honourable: that rewrites
//    ViewRotation from the orbit matrix (cpp:904-920) and jumps the camera.
//    The caller is told to use look_at / distance / angle instead.
//
//  * THE NAMED ANGLE PRESETS RESOLVE TO DIFFERENT YAWS IN THE TWO MODES, and
//    that is the reason they exist. Rendered camera position is
//        orbit:      LookAt + d * (-cosP*sinY, -cosP*cosY, -sinP)
//        non-orbit:  LookAt - d * ( cosP*cosY,  cosP*sinY,  sinP)
//    (the first solved from cpp:395-404, the second read off cpp:876-884).
//    These agree only where cosY == sinY, i.e. yaw -135 / +45; in general
//    Y_orbit = 90 - Y_nonorbit. Each preset's pair was checked by rebuilding
//    the full rotation chain and comparing against the engine's OWN ortho view
//    matrices at cpp:1341-1388 — all six axis views reproduce them exactly,
//    including the in-plane roll. Derivation: the 2026-08-22 staging note.
//
//  * FOV WRITES BOTH FIELDS. ViewFOV is current, FOVAngle is the stored/ini
//    value (h:2009-2012). Epic's own ULevelEditorSubsystem::SetLevelViewportFOV
//    writes only ViewFOV (LevelEditorSubsystem.cpp:396-407) while several engine
//    paths restore ViewFOV from FOVAngle behind the caller's back, so writing
//    one is a change with a timer on it.
//
//  * IT INVALIDATES BY DEFAULT. Neither engine camera setter does
//    (UnrealEditorSubsystem.cpp:52-60, LevelEditorSubsystem.cpp:364-377), and
//    ULevelEditorSubsystem::EditorInvalidateViewports() ignores its caller and
//    invalidates the ACTIVE viewport instead (LevelEditorSubsystem.cpp:211-221).
//    We invalidate the client we actually touched. FEditorViewportClient::
//    Invalidate() null-guards its own Viewport (cpp:6622-6636).
//
//  * WE DO NOT COPY EPIC'S IDIOM. SetLevelViewportCameraInfo calls
//    SetViewLocationForOrbiting(CameraLocation) and then SetViewLocation(
//    CameraLocation), leaving LookAt EQUAL TO THE CAMERA'S OWN POSITION and the
//    orbit radius at zero (LevelEditorSubsystem.cpp:364-377). Harmless while a
//    level viewport is not orbiting; disqualifying for a preview viewport,
//    which is.
//
// -----------------------------------------------------------------------------
// HARD CONSTRAINTS ENCODED HERE
// -----------------------------------------------------------------------------
//
//  * list_viewports IS READ-ONLY. It mutates no viewport, camera or asset.
//    set_viewport_camera mutates ONLY the one client its `target` resolved to,
//    and re-reads THROUGH THE SAME CODE list_viewports uses so the response is
//    a round-trip and not an echo.
//
//  * ONE TARGET RESOLVER, SHARED. The identification keys reported by
//    list_viewports (index / widget_type / preview_assets / kind) and the keys
//    accepted by set_viewport_camera's `target` are produced by the SAME
//    helpers below. They cannot drift apart into "the value it printed is not
//    the value it accepts".
//
//  * AN AMBIGUOUS TARGET IS AN ERROR THAT LISTS THE CANDIDATES. It is never a
//    silent pick — silently moving a different camera than the caller meant is
//    indistinguishable from the action not working.
//
//  * DEFENSIVE AT EVERY HOP. GetEditorViewportWidget() returns a .Pin()'d
//    TSharedPtr that can be invalid; GetPreviewScene() can be null; the
//    `Viewport` member can be null for a client that is not currently attached
//    to a render surface. A crash here costs an editor session and any unsaved
//    fixture state, so every dereference is guarded and the guard's outcome is
//    REPORTED (has_widget / has_viewport) rather than silently coerced.
//
//  * `world_path` for a preview client reads PreviewScene->GetWorld() DIRECTLY
//    and does NOT use FEditorViewportClient::GetWorld(). That virtual falls back
//    to the global GWorld when the preview world is null
//    (EditorViewportClient.cpp:4644-4657), which would silently report the LEVEL
//    world for a preview viewport — precisely the kind of plausible-but-wrong
//    reading this action exists to eliminate.
//
//  * ELevelViewportType is stringified by an explicit switch, not by
//    StaticEnum<>(): the enum carries value-ALIASES (LVT_OrthoTop == LVT_OrthoXY,
//    UnrealEdTypes.h:129+) which make a reflection name lookup ambiguous.
// =============================================================================

#include "MonolithEditorActions.h"
#include "MonolithJsonUtils.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UObjectIterator.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorViewportClient.h"
#include "SEditorViewport.h"
#include "PreviewScene.h"
#include "UnrealClient.h"
#include "Engine/World.h"

#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithViewportCamera, Log, All);

// ----- Local helpers ---------------------------------------------------------

namespace
{
	/** FVector -> [x, y, z]. */
	static TSharedPtr<FJsonValue> VectorToJson(const FVector& Vec)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(Vec.X));
		Arr.Add(MakeShared<FJsonValueNumber>(Vec.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(Vec.Z));
		return MakeShared<FJsonValueArray>(Arr);
	}

	/** FRotator -> [pitch, yaw, roll] — the order every other Monolith camera field uses. */
	static TSharedPtr<FJsonValue> RotatorToJson(const FRotator& Rot)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(Rot.Pitch));
		Arr.Add(MakeShared<FJsonValueNumber>(Rot.Yaw));
		Arr.Add(MakeShared<FJsonValueNumber>(Rot.Roll));
		return MakeShared<FJsonValueArray>(Arr);
	}

	/**
	 * ELevelViewportType -> its source spelling (UnrealEdTypes.h:110-127).
	 * An explicit switch rather than StaticEnum<>()->GetNameStringByValue(): the
	 * enum defines value-aliases (LVT_OrthoTop == LVT_OrthoXY etc. at :129+), so a
	 * reflection lookup can legitimately return either spelling. Only the eight
	 * DISTINCT values are listed; anything else is reported numerically instead of
	 * being coerced to a plausible-looking wrong name.
	 */
	static FString ViewportTypeToString(ELevelViewportType Type)
	{
		switch (Type)
		{
		case LVT_OrthoXY:         return TEXT("LVT_OrthoXY");
		case LVT_OrthoXZ:         return TEXT("LVT_OrthoXZ");
		case LVT_OrthoYZ:         return TEXT("LVT_OrthoYZ");
		case LVT_Perspective:     return TEXT("LVT_Perspective");
		case LVT_OrthoFreelook:   return TEXT("LVT_OrthoFreelook");
		case LVT_OrthoNegativeXY: return TEXT("LVT_OrthoNegativeXY");
		case LVT_OrthoNegativeXZ: return TEXT("LVT_OrthoNegativeXZ");
		case LVT_OrthoNegativeYZ: return TEXT("LVT_OrthoNegativeYZ");
		default:                  return FString::Printf(TEXT("LVT_Unknown(%d)"), (int32)Type);
		}
	}

	/**
	 * Key B — which ASSET this viewport is previewing, resolved through the preview
	 * WORLD rather than through a widget cast (SNiagaraSystemViewport.h is a private
	 * header and is not includable from a plugin).
	 *
	 * Niagara only, deliberately: the Niagara editor adds its preview component to
	 * the scene via AdvancedPreviewScene->AddComponent(PreviewComponent, ...)
	 * (SNiagaraSystemViewport.cpp:1093), and UNiagaraComponent::GetAsset() is a
	 * public inline accessor (NiagaraComponent.h:298), so the match is exact and
	 * cheap. EXTENSION POINT: other asset editors would be added here as further
	 * TObjectIterator passes over their own component types — each one costs a new
	 * include, so they are added when there is a caller that needs them.
	 */
	static void CollectPreviewAssetPaths(UWorld* World, TArray<FString>& OutAssetPaths)
	{
		if (!World)
		{
			return;
		}

		for (TObjectIterator<UNiagaraComponent> It; It; ++It)
		{
			UNiagaraComponent* Component = *It;

			// IsValid() rejects null + pending-kill/garbage; the RF_ClassDefaultObject
			// test skips CDOs explicitly rather than relying on their world being null.
			if (!IsValid(Component) || Component->HasAnyFlags(RF_ClassDefaultObject))
			{
				continue;
			}
			if (Component->GetWorld() != World)
			{
				continue;
			}

			if (const UNiagaraSystem* System = Component->GetAsset())
			{
				OutAssetPaths.AddUnique(System->GetPathName());
			}
		}
	}

	// -------------------------------------------------------------------------
	// SHARED IDENTIFICATION. Both list_viewports (which REPORTS these keys) and
	// set_viewport_camera's `target` resolver (which MATCHES on them) go through
	// these three functions, so the printed key and the accepted key are the same
	// value by construction.
	// -------------------------------------------------------------------------

	/**
	 * Key A — the Slate widget's type name, or an empty string when the weak widget
	 * pointer has expired. GetEditorViewportWidget() (EditorViewportClient.h:1299)
	 * .Pin()s a TWeakPtr, so an invalid return is a normal state, not an error.
	 */
	static FString GetClientWidgetTypeName(FEditorViewportClient* Client)
	{
		if (!Client)
		{
			return FString();
		}
		const TSharedPtr<SEditorViewport> Widget = Client->GetEditorViewportWidget();
		return Widget.IsValid() ? Widget->GetType().ToString() : FString();
	}

	/**
	 * The world this client renders. For a PREVIEW client read the preview scene
	 * DIRECTLY: FEditorViewportClient::GetWorld() falls back to the global GWorld
	 * when the preview world is null (EditorViewportClient.cpp:4644-4657), which
	 * would report the LEVEL world for a preview viewport and look entirely
	 * plausible — and would then let an `asset:` target match the wrong client.
	 */
	static UWorld* GetClientWorld(FEditorViewportClient* Client)
	{
		if (!Client)
		{
			return nullptr;
		}
		if (FPreviewScene* PreviewScene = Client->GetPreviewScene())
		{
			return PreviewScene->GetWorld();
		}
		return Client->GetWorld();
	}

	/** "preview" iff the client carries an FPreviewScene, else "level". */
	static FString GetClientKind(FEditorViewportClient* Client)
	{
		return (Client && Client->GetPreviewScene() != nullptr) ? TEXT("preview") : TEXT("level");
	}

	/** Key B — the asset(s) previewed by this client, resolved through its world. */
	static void GetClientPreviewAssetPaths(FEditorViewportClient* Client, TArray<FString>& OutAssetPaths)
	{
		if (!Client || Client->GetPreviewScene() == nullptr)
		{
			return;
		}
		CollectPreviewAssetPaths(GetClientWorld(Client), OutAssetPaths);
	}

	/**
	 * One line describing a client, for the CANDIDATE LIST in an ambiguous- or
	 * unmatched-target error. Uses exactly the keys the caller can target with.
	 */
	static FString DescribeClientForTargeting(FEditorViewportClient* Client, int32 Index)
	{
		if (!Client)
		{
			return FString::Printf(TEXT("index:%d (null client)"), Index);
		}

		TArray<FString> AssetPaths;
		GetClientPreviewAssetPaths(Client, AssetPaths);

		return FString::Printf(TEXT("index:%d kind=%s widget=%s perspective=%s assets=[%s]"),
			Index,
			*GetClientKind(Client),
			*GetClientWidgetTypeName(Client),
			Client->IsPerspective() ? TEXT("true") : TEXT("false"),
			*FString::Join(AssetPaths, TEXT(", ")));
	}

	/**
	 * One FEditorViewportClient -> JSON. Every dereference is guarded and the guard's
	 * outcome is reported, so a caller can tell "absent" from "zero".
	 */
	static TSharedPtr<FJsonObject> ViewportClientToJson(FEditorViewportClient* Client, int32 Index)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("index"), Index);

		if (!Client)
		{
			// AllViewportClients is a RAW pointer array. A null slot is not expected,
			// but reporting one beats dereferencing it — and beats hiding it.
			Obj->SetBoolField(TEXT("valid"), false);
			return Obj;
		}
		Obj->SetBoolField(TEXT("valid"), true);

		// --- Key A: the Slate widget's type name. The weak pointer is .Pin()'d by the
		// --- accessor, so it can legitimately come back invalid.
		const FString WidgetType = GetClientWidgetTypeName(Client);
		Obj->SetBoolField(TEXT("has_widget"), Client->GetEditorViewportWidget().IsValid());
		Obj->SetStringField(TEXT("widget_type"), WidgetType);

		// --- kind: "preview" iff the client carries a preview scene.
		Obj->SetStringField(TEXT("kind"), GetClientKind(Client));

		// --- The world, read through the shared helper (which reads a preview
		// --- client's scene directly rather than through the GWorld-falling-back
		// --- FEditorViewportClient::GetWorld()).
		UWorld* World = GetClientWorld(Client);
		Obj->SetBoolField(TEXT("has_world"), World != nullptr);
		Obj->SetStringField(TEXT("world_path"), World ? World->GetPathName() : FString());

		// --- Key B: the asset(s) being previewed, via the world.
		TArray<FString> PreviewAssetPaths;
		GetClientPreviewAssetPaths(Client, PreviewAssetPaths);

		TArray<TSharedPtr<FJsonValue>> PreviewAssets;
		for (const FString& AssetPath : PreviewAssetPaths)
		{
			PreviewAssets.Add(MakeShared<FJsonValueString>(AssetPath));
		}
		Obj->SetArrayField(TEXT("preview_assets"), PreviewAssets);

		// --- Viewport type / state.
		Obj->SetStringField(TEXT("viewport_type"), ViewportTypeToString(Client->GetViewportType()));
		Obj->SetBoolField(TEXT("is_perspective"), Client->IsPerspective());
		Obj->SetBoolField(TEXT("is_realtime"), Client->IsRealtime());

		// IsVisible() consults VisibilityDelegate and returns FALSE when that delegate
		// is UNBOUND (EditorViewportClient.cpp:5826-5837) — which is the normal state
		// for a client not owned by a docked Slate layout. So false here means
		// "not reported visible", NOT necessarily "hidden".
		Obj->SetBoolField(TEXT("is_visible"), Client->IsVisible());

		// --- Render surface. Null for a client with no attached FViewport; its
		// --- GetSizeXY() would be an unguarded deref, so guard and report.
		FViewport* Viewport = Client->Viewport;
		const bool bHasViewport = (Viewport != nullptr);
		Obj->SetBoolField(TEXT("has_viewport"), bHasViewport);

		TArray<TSharedPtr<FJsonValue>> SizeArr;
		const FIntPoint Size = bHasViewport ? Viewport->GetSizeXY() : FIntPoint(0, 0);
		SizeArr.Add(MakeShared<FJsonValueNumber>(Size.X));
		SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Y));
		Obj->SetArrayField(TEXT("size"), SizeArr);

		// --- Camera, as STORED.
		const FVector  ViewLocation = Client->GetViewLocation();
		const FRotator ViewRotation = Client->GetViewRotation();
		const FVector  LookAt       = Client->GetLookAtLocation();
		const bool     bOrbit       = Client->bUsingOrbitCamera;

		Obj->SetBoolField(TEXT("using_orbit_camera"), bOrbit);
		Obj->SetField(TEXT("view_location"), VectorToJson(ViewLocation));
		Obj->SetField(TEXT("view_rotation"), RotatorToJson(ViewRotation));
		Obj->SetField(TEXT("look_at"), VectorToJson(LookAt));

		// Only the DISTANCE from LookAt survives into the orbit matrix
		// (EditorViewportClient.cpp:401) — the direction of ViewLocation is discarded.
		const double OrbitDistance = (ViewLocation - LookAt).Size();
		Obj->SetNumberField(TEXT("orbit_distance"), OrbitDistance);

		// --- Camera, as RENDERED. THE LOAD-BEARING FIELD.
		// Recomputed from the orbit matrix when orbiting — never an echo of
		// view_location. See the file header for the derivation.
		FVector  EffectiveLocation = ViewLocation;
		FRotator EffectiveRotation = ViewRotation;
		if (bOrbit)
		{
			// ComputeOrbitMatrix() is the full world->view matrix in orbit mode, so the
			// camera's world transform is the origin/rotation of its INVERSE. Same idiom
			// the engine uses at EditorViewportClient.h:541-546.
			const FMatrix InverseOrbit = Client->GetViewTransform().ComputeOrbitMatrix().Inverse();
			EffectiveLocation = InverseOrbit.GetOrigin();
			EffectiveRotation = InverseOrbit.Rotator();
		}
		Obj->SetField(TEXT("effective_camera_location"), VectorToJson(EffectiveLocation));
		Obj->SetField(TEXT("effective_camera_rotation"), RotatorToJson(EffectiveRotation));

		// True when the stored location is NOT where the camera actually renders from.
		// This is the single flag a harness should assert on before trusting a read-back.
		Obj->SetBoolField(TEXT("view_location_is_effective"),
			EffectiveLocation.Equals(ViewLocation, 0.01));

		// --- FOV. TWO fields, and they diverge: ViewFOV is current, FOVAngle is the
		// --- stored/ini value (EditorViewportClient.h:2009-2012). Epic's own setter
		// --- ULevelEditorSubsystem::SetLevelViewportFOV writes only ViewFOV
		// --- (LevelEditorSubsystem.cpp:396-407) while several engine paths restore
		// --- ViewFOV = FOVAngle behind the caller's back. Report BOTH so the
		// --- divergence is visible instead of being a silent surprise later.
		Obj->SetNumberField(TEXT("view_fov"), Client->ViewFOV);
		Obj->SetNumberField(TEXT("fov_angle"), Client->FOVAngle);
		Obj->SetBoolField(TEXT("fov_diverged"),
			!FMath::IsNearlyEqual(Client->ViewFOV, Client->FOVAngle, 0.01f));
		Obj->SetNumberField(TEXT("aspect_ratio"), Client->AspectRatio);

		return Obj;
	}
}

// ----- Mutator-only helpers --------------------------------------------------
//
// Separate block purely for readability; anonymous-namespace names declared above
// are in scope here, and the target resolver deliberately reuses them so the keys
// list_viewports PRINTS are the keys set_viewport_camera ACCEPTS.

namespace
{
	/** Every client, described the way the caller would target it. */
	static FString DescribeAllClientsForTargeting()
	{
		if (!GEditor)
		{
			return FString();
		}

		const TArray<FEditorViewportClient*>& Clients = GEditor->GetAllViewportClients();
		TArray<FString> Lines;
		Lines.Reserve(Clients.Num());
		for (int32 i = 0; i < Clients.Num(); ++i)
		{
			Lines.Add(DescribeClientForTargeting(Clients[i], i));
		}
		return FString::Join(Lines, TEXT(" | "));
	}

	/**
	 * Resolve a `target` string to exactly ONE live viewport client.
	 *
	 * Accepted forms — all four match on keys list_viewports reports:
	 *   "index:N"                        positional index in GetAllViewportClients()
	 *   "widget:SNiagaraSystemViewport"  the Slate type name (Key A)
	 *   "asset:/Game/FX/NS_Foo"          an asset in the client's preview world (Key B)
	 *   "level"                          the one PERSPECTIVE level viewport
	 *
	 * Returns nullptr and fills OutError on no match, on ambiguity, or on a
	 * malformed target. An ambiguous match NAMES THE CANDIDATES and never picks:
	 * quietly driving a different camera than the caller meant is indistinguishable
	 * from the action doing nothing.
	 */
	static FEditorViewportClient* ResolveViewportTarget(
		const FString& RawTarget,
		int32& OutIndex,
		FString& OutError)
	{
		OutIndex = INDEX_NONE;

		if (!GEditor)
		{
			OutError = TEXT("GEditor is null — no editor engine to resolve a viewport against.");
			return nullptr;
		}

		const FString Target = RawTarget.TrimStartAndEnd();
		if (Target.IsEmpty())
		{
			OutError = TEXT("'target' is required and must not be empty. Forms: \"index:N\" (from editor::list_viewports), \"widget:SNiagaraSystemViewport\", \"asset:/Game/Path/NS_Foo\", or \"level\".");
			return nullptr;
		}

		const TArray<FEditorViewportClient*>& Clients = GEditor->GetAllViewportClients();
		if (Clients.Num() == 0)
		{
			OutError = TEXT("GEditor->GetAllViewportClients() is EMPTY — there is no viewport to drive. Every FEditorViewportClient registers itself in its base constructor, so an empty list means no editor viewport exists yet.");
			return nullptr;
		}

		FString Scheme = Target;
		FString Value;
		if (!Target.Split(TEXT(":"), &Scheme, &Value, ESearchCase::IgnoreCase, ESearchDir::FromStart))
		{
			Scheme = Target;
			Value.Reset();
		}
		Scheme = Scheme.TrimStartAndEnd();
		Value = Value.TrimStartAndEnd();

		// --- "index:N" -------------------------------------------------------
		if (Scheme.Equals(TEXT("index"), ESearchCase::IgnoreCase))
		{
			if (Value.IsEmpty() || !Value.IsNumeric())
			{
				OutError = FString::Printf(
					TEXT("'target' index is not a number: \"%s\". Expected \"index:N\" with N from editor::list_viewports."),
					*Target);
				return nullptr;
			}

			const int32 Index = FCString::Atoi(*Value);
			if (!Clients.IsValidIndex(Index))
			{
				OutError = FString::Printf(
					TEXT("No viewport at index %d — there are %d (valid 0..%d). Indices are POSITIONAL in GEditor->GetAllViewportClients() and RENUMBER whenever any editor closes, so re-read editor::list_viewports immediately before targeting. Candidates: %s"),
					Index, Clients.Num(), Clients.Num() - 1, *DescribeAllClientsForTargeting());
				return nullptr;
			}
			if (Clients[Index] == nullptr)
			{
				OutError = FString::Printf(TEXT("The viewport client at index %d is null."), Index);
				return nullptr;
			}

			OutIndex = Index;
			return Clients[Index];
		}

		// --- Everything else: collect matches, then insist on exactly one. ----
		TArray<int32> Matches;

		if (Scheme.Equals(TEXT("widget"), ESearchCase::IgnoreCase))
		{
			if (Value.IsEmpty())
			{
				OutError = TEXT("'target' widget name is empty. Expected \"widget:SNiagaraSystemViewport\" — the widget_type reported by editor::list_viewports.");
				return nullptr;
			}

			// Exact (case-insensitive) first. Only if nothing matches exactly do we
			// fall back to a substring test, and the response says which was used —
			// a loose match that silently beat an exact one would be a trap.
			for (int32 i = 0; i < Clients.Num(); ++i)
			{
				if (Clients[i] && GetClientWidgetTypeName(Clients[i]).Equals(Value, ESearchCase::IgnoreCase))
				{
					Matches.Add(i);
				}
			}
			if (Matches.Num() == 0)
			{
				for (int32 i = 0; i < Clients.Num(); ++i)
				{
					const FString WidgetType = GetClientWidgetTypeName(Clients[i]);
					if (Clients[i] && !WidgetType.IsEmpty() && WidgetType.Contains(Value, ESearchCase::IgnoreCase))
					{
						Matches.Add(i);
					}
				}
			}
		}
		else if (Scheme.Equals(TEXT("asset"), ESearchCase::IgnoreCase))
		{
			if (Value.IsEmpty())
			{
				OutError = TEXT("'target' asset path is empty. Expected \"asset:/Game/Path/NS_Foo\".");
				return nullptr;
			}

			// Accept the path with or without the trailing \".AssetName\" object
			// suffix: list_viewports reports the full object path, but callers type
			// the package path far more often, and refusing that would be pedantry.
			FString ValuePackage = Value;
			{
				FString Left, Right;
				if (Value.Split(TEXT("."), &Left, &Right, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
				{
					ValuePackage = Left;
				}
			}

			for (int32 i = 0; i < Clients.Num(); ++i)
			{
				TArray<FString> AssetPaths;
				GetClientPreviewAssetPaths(Clients[i], AssetPaths);

				for (const FString& AssetPath : AssetPaths)
				{
					FString AssetPackage = AssetPath;
					FString Left, Right;
					if (AssetPath.Split(TEXT("."), &Left, &Right, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
					{
						AssetPackage = Left;
					}

					if (AssetPath.Equals(Value, ESearchCase::IgnoreCase)
						|| AssetPackage.Equals(ValuePackage, ESearchCase::IgnoreCase))
					{
						Matches.Add(i);
						break;
					}
				}
			}
		}
		else if (Scheme.Equals(TEXT("level"), ESearchCase::IgnoreCase) && Value.IsEmpty())
		{
			// The PERSPECTIVE level viewport, not GetLevelViewportClients()[0] —
			// index 0 is an arbitrary pane and in a 4-pane layout is very likely an
			// ortho one whose hidden FViewport reports a size of {0,0} (gap #120).
			// Epic's own lookup filters on IsPerspective() too
			// (UnrealEditorSubsystem.cpp:132-142).
			for (int32 i = 0; i < Clients.Num(); ++i)
			{
				if (Clients[i] && Clients[i]->GetPreviewScene() == nullptr && Clients[i]->IsPerspective())
				{
					Matches.Add(i);
				}
			}
		}
		else
		{
			OutError = FString::Printf(
				TEXT("Unrecognised 'target' form \"%s\". Use \"index:N\" (from editor::list_viewports), \"widget:SNiagaraSystemViewport\", \"asset:/Game/Path/NS_Foo\", or \"level\"."),
				*Target);
			return nullptr;
		}

		if (Matches.Num() == 0)
		{
			OutError = FString::Printf(
				TEXT("No viewport matched target \"%s\". Candidates: %s"),
				*Target, *DescribeAllClientsForTargeting());
			return nullptr;
		}
		if (Matches.Num() > 1)
		{
			TArray<FString> Lines;
			Lines.Reserve(Matches.Num());
			for (const int32 Index : Matches)
			{
				Lines.Add(DescribeClientForTargeting(Clients[Index], Index));
			}
			OutError = FString::Printf(
				TEXT("Target \"%s\" is AMBIGUOUS — %d viewports match, and this action will not pick one for you. Re-target by index. Matches: %s"),
				*Target, Matches.Num(), *FString::Join(Lines, TEXT(" | ")));
			return nullptr;
		}

		OutIndex = Matches[0];
		return Clients[Matches[0]];
	}

	// -------------------------------------------------------------------------
	// NAMED ANGLE PRESETS
	//
	// Yaw depends on whether the client is ORBITING, because the two modes
	// parameterise azimuth differently (see the file header). Pitch does not: the
	// vertical component is -sin(Pitch) in both.
	//
	// Every axis preset below was checked by rebuilding the full rotation chain
	// and comparing it against the engine's OWN ortho view matrices at
	// EditorViewportClient.cpp:1341-1388 — all six reproduce them exactly,
	// in-plane roll included. persp45 has no engine counterpart and is a choice:
	// halfway between `front` and `right`, looking down 45 degrees.
	//
	// ⚠️ top/bottom use pitch ±90 exactly. The orbit matrix is composed from
	// FTransforms, so there is no gimbal degeneracy in what RENDERS; the only
	// casualty is that the REPORTED effective_camera_rotation decomposition of a
	// straight-down view is not unique. Yaw still matters at ±90: it does not move
	// the camera (cos(±90) == 0) but it does roll the image, and the values below
	// are the ones that match the editor's own Top/Bottom framing.
	// -------------------------------------------------------------------------
	struct FViewAnglePreset
	{
		const TCHAR* Name;
		double       Pitch;
		double       YawOrbiting;
		double       YawNotOrbiting;
		const TCHAR* CameraSits;
	};

	static const FViewAnglePreset GViewAnglePresets[] =
	{
		{ TEXT("front"),   0.0,   -90.0,  180.0,  TEXT("+X") },
		{ TEXT("back"),    0.0,    90.0,    0.0,  TEXT("-X") },
		{ TEXT("right"),   0.0,   180.0,  -90.0,  TEXT("+Y") },
		{ TEXT("left"),    0.0,     0.0,   90.0,  TEXT("-Y") },
		{ TEXT("top"),   -90.0,   -90.0,  180.0,  TEXT("+Z") },
		{ TEXT("bottom"), 90.0,    90.0,    0.0,  TEXT("-Z") },
		{ TEXT("persp45"), -45.0, -135.0, -135.0, TEXT("+X+Y, above") },
	};

	static const FViewAnglePreset* FindViewAnglePreset(const FString& Name)
	{
		for (const FViewAnglePreset& Preset : GViewAnglePresets)
		{
			if (Name.Equals(Preset.Name, ESearchCase::IgnoreCase))
			{
				return &Preset;
			}
		}
		return nullptr;
	}

	static FString JoinViewAnglePresetNames()
	{
		TArray<FString> Names;
		for (const FViewAnglePreset& Preset : GViewAnglePresets)
		{
			Names.Add(Preset.Name);
		}
		return FString::Join(Names, TEXT(", "));
	}

	// ----- Param readers. Absent is not an error; present-but-wrong-type is. ----

	/** Absent/null -> bOutPresent stays false, returns true. */
	static bool ReadBoolParam(
		const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool& OutValue, bool& bOutPresent)
	{
		bOutPresent = false;
		if (!Params.IsValid())
		{
			return true;
		}

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

	static bool ReadNumberParam(
		const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, double& OutValue, bool& bOutPresent)
	{
		bOutPresent = false;
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
		if (!Field.IsValid() || Field->Type == EJson::Null)
		{
			return true;
		}

		double AsNumber = 0.0;
		if (Field->TryGetNumber(AsNumber))
		{
			OutValue = AsNumber;
			bOutPresent = true;
			return true;
		}

		FString AsString;
		if (Field->TryGetString(AsString) && !AsString.TrimStartAndEnd().IsEmpty())
		{
			OutValue = FCString::Atod(*AsString.TrimStartAndEnd());
			bOutPresent = true;
			return true;
		}

		return false;
	}

	/** [x,y,z] (or {x,y,z}). Absent -> bOutPresent false. Malformed -> false + error. */
	static bool ReadVectorParam(
		const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName,
		FVector& OutValue, bool& bOutPresent, FString& OutError)
	{
		bOutPresent = false;
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
		if (!Field.IsValid() || Field->Type == EJson::Null)
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Field->TryGetArray(Arr) && Arr)
		{
			if (Arr->Num() != 3)
			{
				OutError = FString::Printf(
					TEXT("'%s' must be a 3-element array [x,y,z] (got %d element(s))."), FieldName, Arr->Num());
				return false;
			}
			for (int32 i = 0; i < 3; ++i)
			{
				double Component = 0.0;
				if (!(*Arr)[i].IsValid() || !(*Arr)[i]->TryGetNumber(Component))
				{
					OutError = FString::Printf(TEXT("'%s'[%d] is not a number."), FieldName, i);
					return false;
				}
				OutValue[i] = Component;
			}
			bOutPresent = true;
			return true;
		}

		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Field->TryGetObject(Obj) && Obj && (*Obj).IsValid())
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			const bool bGotX = (*Obj)->TryGetNumberField(TEXT("x"), X) || (*Obj)->TryGetNumberField(TEXT("X"), X);
			const bool bGotY = (*Obj)->TryGetNumberField(TEXT("y"), Y) || (*Obj)->TryGetNumberField(TEXT("Y"), Y);
			const bool bGotZ = (*Obj)->TryGetNumberField(TEXT("z"), Z) || (*Obj)->TryGetNumberField(TEXT("Z"), Z);
			if (!bGotX || !bGotY || !bGotZ)
			{
				OutError = FString::Printf(TEXT("'%s' object form needs all three of x, y, z."), FieldName);
				return false;
			}
			OutValue = FVector(X, Y, Z);
			bOutPresent = true;
			return true;
		}

		OutError = FString::Printf(TEXT("'%s' must be [x,y,z] or {x,y,z}."), FieldName);
		return false;
	}

	/** [pitch,yaw,roll] — the order every other Monolith camera field uses. */
	static bool ReadRotatorParam(
		const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName,
		FRotator& OutValue, bool& bOutPresent, FString& OutError)
	{
		FVector AsVector(0.0);
		if (!ReadVectorParam(Params, FieldName, AsVector, bOutPresent, OutError))
		{
			OutError = FString::Printf(
				TEXT("'%s' must be a 3-element array [pitch,yaw,roll]. (%s)"), FieldName, *OutError);
			return false;
		}
		if (bOutPresent)
		{
			OutValue = FRotator(AsVector.X, AsVector.Y, AsVector.Z);
		}
		return true;
	}
}

// =============================================================================
// editor::list_viewports
// =============================================================================

FMonolithActionResult FMonolithEditorActions::HandleListViewports(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(
			TEXT("GEditor is null — no editor engine to enumerate viewports from."));
	}

	// Header-inline accessor over the engine-owned registry every
	// FEditorViewportClient adds itself to in its base constructor
	// (EditorEngine.h:718-719; EditorViewportClient.cpp:601).
	const TArray<FEditorViewportClient*>& Clients = GEditor->GetAllViewportClients();

	TArray<TSharedPtr<FJsonValue>> ViewportValues;
	ViewportValues.Reserve(Clients.Num());

	int32 PreviewCount = 0;
	int32 LevelCount = 0;

	for (int32 i = 0; i < Clients.Num(); ++i)
	{
		FEditorViewportClient* Client = Clients[i];
		if (Client)
		{
			if (Client->GetPreviewScene() != nullptr)
			{
				++PreviewCount;
			}
			else
			{
				++LevelCount;
			}
		}
		ViewportValues.Add(MakeShared<FJsonValueObject>(ViewportClientToJson(Client, i)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("viewport_count"), Clients.Num());
	Result->SetNumberField(TEXT("preview_viewport_count"), PreviewCount);
	Result->SetNumberField(TEXT("level_viewport_count"), LevelCount);
	Result->SetArrayField(TEXT("viewports"), ViewportValues);

	// For cross-checking against editor::get_viewport_info, which reads the SEPARATE
	// LevelViewportClients array (a subset of this one).
	Result->SetNumberField(TEXT("level_viewport_clients_num"), GEditor->GetLevelViewportClients().Num());

	if (Clients.Num() == 0)
	{
		FMonolithJsonUtils::AddWarning(Result,
			TEXT("GEditor->GetAllViewportClients() is EMPTY. Every FEditorViewportClient registers itself in its base constructor (EditorViewportClient.cpp:601), so an empty list means no editor viewport exists yet — e.g. the editor is still starting up, or this is a commandlet/-nullrhi run."));
	}
	if (PreviewCount == 0 && Clients.Num() > 0)
	{
		FMonolithJsonUtils::AddWarning(Result,
			TEXT("No PREVIEW viewport client is registered — only level viewports. Open the asset editor you intend to target (e.g. a Niagara system) and call this again; a preview viewport only exists while its asset editor is open."));
	}

	UE_LOG(LogMonolithViewportCamera, Log,
		TEXT("list_viewports: %d client(s) — %d preview, %d level."),
		Clients.Num(), PreviewCount, LevelCount);

	return FMonolithActionResult::Success(Result);
}

// =============================================================================
// editor::set_viewport_camera
// =============================================================================

FMonolithActionResult FMonolithEditorActions::HandleSetViewportCamera(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(
			TEXT("GEditor is null — no editor engine to drive a viewport camera in."));
	}
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(
			TEXT("No parameters supplied. 'target' is required."),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	// --- Target -------------------------------------------------------------
	FString TargetString;
	if (!Params->TryGetStringField(TEXT("target"), TargetString))
	{
		return FMonolithActionResult::Error(
			TEXT("'target' is required. Forms: \"index:N\" (from editor::list_viewports), \"widget:SNiagaraSystemViewport\", \"asset:/Game/Path/NS_Foo\", or \"level\"."),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	int32 ResolvedIndex = INDEX_NONE;
	FString ResolveError;
	FEditorViewportClient* Client = ResolveViewportTarget(TargetString, ResolvedIndex, ResolveError);
	if (!Client)
	{
		return FMonolithActionResult::Error(ResolveError, FMonolithJsonUtils::ErrInvalidParams);
	}

	// --- Parse EVERYTHING before touching anything, so a bad param cannot leave
	// --- a half-moved camera behind (the same discipline set_preview_scene uses).
	FString ParseError;

	FString AngleName;
	const bool bHasAngle = Params->TryGetStringField(TEXT("angle"), AngleName) && !AngleName.TrimStartAndEnd().IsEmpty();
	AngleName = AngleName.TrimStartAndEnd();

	const FViewAnglePreset* Preset = nullptr;
	if (bHasAngle)
	{
		Preset = FindViewAnglePreset(AngleName);
		if (!Preset)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Unknown angle preset \"%s\". Valid: %s. (Or pass raw 'yaw'/'pitch' instead.)"),
				*AngleName, *JoinViewAnglePresetNames()),
				FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	double Yaw = 0.0;   bool bHasYaw = false;
	double Pitch = 0.0; bool bHasPitch = false;
	if (!ReadNumberParam(Params, TEXT("yaw"), Yaw, bHasYaw))
	{
		return FMonolithActionResult::Error(TEXT("'yaw' must be a number (degrees)."), FMonolithJsonUtils::ErrInvalidParams);
	}
	if (!ReadNumberParam(Params, TEXT("pitch"), Pitch, bHasPitch))
	{
		return FMonolithActionResult::Error(TEXT("'pitch' must be a number (degrees)."), FMonolithJsonUtils::ErrInvalidParams);
	}

	FRotator RotationParam = FRotator::ZeroRotator;
	bool bHasRotation = false;
	if (!ReadRotatorParam(Params, TEXT("rotation"), RotationParam, bHasRotation, ParseError))
	{
		return FMonolithActionResult::Error(ParseError, FMonolithJsonUtils::ErrInvalidParams);
	}

	FVector LookAtParam = FVector::ZeroVector;
	bool bHasLookAt = false;
	if (!ReadVectorParam(Params, TEXT("look_at"), LookAtParam, bHasLookAt, ParseError))
	{
		return FMonolithActionResult::Error(ParseError, FMonolithJsonUtils::ErrInvalidParams);
	}

	bool bResetLookAt = false, bHasResetLookAt = false;
	if (!ReadBoolParam(Params, TEXT("reset_look_at"), bResetLookAt, bHasResetLookAt))
	{
		return FMonolithActionResult::Error(TEXT("'reset_look_at' must be a bool."), FMonolithJsonUtils::ErrInvalidParams);
	}
	const bool bWantsResetLookAt = bHasResetLookAt && bResetLookAt;

	double DistanceParam = 0.0; bool bHasDistance = false;
	if (!ReadNumberParam(Params, TEXT("distance"), DistanceParam, bHasDistance))
	{
		return FMonolithActionResult::Error(TEXT("'distance' must be a number."), FMonolithJsonUtils::ErrInvalidParams);
	}
	if (bHasDistance && DistanceParam <= 0.0)
	{
		return FMonolithActionResult::Error(
			TEXT("'distance' must be greater than zero — a zero radius puts the camera exactly on the pivot and renders nothing."),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	FVector LocationParam = FVector::ZeroVector;
	bool bHasLocation = false;
	if (!ReadVectorParam(Params, TEXT("location"), LocationParam, bHasLocation, ParseError))
	{
		return FMonolithActionResult::Error(ParseError, FMonolithJsonUtils::ErrInvalidParams);
	}

	double FovParam = 0.0; bool bHasFov = false;
	if (!ReadNumberParam(Params, TEXT("fov"), FovParam, bHasFov))
	{
		return FMonolithActionResult::Error(TEXT("'fov' must be a number (degrees)."), FMonolithJsonUtils::ErrInvalidParams);
	}
	if (bHasFov && (FovParam < 5.0 || FovParam > 170.0))
	{
		// Epic clamps to this range rather than refusing (LevelEditorSubsystem.cpp:396-407).
		// We refuse instead: a silently clamped value is a value the caller did not set.
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("'fov' %.3f is outside the editor's supported range 5..170 degrees."), FovParam),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	bool bOrbitMode = false, bHasOrbitMode = false;
	if (!ReadBoolParam(Params, TEXT("orbit_mode"), bOrbitMode, bHasOrbitMode))
	{
		return FMonolithActionResult::Error(TEXT("'orbit_mode' must be a bool."), FMonolithJsonUtils::ErrInvalidParams);
	}

	bool bRealtime = false, bHasRealtime = false;
	if (!ReadBoolParam(Params, TEXT("realtime"), bRealtime, bHasRealtime))
	{
		return FMonolithActionResult::Error(TEXT("'realtime' must be a bool."), FMonolithJsonUtils::ErrInvalidParams);
	}

	bool bInvalidate = true, bHasInvalidate = false;
	if (!ReadBoolParam(Params, TEXT("invalidate"), bInvalidate, bHasInvalidate))
	{
		return FMonolithActionResult::Error(TEXT("'invalidate' must be a bool."), FMonolithJsonUtils::ErrInvalidParams);
	}

	// --- Coherence checks ---------------------------------------------------
	const bool bHasAnyRotationInput = bHasAngle || bHasYaw || bHasPitch || bHasRotation;

	if (bHasRotation && (bHasAngle || bHasYaw || bHasPitch))
	{
		return FMonolithActionResult::Error(
			TEXT("'rotation' is mutually exclusive with 'angle' / 'yaw' / 'pitch' — pass a full [pitch,yaw,roll] OR a preset/components, not both. (There is no defensible precedence between them, and guessing one would silently discard the other.)"),
			FMonolithJsonUtils::ErrInvalidParams);
	}
	if (bHasLocation && (bHasLookAt || bWantsResetLookAt || bHasDistance))
	{
		return FMonolithActionResult::Error(
			TEXT("'location' is mutually exclusive with 'look_at' / 'reset_look_at' / 'distance' — those place the camera relative to a pivot, 'location' places it absolutely."),
			FMonolithJsonUtils::ErrInvalidParams);
	}
	if (bHasLookAt && bWantsResetLookAt && !LookAtParam.IsNearlyZero())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("'reset_look_at' is true but 'look_at' is [%g, %g, %g], not the origin. Pass one or the other."),
			LookAtParam.X, LookAtParam.Y, LookAtParam.Z),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	const bool bAnyMutation =
		bHasAnyRotationInput || bHasLookAt || bWantsResetLookAt || bHasDistance ||
		bHasLocation || bHasFov || bHasOrbitMode || bHasRealtime;

	if (!bAnyMutation)
	{
		// Never a silent no-op.
		return FMonolithActionResult::Error(
			TEXT("No mutating parameter supplied (angle, yaw, pitch, rotation, look_at, reset_look_at, distance, location, fov, orbit_mode, realtime) — nothing to do. 'invalidate' alone only requests a redraw of an unchanged camera. Use editor::list_viewports to read the current state."),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	TArray<FString> Warnings;
	TArray<FString> Applied;

	// --- 1. Orbit mode FIRST. ToggleOrbitCamera rewrites ViewRotation from the
	// --- orbit matrix (EditorViewportClient.cpp:904-920), so anything we write
	// --- before it would be discarded — and the preset yaw depends on the mode
	// --- we end up in, not the one we started in.
	const bool bWasOrbiting = Client->bUsingOrbitCamera;
	if (bHasOrbitMode && bOrbitMode != bWasOrbiting)
	{
		Client->ToggleOrbitCamera(bOrbitMode);
		Applied.Add(TEXT("orbit_mode"));
		Warnings.Add(FString::Printf(
			TEXT("orbit_mode was toggled %s->%s. ToggleOrbitCamera REWRITES ViewRotation from the orbit matrix (EditorViewportClient.cpp:904-920), so the camera visibly jumps and any rotation read before this call is stale."),
			bWasOrbiting ? TEXT("true") : TEXT("false"),
			bOrbitMode ? TEXT("true") : TEXT("false")));
	}

	const bool bOrbitNow = Client->bUsingOrbitCamera;

	// --- 2. A raw `location` is only honourable when NOT orbiting.
	if (bHasLocation && bOrbitNow)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Viewport %d is ORBITING, and in orbit mode SetViewLocation() cannot place the camera: only the DISTANCE |view_location - look_at| survives into the rendered view (EditorViewportClient.cpp:401), while GetViewLocation() still returns your value verbatim — so the write would round-trip perfectly and move the camera only radially. Use 'look_at' + 'distance' + 'angle' (or raw 'yaw'/'pitch') instead. Passing orbit_mode:false would also work, but it rewrites ViewRotation and jumps the camera, so this action will not do it for you."),
			ResolvedIndex),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	// --- 3. Resolve the pivot and the radius from the PRE-MOVE state.
	const FVector  PreLookAt      = Client->GetLookAtLocation();
	const FVector  PreViewLocation = Client->GetViewLocation();
	const FRotator PreViewRotation = Client->GetViewRotation();

	FVector LookAt = PreLookAt;
	if (bWantsResetLookAt)
	{
		LookAt = FVector::ZeroVector;
	}
	if (bHasLookAt)
	{
		LookAt = LookAtParam;
	}

	double Distance = DistanceParam;
	if (!bHasDistance)
	{
		Distance = (PreViewLocation - PreLookAt).Size();
		if (Distance < 1.0)
		{
			// Degenerate radius — e.g. after ULevelEditorSubsystem::SetLevelViewportCameraInfo,
			// which leaves LookAt equal to the camera's own position
			// (LevelEditorSubsystem.cpp:364-377). Keeping it would park the camera
			// inside the pivot and render nothing.
			Distance = 256.0; // the engine's own default (EditorViewportClient.h:1192)
			Warnings.Add(FString::Printf(
				TEXT("'distance' was not supplied and the viewport's current radius |view_location - look_at| was %.4f (degenerate). Fell back to the engine default of 256. Pass 'distance' explicitly to control it."),
				(PreViewLocation - PreLookAt).Size()));
		}
	}

	// --- 4. Rotation. Preset first, then explicit components override it.
	FRotator NewRotation = PreViewRotation;
	FString PresetYawSource;
	if (bHasRotation)
	{
		NewRotation = RotationParam;
	}
	else
	{
		if (Preset)
		{
			NewRotation.Pitch = Preset->Pitch;
			NewRotation.Yaw   = bOrbitNow ? Preset->YawOrbiting : Preset->YawNotOrbiting;
			NewRotation.Roll  = 0.0;
			PresetYawSource   = bOrbitNow ? TEXT("orbiting") : TEXT("not_orbiting");
		}
		if (bHasPitch)
		{
			NewRotation.Pitch = Pitch;
		}
		if (bHasYaw)
		{
			NewRotation.Yaw = Yaw;
		}
	}

	if (bHasAnyRotationInput)
	{
		// MUST precede SetViewLocationForOrbiting, which derives its direction from
		// the CURRENT ViewRotation (EditorViewportClient.cpp:878-882).
		Client->SetViewRotation(NewRotation);
		Applied.Add(TEXT("rotation"));

		if (bOrbitNow && !FMath::IsNearlyZero(NewRotation.Roll))
		{
			Warnings.Add(TEXT("Roll is DISCARDED while orbiting — ComputeOrbitMatrix uses only Yaw and Pitch (EditorViewportClient.cpp:399-400). The value was stored but does not affect what renders."));
		}
	}

	// --- 5. Placement.
	const bool bPlaceByOrbit =
		!bHasLocation && (bHasAnyRotationInput || bHasLookAt || bWantsResetLookAt || bHasDistance);

	if (bHasLocation)
	{
		// Only reachable when NOT orbiting (refused above).
		Client->SetViewLocation(LocationParam);
		Applied.Add(TEXT("location"));
	}
	else if (bPlaceByOrbit)
	{
		// The one placement path: correct in BOTH modes. Orbiting, it sets the pivot
		// and the radius ComputeOrbitMatrix reads; not orbiting, it puts the camera
		// at LookAt - Distance * Forward(ViewRotation), i.e. looking at the pivot.
		Client->SetViewLocationForOrbiting(LookAt, (float)Distance);
		Applied.Add(TEXT("look_at"));
		Applied.Add(TEXT("distance"));
	}

	// --- 6. FOV — BOTH fields (EditorViewportClient.h:2009-2012).
	if (bHasFov)
	{
		Client->ViewFOV  = (float)FovParam;
		Client->FOVAngle = (float)FovParam;
		Applied.Add(TEXT("fov"));
	}

	// --- 7. Realtime.
	if (bHasRealtime)
	{
		Client->SetRealtime(bRealtime);
		Applied.Add(TEXT("realtime"));
		Warnings.Add(TEXT("'realtime' is PERSISTED between editor sessions by the engine (EditorViewportClient.h:407-411) — it is not a temporary override."));
	}

	// --- 8. Invalidate the client WE touched. Neither engine camera setter does,
	// --- and ULevelEditorSubsystem::EditorInvalidateViewports() would redraw the
	// --- ACTIVE viewport instead (LevelEditorSubsystem.cpp:211-221).
	// --- FEditorViewportClient::Invalidate null-guards Viewport itself (cpp:6624).
	if (bInvalidate)
	{
		Client->Invalidate();
	}

	// --- 9. Re-read THROUGH THE SAME CODE list_viewports uses. A genuine read-back
	// --- of the post-write state, not an echo of the request.
	TSharedPtr<FJsonObject> ViewportJson = ViewportClientToJson(Client, ResolvedIndex);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("target"), TargetString);
	Result->SetNumberField(TEXT("resolved_index"), ResolvedIndex);
	Result->SetObjectField(TEXT("viewport"), ViewportJson);

	// Also as a one-element array so a caller can reuse its list_viewports parsing.
	TArray<TSharedPtr<FJsonValue>> ViewportValues;
	ViewportValues.Add(MakeShared<FJsonValueObject>(ViewportJson));
	Result->SetArrayField(TEXT("viewports"), ViewportValues);
	Result->SetNumberField(TEXT("viewport_count"), 1);

	TArray<TSharedPtr<FJsonValue>> AppliedValues;
	for (const FString& Field : Applied)
	{
		AppliedValues.Add(MakeShared<FJsonValueString>(Field));
	}
	Result->SetArrayField(TEXT("applied"), AppliedValues);
	Result->SetBoolField(TEXT("invalidated"), bInvalidate);
	Result->SetBoolField(TEXT("was_orbiting"), bWasOrbiting);
	Result->SetBoolField(TEXT("is_orbiting"), bOrbitNow);

	// What the request actually resolved to, so the maths is checkable from the
	// response alone rather than by re-deriving it.
	Result->SetStringField(TEXT("angle"), bHasAngle ? AngleName : FString());
	Result->SetStringField(TEXT("preset_yaw_source"), PresetYawSource);
	Result->SetStringField(TEXT("preset_camera_sits"), Preset ? Preset->CameraSits : TEXT(""));
	Result->SetField(TEXT("requested_view_rotation"), RotatorToJson(NewRotation));
	Result->SetField(TEXT("requested_look_at"), VectorToJson(bHasLocation ? PreLookAt : LookAt));
	Result->SetNumberField(TEXT("requested_distance"), bHasLocation ? (PreViewLocation - PreLookAt).Size() : Distance);

	// --- Cautions that only make sense once the write is done.
	if (!Client->IsPerspective())
	{
		Warnings.Add(FString::Printf(
			TEXT("Viewport %d is ORTHOGRAPHIC (%s). Its view-rotation matrix is FIXED by the viewport type (EditorViewportClient.cpp:1341-1388) and IGNORES ViewRotation, so 'angle'/'yaw'/'pitch' changed the stored value without changing what renders. The location/pivot did move. Angle presets are meaningful only on a perspective viewport."),
			ResolvedIndex, *ViewportTypeToString(Client->GetViewportType())));
	}

	bool bLocationIsEffective = true;
	ViewportJson->TryGetBoolField(TEXT("view_location_is_effective"), bLocationIsEffective);
	if (bOrbitNow && !bLocationIsEffective)
	{
		Warnings.Add(TEXT("view_location_is_effective is FALSE: this viewport is orbiting, so 'view_location' is NOT where the camera renders from. Read 'effective_camera_location' — that is the recomputed rendered position. This is expected at every azimuth except yaw -135 / +45, where the two coincide."));
	}

	const FString WidgetType = GetClientWidgetTypeName(Client);
	if (Client->GetPreviewScene() != nullptr && WidgetType.Contains(TEXT("Niagara")))
	{
		Warnings.Add(TEXT("This is a NIAGARA preview viewport, which RE-FOCUSES ITSELF once per preview-component rebuild and overwrites both location and rotation (SNiagaraSystemViewport.cpp:194-214, armed at :1095 by SetPreviewComponent). The refocus is deferred to the next Draw where the sim age > 0 and bounds are valid, so a reset/recompile after this call — or simply this call landing before the system starts — will silently lose the camera. Set the camera AFTER the system is running and re-read editor::list_viewports immediately before capturing."));
	}

	FMonolithJsonUtils::AddWarnings(Result, Warnings);

	UE_LOG(LogMonolithViewportCamera, Log,
		TEXT("set_viewport_camera: index %d (%s) — applied [%s], orbit %s, look_at (%s), distance %.3f, rotation (P=%.3f Y=%.3f R=%.3f)."),
		ResolvedIndex, *WidgetType, *FString::Join(Applied, TEXT(",")),
		bOrbitNow ? TEXT("on") : TEXT("off"),
		*LookAt.ToString(), Distance,
		NewRotation.Pitch, NewRotation.Yaw, NewRotation.Roll);

	return FMonolithActionResult::Success(Result);
}
