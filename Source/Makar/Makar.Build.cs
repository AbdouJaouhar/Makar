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
			"Water" // UBuoyancyComponent + ocean
		});
	}
}
