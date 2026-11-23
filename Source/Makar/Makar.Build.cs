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

		// The finest ocean cascade runs on the GPU (compute shaders in the MakarShaders module).
		PrivateDependencyModuleNames.AddRange(new string[] { "MakarShaders", "RenderCore", "RHI" });

		if (Target.bBuildEditor)
		{
			// The ocean follows the level editor's viewport camera when previewed in the editor.
			PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "AssetRegistry" }); // AssetRegistry: baking the flora (AOceanIslandFlora::BakeMeshes)
		}
	}
}
