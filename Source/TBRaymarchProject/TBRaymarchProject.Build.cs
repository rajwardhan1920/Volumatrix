// Copyright 2021 Tomas Bartipan and Technical University of Munich.
// Licensed under MIT license - See License.txt for details.

using UnrealBuildTool;

public class TBRaymarchProject : ModuleRules
{
    public TBRaymarchProject(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "InputCore",
                "RenderCore",
                "RHI",
                // Needed to use ARaymarchVolume
                "Raymarcher"
            });

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "VolumeTextureToolkit"
            });
    }
}
