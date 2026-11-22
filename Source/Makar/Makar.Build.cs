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
			"ProceduralMeshComponent", // ocean surface mesh
			"MeshDescription",         // procedural flora and rocks (OceanFlora)
			"StaticMeshDescription"
		});

		if (Target.bBuildEditor)
		{
			// The ocean follows the level editor's viewport camera when previewed in the editor.
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
	}
}
