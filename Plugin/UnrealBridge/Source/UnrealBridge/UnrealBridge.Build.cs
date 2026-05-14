using UnrealBuildTool;

public class UnrealBridge : ModuleRules
{
	public UnrealBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Sockets",
			"Networking",
			"Json",
			"JsonUtilities",
			"PythonScriptPlugin",
			"DeveloperSettings",
			"BlueprintGraph",
			"KismetCompiler",
			"UMG",
			"AssetRegistry",
			"Kismet",
			"GraphEditor",
			"UMGEditor",
			"AnimGraph",
			"UnrealEd",
			"EditorSubsystem",
			"MovieScene",
			"AnimGraphRuntime",
			"ContentBrowser",
			"ContentBrowserData",
			"LevelEditor",
			"GameplayAbilities",
			"GameplayAbilitiesEditor",
			"GameplayTags",
			"GameplayTagsEditor",
			"GameplayTasks",
			"GameplayTasksEditor",
			"MainFrame",
			"NavigationSystem",
			"Navmesh",
			"Landscape",
			"EnhancedInput",
			"InputBlueprintNodes",
			"InputEditor",
			"Projects",
			"Slate",
			"SlateCore",
			"InputCore",
			"SourceControl",
			"ImageCore",
			"ImageWrapper",
			"RenderCore",
			"RHI",
			// TraceLog hosts UE::Trace::EnumerateChannels (used by M4-4
			// list_trace_channels). Core publicly forwards TraceLog headers
			// but the symbols are __declspec(dllimport) so a direct link
			// dep is required. TraceLog.Build.cs sets
			// bRequiresImplementModule=false, so this only pulls the link
			// import — no extra runtime cost.
			"TraceLog",
			// TraceServices — perf-capability M4-5 ParseTraceToSummary.
			// Loads + analyses .utrace files via IAnalysisService::Analyze
			// (synchronous). Pulls in TraceAnalysis transitively.
			"TraceServices",
		});

		if (Target.Version.MajorVersion > 5 || (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 7))
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"MaterialEditor",
				"PoseSearch",
				"Chooser",
				"ChooserEditor",
				"StructUtils",
				"GeometryScriptingCore",
				"GeometryFramework",
				"GeometryScriptingEditor",
				"PCG",
			});
		}

		// Live Coding is a Windows-only editor module. Guard the dep so
		// non-Windows builds of this editor plugin don't fail to link.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
