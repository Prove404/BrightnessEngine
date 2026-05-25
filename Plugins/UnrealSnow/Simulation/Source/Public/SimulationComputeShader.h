#pragma once

#include "Cells/GPUSimulationCell.h"
#include "Private/ComputeShaderDeclaration.h"
#include "ClimateData.h"
#include "RWStructuredBuffer.h"
#include "Cells/DebugCell.h"
#include "RHI.h"

DECLARE_LOG_CATEGORY_EXTERN(SnowComputeShader, Log, All);

/**
* This class demonstrates how to use the compute shader we have declared.
* Most importantly which RHI functions are needed to call and how to get 
* some interesting output.                                                
*/
class SIMULATION_API FSimulationComputeShader
{
public:
	FSimulationComputeShader(ERHIFeatureLevel::Type ShaderFeatureLevel);
	~FSimulationComputeShader();

	/** Initializes the simulation with the correct input data. */
	void Initialize(
		TResourceArray<FGPUSimulationCell>& Cells, TResourceArray<FClimateData>& WeatherData, float InK_e, float InK_m,
		float InTMeltA, float InTMeltB, float InTSnowA, float InTSnowB, int32 InTotalSimulationHours,
		int32 CellsDimensionX, int32 CellsDimensionY, float InMeasurementAltitude, float MaxSnow,
		float InSlopeThreshold, float InFreshSnowAlbedo, float InOldSnowAlbedo,
		bool InbUseWeatherSnowFraction, bool InbDisableLapseRateAdjustments);

	/**
	* Run this to execute the compute shader once!
	* @param TotalElapsedTimeSeconds - We use this for simulation state
	*/
	void ExecuteComputeShader(int CurrentTimeStep, int32 InTimesteps, int InHourOfDay, bool CaptureDebugInformation, TArray<FDebugCell>& DebugInformation);

	/**
	* Only execute this from the render thread.
	*/
	void ExecuteComputeShaderInternal(bool CaptudeDebugInformation, TArray<FDebugCell>& DebugInformation);

	/**
		* Returns the maximum snow of the last execution.
		*/
	float GetMaxSnow();

	FTextureRHIRef GetTexture() { return Texture; }

	FRWStructuredBuffer* GetSnowBuffer() { return SnowOutputBuffer; }

	FRWStructuredBuffer* GetMaxSnowBuffer() { return MaxSnowBuffer; }

	/**
	 * Read GPU snow depth data back to CPU array (for diagnostics)
	 * @param OutDepthArray - Array to fill with snow depth in meters (mm / area)
	 */
	void ReadbackSnowDepth(TArray<float>& OutDepthArray);

	/**
	 * Read GPU cell state data back to CPU arrays (for diagnostics)
	 * @param OutSWE - Snow water equivalent in liters per cell
	 * @param OutAlbedo - Albedo values (0-1)
	 * @param OutDaysSinceSnow - Days since last snowfall
	 */
	void ReadbackCellState(TArray<float>& OutSWE, TArray<float>& OutAlbedo, TArray<float>& OutDaysSinceSnow);

private:
	/** Feature level */
	ERHIFeatureLevel::Type FeatureLevel;

	/** Execution state */
	bool IsComputeShaderExecuting;
	bool IsUnloading;

	/** Simulation parameters */
	int32 NumCells;
	int32 CellsDimensionX;
	int32 CellsDimensionY;
	bool Debug = false;

	/** Shader parameters */
	// Constant parameters
	int32 TotalSimulationHours;
	int32 CellsDimensionX_Param;
	float ThreadGroupCountX;
	float ThreadGroupCountY;
	float TSnowA;
	float TSnowB;
	float TMeltA;
	float TMeltB;
	float k_e;
	float k_m;
	float MeasurementAltitude;
	float SlopeThreshold;
	float FreshSnowAlbedo;
	float OldSnowAlbedo;
	bool bUseWeatherSnowFraction;
	bool bDisableLapseRateAdjustments;
	// Variable parameters
	int32 CurrentSimulationStep;
	int32 Timesteps;
	int32 DayOfYear;
	int32 HourOfDay;
	float WeatherSnowFrac;
	float PrecipRate_kgm2s;

	/** Main texture */
	FTextureRHIRef Texture;

	/** We need a UAV if we want to be able to write to the resource*/
	FUnorderedAccessViewRHIRef TextureUAV;

	/** Cells for the simulation. */
	FRWStructuredBuffer* SimulationCellsBuffer;

	/** Temperature data for the simulation. */
	FRWStructuredBuffer* ClimateDataBuffer;

	/** Maximum snow buffer. */
	FRWStructuredBuffer* MaxSnowBuffer;
	float MaxSnow;

	/** Output snow map array. */
	FRWStructuredBuffer* SnowOutputBuffer;
};
