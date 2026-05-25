// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "RHIResources.h"
#include "ClimateData.h"
#include "SimulationWeatherDataProviderBase.generated.h"

struct FWeatherForcingData;

class ASnowSimulationActor;

// @TODO simplify API because most weather data sources only provide monthly or daily average values.
// @TODO use stochastic downscaling for hourly weather data
// @TODO extend ActorComponent?
/**
 * Base class for all data providers for the simulation.
 */
UCLASS(BlueprintType)
class SIMULATIONDATA_API USimulationWeatherDataProviderBase : public UActorComponent
{
	GENERATED_BODY()

public:

	/** Initializes the data provider. */
	virtual void Initialize(FDateTime StartTime, FDateTime EndTime) PURE_VIRTUAL(USimulationWeatherDataProviderBase::Initialize, ;);

	/** Returns the altitude at which the measurements were taken. */
	virtual float GetMeasurementAltitude() PURE_VIRTUAL(USimulationWeatherDataProviderBase::GetMeasurementAltitude, return 0.0f;);

	/** Creates a resource array containing all weather data. Caller is responsible of deleting the resource. */
	virtual TResourceArray<FClimateData>* CreateRawClimateDataResourceArray(FDateTime StartTime, FDateTime EndTime) PURE_VIRTUAL(USimulationWeatherDataProviderBase::CreateRawClimateDataResourceArray, return nullptr;);

	/** Get comprehensive weather forcing data for a specific time and location */
	virtual FWeatherForcingData GetWeatherForcing(FDateTime Time, int32 GridX = 0, int32 GridY = 0) { return FWeatherForcingData(); }
};


