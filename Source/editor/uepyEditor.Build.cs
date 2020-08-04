using UnrealBuildTool;
using System.IO;
using System.Collections.Generic;

public class uepyEditor : ModuleRules
{
	public uepyEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseRTTI = true;
		
		PublicIncludePaths.AddRange(
			new string[] {
                Path.Combine(ModuleDirectory, "../../pybind11/include"),
                Path.Combine(ModuleDirectory, "../../python/include"),
			});
        PublicLibraryPaths.Add(Path.Combine(ModuleDirectory, "../../python/libs"));
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
				"InputCore",
				"UnrealEd",
				"LevelEditor",
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}

