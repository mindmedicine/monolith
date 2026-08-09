using UnrealBuildTool;
using System.IO;

public class MonolithPCG : ModuleRules
{
	public MonolithPCG(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// PCG ships as an engine plugin (Engine/Plugins/PCG) from 5.2 onward, but it is
		// not enabled in every project. Follow the MonolithComboGraph pattern: probe for
		// the plugin, compile the real actions only when it is present, and fall back to
		// an empty module otherwise so a Monolith build never hard-fails on a missing
		// optional dependency.
		//
		// Note: presence on disk is NOT the same as "enabled for this target". The
		// Monolith.uplugin carries a matching `{ "Name": "PCG", "Optional": true }`
		// entry, which is what actually enables it; without that entry UBT rejects the
		// PCG module reference even though the directory exists.
		bool bHasPCG = false;
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		if (!bReleaseBuild)
		{
			string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
			bHasPCG = Directory.Exists(Path.Combine(EngineDir, "Plugins", "PCG"));

			if (!bHasPCG && Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins");
				if (Directory.Exists(ProjectPluginsDir))
				{
					bHasPCG = Directory.Exists(Path.Combine(ProjectPluginsDir, "PCG"));
				}
			}
		}

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		if (bHasPCG)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"MonolithCore",
				"UnrealEd",
				"EditorScriptingUtilities",
				"AssetTools",
				"AssetRegistry",
				"Slate",
				"SlateCore",
				"Json",
				"JsonUtilities",
				"PCG"
			});
			PublicDefinitions.Add("WITH_PCG=1");
		}
		else
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"MonolithCore",
				"UnrealEd",
				"Json",
				"JsonUtilities"
			});
			PublicDefinitions.Add("WITH_PCG=0");
		}
	}
}
