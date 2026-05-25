using UnrealBuildTool;

namespace UnrealBuildTool.Rules
{
    public class UnrealSnowEditor : ModuleRules
    {
        public UnrealSnowEditor(ReadOnlyTargetRules Target) : base(Target)
        {
            PublicDependencyModuleNames.AddRange(new string[]
            {
                "Core", "CoreUObject", "Engine", "HTTP", "Json", "JsonUtilities"
            });

            PrivateDependencyModuleNames.AddRange(new string[]
            {
                "Slate", "SlateCore", "ToolMenus", "LevelEditor", "EditorSubsystem", "Projects", "InputCore",
                "UnrealEd", "SimulationData", "Simulation", "GeoReferencing"
            });

        }
    }
}


