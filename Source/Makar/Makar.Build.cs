using UnrealBuildTool;

public class Makar : ModuleRules
{
	public Makar(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"ProceduralMeshComponent" // ocean surface mesh
		});

		if (Target.bBuildEditor)
		{
			// The ocean follows the level editor's viewport camera when previewed in the editor.
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
	}
}
