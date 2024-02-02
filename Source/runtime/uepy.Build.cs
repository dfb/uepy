using UnrealBuildTool;
using System.IO;
using System.Collections.Generic;

public class uepy : ModuleRules
{
    public uepy(ReadOnlyTargetRules Target) : base(Target)
    {
        //OptimizeCode = CodeOptimization.Never;
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseRTTI = true;
        bEnableExceptions = true; // needed for clipper.cpp and pybind11

        PublicDefinitions.Add("EIGEN_MPL2_ONLY=1"); // Keep eigen as pure MPL2

        PublicIncludePaths.AddRange(
            new string[] {
                Path.Combine(ModuleDirectory, "../../pybind11/include"),
                Path.Combine(ModuleDirectory, "../../python/include"),
                Path.Combine(ModuleDirectory, "../../Eigen"),
            });
        //PublicLibraryPaths.Add(Path.Combine(ModuleDirectory, "../../python/libs"));
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
                "Paper2D",
            }
            );


        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "AIModule",
                "AudioCapture",
                "AudioMixer",
                "CinematicCamera",
                "Core",
                "CoreUObject",
                "Engine",
                "HeadMountedDisplay",
                "InputCore",
                "MediaAssets",
                "Niagara",
                "PhysicsCore",
                "Projects",
                "Slate",
                "SlateCore",
                "UMG",
                "WebBrowserWidget",
            }
        );

        if (Target.Type == TargetType.Editor)
            PrivateDependencyModuleNames.Add("UnrealEd"); // for FEditorDelegates

        DynamicallyLoadedModuleNames.AddRange(
            new string[]
            {
                // ... add any modules that your module loads dynamically here ...
            }
            );

        // for now it works only if the python files live in the same dir as the plugin DLL, so copy them over
        string destDir = Path.Combine(PluginDirectory, "../../Binaries/Win64");
        if (!Directory.Exists(destDir))
            Directory.CreateDirectory(destDir);

        string pythonHome = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../python"));
        foreach(string srcFilename in Directory.GetFiles(pythonHome, "*.*", SearchOption.TopDirectoryOnly))
        {
            if (srcFilename.EndsWith(".exe")) // don't need e.g. python.exe in there
                continue;
            string destFilename = Path.GetFullPath(Path.Combine(destDir, Path.GetFileName(srcFilename)));
            if (!File.Exists(destFilename))
            {
                File.Copy(srcFilename, destFilename);
                System.Console.WriteLine("Copying " + srcFilename + " --> " + destFilename);
            }
        }
        System.Threading.Thread.Sleep(500); // grr. TODO: only needed on Windows
        System.Console.WriteLine("Done copying files");
    }
}
