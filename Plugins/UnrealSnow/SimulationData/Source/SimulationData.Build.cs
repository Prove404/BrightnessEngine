namespace UnrealBuildTool.Rules
{
	public class SimulationData : ModuleRules
	{
		public SimulationData(ReadOnlyTargetRules Target) : base(Target)
		{
			PrivateIncludePaths.AddRange(
				new string[]
				{
					"SimulationData/Private"
				}
			);

			// Explicitly include Util source files
			PrivateIncludePaths.Add("SimulationData/Private/Util");

			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"Core", "CoreUObject", "Engine", "RenderCore", "Landscape", "RHI",
					"SimplexNoise", "ShaderUtility", "DeveloperSettings", "GeoReferencing"
				}
			);

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"HTTP", "Json", "JsonUtilities"
				}
			);
		}
	}
}
