#pragma once

#if WITH_EDITOR

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "FrostEditorFetcher.generated.h"

UCLASS()
class UNREALSNOWEDITOR_API UFrostEditorFetcher : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    DECLARE_MULTICAST_DELEGATE_TwoParams(FFrostPullFinished, bool /*bSuccess*/, int32 /*NumSamples*/);

    // Download the latest observations from Frost and write them to the configured JSON path.
    UFUNCTION(Exec, CallInEditor, Category="Frost")
    void PullFrostNow();

    FFrostPullFinished OnFrostPullFinished;

private:
    void HandlePullFinished(bool bSuccess, int32 NumSamples);

    bool bPullInProgress = false;
};

#endif // WITH_EDITOR
