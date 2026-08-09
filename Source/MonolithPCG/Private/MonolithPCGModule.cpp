#include "MonolithPCGModule.h"
#include "MonolithPCGActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"

#define LOCTEXT_NAMESPACE "FMonolithPCGModule"

void FMonolithPCGModule::StartupModule()
{
	// NOTE (upstream follow-up): every other domain module gates registration on a
	// `UMonolithSettings::bEnable<Domain>` toggle. MonolithPCG deliberately does NOT,
	// because adding `bEnablePCG` means editing MonolithCore/Public/MonolithSettings.h,
	// which was outside this change's scope. The gate should be added before upstreaming:
	//     if (!GetDefault<UMonolithSettings>()->bEnablePCG) return;
	// Behaviour today: the `pcg` namespace registers whenever the module loads, and the
	// module only loads when the PCG plugin is present (see MonolithPCG.Build.cs).

#if WITH_PCG
	FMonolithPCGActions::RegisterActions(FMonolithToolRegistry::Get());
	UE_LOG(LogMonolith, Log, TEXT("Monolith — PCG module loaded (7 actions)"));
#else
	UE_LOG(LogMonolith, Log, TEXT("Monolith — PCG module loaded with WITH_PCG=0; no pcg actions registered (PCG plugin not found)"));
#endif
}

void FMonolithPCGModule::ShutdownModule()
{
#if WITH_PCG
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("pcg"));
#endif
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithPCGModule, MonolithPCG)
