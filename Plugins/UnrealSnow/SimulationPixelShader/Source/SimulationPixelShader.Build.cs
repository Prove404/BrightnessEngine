using System.IO;

namespace UnrealBuildTool.Rules
{
	public class SimulationPixelShader : ModuleRules
	{
		public SimulationPixelShader(ReadOnlyTargetRules Target) : base(Target)
        {
            PrivateIncludePaths.AddRange(
                new string[] {
                    Path.Combine(ModuleDirectory, "Private")
                }
                );

            PublicIncludePaths.AddRange(
                new string[] {
                    Path.Combine(ModuleDirectory, "Public"),
                    "ShaderUtility/Public",
                    "RenderCore/Public"
                }
                );

			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
                    "Engine",
                    "RenderCore",
                    "RHI",
                    "Renderer",
                    "ShaderUtility",
                    "Projects"
                }
				);

		}
	}
}
