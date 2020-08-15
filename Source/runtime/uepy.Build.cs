using UnrealBuildTool;
using System.IO;
using System.Collections.Generic;

public class uepy : ModuleRules
{
    public uepy(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseRTTI = true;
        bEnableExceptions = true; // needed for clipper.cpp
        
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
                "CoreUObject",
                "Engine",
                "InputCore",
                "Projects",
                "Slate",
                "SlateCore",
                "UMG",
            }
        );
        if (Target.Configuration != UnrealTargetConfiguration.Shipping)
            PrivateDependencyModuleNames.Add("UnrealEd"); // for FEditorDelegates
        
        DynamicallyLoadedModuleNames.AddRange(
            new string[]
            {
                // ... add any modules that your module loads dynamically here ...
            }
            );

        // for now it works only if the python files live in the same dir as the plugin DLL, so copy them over
        string destDir = Path.Combine(ModuleDirectory, "../../Binaries/Win64");
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

