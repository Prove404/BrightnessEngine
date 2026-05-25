#pragma once

#if WITH_EDITOR

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "CsvEditorFetcher.generated.h"

UCLASS()
class UNREALSNOWEDITOR_API UCsvEditorFetcher : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    DECLARE_MULTICAST_DELEGATE_TwoParams(FCsvPullFinished, bool /*bSuccess*/, int32 /*NumSamples*/);

    // Read configured CSV, slice to simulation time span, and write JSON.
    UFUNCTION(Exec, CallInEditor, Category="CSV")
    void PullCsvNow();

    FCsvPullFinished OnCsvPullFinished;

private:
    void HandlePullFinished(bool bSuccess, int32 NumSamples);
    bool bPullInProgress = false;
};

#endif // WITH_EDITOR


