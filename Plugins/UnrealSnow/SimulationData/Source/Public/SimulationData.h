#pragma once

#include "Modules/ModuleManager.h"

class FSimulationDataModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
