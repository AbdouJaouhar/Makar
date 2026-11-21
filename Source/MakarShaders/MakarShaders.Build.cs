using UnrealBuildTool;

// Tiny module loaded early (PostConfigInit) so the material compiler can find Shaders/*.ush
// under the virtual path "/Makar".
public class MakarShaders : ModuleRules
{
	public MakarShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new string[] { "Core", "RenderCore" });
	}
}
