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
        // PublicLibraryPaths.Add(Path.Combine(ModuleDirectory, "../../python/libs"));
        PublicAdditionalLibraries.Add(Path.Combine(ModuleDirectory, "../../python/libs/python311.lib"));



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
                "AssetRegistry",
                "AudioMixer",
                "Blutility",
                "CoreUObject",
                "EditorStyle",
                "Engine",
                "InputCore",
                "LevelEditor",
                "MediaAssets",
                "Paper2D",
                "Projects",
                "Slate",
                "SlateCore",
                "uepy",
                "UMG",
                "UnrealEd",
                "WorkspaceMenuStructure",

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

