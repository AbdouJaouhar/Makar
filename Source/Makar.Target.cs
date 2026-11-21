using UnrealBuildTool;

public class MakarTarget : TargetRules
{
	public MakarTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "Makar", "MakarShaders" });
	}
}
