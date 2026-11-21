using UnrealBuildTool;

public class MakarEditorTarget : TargetRules
{
	public MakarEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "Makar", "MakarShaders" });
	}
}
