namespace UnrealBuildTool.Rules
{
	public class ShaderUtility : ModuleRules
	{
		public ShaderUtility(ReadOnlyTargetRules Target) : base(Target)
        {
            PrivateIncludePaths.AddRange(
                new string[] {
					"ShaderUtility/Private"
				}
                );

            PublicIncludePaths.AddRange(
                new string[] {
                    "ShaderUtility/Public",
                }
                );

            PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
                    "Engine",
                    "RenderCore",
                    "RHI"
				}
				);
		}
	}
}