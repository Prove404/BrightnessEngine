// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#include "SimulationData.h"
#include "Modules/ModuleManager.h"
#include "Csv/CsvSettings.h"

void FSimulationDataModule::StartupModule()
{
    // Touch settings class to ensure it registers with Project Settings
    UCsvSettings::StaticClass();
}

void FSimulationDataModule::ShutdownModule()
{

}

IMPLEMENT_MODULE(FSimulationDataModule, SimulationData)