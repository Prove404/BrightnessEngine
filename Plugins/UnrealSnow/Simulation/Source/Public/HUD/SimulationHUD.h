#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "SimulationHUD.generated.h"


UCLASS()
class ASimulationHUD : public AHUD
{
	GENERATED_BODY()

public:

	/** Font for displaying text - defaults to engine font */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	UFont* UE4Font;

	/** Initialize default font if needed */
	void InitializeDefaultFont();

	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

	void DrawHUD() override;
};