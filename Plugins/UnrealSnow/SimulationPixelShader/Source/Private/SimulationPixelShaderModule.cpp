#include "SimulationPixelShader.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

void FSimulationPixelShaderModule::StartupModule()
{
    // Register shader source directory mapping for this plugin
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SimulationPixelShader"));
    if (Plugin.IsValid())
    {
        const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
        AddShaderSourceDirectoryMapping(TEXT("/Plugin/SimulationPixelShader"), ShaderDir);
        UE_LOG(LogTemp, Log, TEXT("SimulationPixelShader: Registered shader directory mapping /Plugin/SimulationPixelShader -> %s"), *ShaderDir);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("SimulationPixelShader: Failed to find plugin - shader compilation will fail!"));
    }
}

void FSimulationPixelShaderModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FSimulationPixelShaderModule, SimulationPixelShader)
