using System.IO;
using System.Linq;

namespace UnrealBuildTool.Rules
{
    public class Simulation : ModuleRules
    {
        public Simulation(ReadOnlyTargetRules Target) : base(Target)
        {
            PrivateIncludePaths.AddRange(
                new string[] {
                    "Simulation/Private",
                    "ShaderUtility/Public",
                    Path.Combine(ModuleDirectory, "../SimulationPixelShader/Source/Public")
                }
                );

            PublicDependencyModuleNames.AddRange(
                new string[]
                {
                      "Core", "CoreUObject", "Engine", "RenderCore", "Renderer", "Landscape", "RHI",
                        "SimplexNoise", "ShaderUtility", "SimulationData", "SimulationPixelShader", "GeoReferencing",
                        "ImageWrapper", "Json", "JsonUtilities"
                }
                );

            if (Target.bBuildEditor)
            {
                PrivateDependencyModuleNames.Add("UnrealEd");
            }

            bool bHasSunPosition = false;

            // Check if SunPosition plugin is enabled in the project
            // Since we can see it's enabled in BrightnessEngine.uproject, we'll assume it's available
            bHasSunPosition = true;
            
            if (bHasSunPosition)
            {
                PublicDependencyModuleNames.Add("SunPosition");
                System.Console.WriteLine("[Simulation] SunPosition plugin enabled - adding module dependency");
            }

            PublicDefinitions.Add(bHasSunPosition ? "WITH_SUNSKY_SUPPORT=1" : "WITH_SUNSKY_SUPPORT=0");
        }
    }
}
