using UnrealBuildTool;

// Module loaded early (PostConfigInit), so that the material compiler finds Shaders/*.ush under the virtual path
// "/Makar", and so that its global compute shaders (the GPU ocean cascade, Shaders/OceanFFT.usf) are registered
// before the global shader map is compiled.
public class MakarShaders : ModuleRules
{
	public MakarShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core" });
		PrivateDependencyModuleNames.AddRange(new string[] { "RenderCore", "RHI" });
	}
}
