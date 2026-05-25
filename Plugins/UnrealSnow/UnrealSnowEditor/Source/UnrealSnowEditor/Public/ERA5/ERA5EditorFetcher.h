#pragma once

#if WITH_EDITOR

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "ERA5EditorFetcher.generated.h"

UCLASS()
class UNREALSNOWEDITOR_API UERA5EditorFetcher : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    DECLARE_MULTICAST_DELEGATE_TwoParams(FERA5PullFinished, bool /*bSuccess*/, int32 /*NumSamples*/);

    UFUNCTION(Exec, CallInEditor, Category="ERA5")
    void PullERA5Now();

    FERA5PullFinished OnERA5PullFinished;

private:
    void HandlePullFinished(bool bSuccess, int32 NumSamples);

    bool bPullInProgress = false;
};

#endif // WITH_EDITOR
