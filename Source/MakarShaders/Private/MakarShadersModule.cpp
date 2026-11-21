#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

class FMakarShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Absolute path: ShaderCompileWorker does not share the editor's working directory.
		const FString ShaderDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders")));
		if (!AllShaderSourceDirectoryMappings().Contains(TEXT("/Makar")))
		{
			AddShaderSourceDirectoryMapping(TEXT("/Makar"), ShaderDirectory);
		}
	}
};

IMPLEMENT_MODULE(FMakarShadersModule, MakarShaders);
