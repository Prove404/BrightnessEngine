#pragma once

#if WITH_EDITOR

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "MeteoSwissEditorFetcher.generated.h"

UCLASS()
class UNREALSNOWEDITOR_API UMeteoSwissEditorFetcher : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	DECLARE_MULTICAST_DELEGATE_TwoParams(FMeteoSwissPullFinished, bool /*bSuccess*/, int32 /*NumSamples*/);

	UFUNCTION(Exec, CallInEditor, Category="MeteoSwiss")
	void PullMeteoSwissNow();

	FMeteoSwissPullFinished OnMeteoSwissPullFinished;

private:
	void HandlePullFinished(bool bSuccess, int32 NumSamples);

	bool bPullInProgress = false;
};

#endif // WITH_EDITOR
