#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FUICommandList;

class FUnrealSnowEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterMenus();
    void OnPullFrost();
    void OnPullERA5();
    void OnPullMeteoSwiss();
    void OnPullCsv();
    void OnFrostPullFinished(bool bSuccess, int32 NumSamples);
    void OnERA5PullFinished(bool bSuccess, int32 NumSamples);
    void OnMeteoSwissPullFinished(bool bSuccess, int32 NumSamples);
    void OnCsvPullFinished(bool bSuccess, int32 NumSamples);
    void ToggleReloadProviders_Exec();
    void BeginPull();
    void EndPull();
    bool IsBusy() const;

private:
    bool bReloadAfterPull = true;
    int32 ActivePullCount = 0;
    TSharedPtr<FUICommandList> PluginCommands;
    FDelegateHandle MenuRegHandle;
};
