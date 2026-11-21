#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

class FMakarShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const FString ShaderDirectory = FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Makar"), ShaderDirectory);
	}
};

IMPLEMENT_MODULE(FMakarShadersModule, MakarShaders);
