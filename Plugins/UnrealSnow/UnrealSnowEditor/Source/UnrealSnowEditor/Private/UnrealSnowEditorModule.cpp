#include "UnrealSnowEditorModule.h"

#include "FrostEditorCommands.h"
#include "ERA5/ERA5EditorCommands.h"
#include "MeteoSwiss/MeteoSwissEditorCommands.h"
#include "Csv/CsvEditorFetcher.h"

#include "ToolMenus.h"
#include "LevelEditor.h"
#include "Editor.h"
#include "Frost/FrostEditorFetcher.h"
#include "ERA5/ERA5EditorFetcher.h"
#include "MeteoSwiss/MeteoSwissEditorFetcher.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Frost/FrostWeatherProvider.h"
#include "ERA5/ERA5WeatherProvider.h"
#include "MeteoSwiss/MeteoSwissWeatherDataProvider.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

namespace
{
    void ExecutePullFrostConsole(UWorld* World)
    {
        if (!GEditor || !World)
        {
            return;
        }

        if (UFrostEditorFetcher* Subsys = GEditor->GetEditorSubsystem<UFrostEditorFetcher>())
        {
            Subsys->PullFrostNow();
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[Frost] Editor fetcher subsystem not available."));
        }
    }

    void ExecutePullERA5Console(UWorld* World)
    {
        if (!GEditor || !World)
        {
            return;
        }

        if (UERA5EditorFetcher* Subsys = GEditor->GetEditorSubsystem<UERA5EditorFetcher>())
        {
            Subsys->PullERA5Now();
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Editor fetcher subsystem not available."));
        }
    }

    void ExecutePullMeteoSwissConsole(UWorld* World)
    {
        if (!GEditor || !World)
        {
            return;
        }

        if (UMeteoSwissEditorFetcher* Subsys = GEditor->GetEditorSubsystem<UMeteoSwissEditorFetcher>())
        {
            Subsys->PullMeteoSwissNow();
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] Editor fetcher subsystem not available."));
        }
    }

    void ExecutePullCsvConsole(UWorld* World)
    {
        if (!GEditor || !World)
        {
            return;
        }

        if (UCsvEditorFetcher* Subsys = GEditor->GetEditorSubsystem<UCsvEditorFetcher>())
        {
            Subsys->PullCsvNow();
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[CSV] Editor fetcher subsystem not available."));
        }
    }

    FAutoConsoleCommandWithWorld GPullFrostConsoleCommand(
        TEXT("PullFrostNow"),
        TEXT("Download latest Frost observations to the configured JSON output."),
        FConsoleCommandWithWorldDelegate::CreateStatic(&ExecutePullFrostConsole));

    FAutoConsoleCommandWithWorld GPullERA5ConsoleCommand(
        TEXT("PullERA5Now"),
        TEXT("Download ERA5 reanalysis data to the configured JSON output."),
        FConsoleCommandWithWorldDelegate::CreateStatic(&ExecutePullERA5Console));

    FAutoConsoleCommandWithWorld GPullMeteoSwissConsoleCommand(
        TEXT("PullMeteoSwissNow"),
        TEXT("Download MeteoSwiss station data to the configured JSON output."),
        FConsoleCommandWithWorldDelegate::CreateStatic(&ExecutePullMeteoSwissConsole));

    FAutoConsoleCommandWithWorld GPullCsvConsoleCommand(
        TEXT("PullCsvNow"),
        TEXT("Read configured CSV and write sliced JSON for the simulation window."),
        FConsoleCommandWithWorldDelegate::CreateStatic(&ExecutePullCsvConsole));
}
void FUnrealSnowEditorModule::StartupModule()
{
    FFrostEditorCommands::Register();
    FERA5EditorCommands::Register();
    FMeteoSwissEditorCommands::Register();

    PluginCommands = MakeShareable(new FUICommandList);
    PluginCommands->MapAction(
        FFrostEditorCommands::Get().PullFrost,
        FExecuteAction::CreateRaw(this, &FUnrealSnowEditorModule::OnPullFrost),
        FCanExecuteAction::CreateLambda([this]() { return !IsBusy(); })
    );
    PluginCommands->MapAction(
        FFrostEditorCommands::Get().ToggleReloadProviders,
        FExecuteAction::CreateRaw(this, &FUnrealSnowEditorModule::ToggleReloadProviders_Exec),
        FCanExecuteAction(),
        FIsActionChecked::CreateLambda([this]() { return bReloadAfterPull; })
    );
    PluginCommands->MapAction(
        FERA5EditorCommands::Get().PullERA5,
        FExecuteAction::CreateRaw(this, &FUnrealSnowEditorModule::OnPullERA5),
        FCanExecuteAction::CreateLambda([this]() { return !IsBusy(); })
    );
    PluginCommands->MapAction(
        FMeteoSwissEditorCommands::Get().PullMeteoSwiss,
        FExecuteAction::CreateRaw(this, &FUnrealSnowEditorModule::OnPullMeteoSwiss),
        FCanExecuteAction::CreateLambda([this]() { return !IsBusy(); })
    );
    PluginCommands->MapAction(
        FFrostEditorCommands::Get().PullCsv,
        FExecuteAction::CreateRaw(this, &FUnrealSnowEditorModule::OnPullCsv),
        FCanExecuteAction::CreateLambda([this]() { return !IsBusy(); })
    );

    UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUnrealSnowEditorModule::RegisterMenus));

    if (UFrostEditorFetcher* FrostSubsys = GEditor ? GEditor->GetEditorSubsystem<UFrostEditorFetcher>() : nullptr)
    {
        FrostSubsys->OnFrostPullFinished.AddRaw(this, &FUnrealSnowEditorModule::OnFrostPullFinished);
    }

    if (UERA5EditorFetcher* EraSubsys = GEditor ? GEditor->GetEditorSubsystem<UERA5EditorFetcher>() : nullptr)
    {
        EraSubsys->OnERA5PullFinished.AddRaw(this, &FUnrealSnowEditorModule::OnERA5PullFinished);
    }

    if (UMeteoSwissEditorFetcher* MeteoSwissSubsys = GEditor ? GEditor->GetEditorSubsystem<UMeteoSwissEditorFetcher>() : nullptr)
    {
        MeteoSwissSubsys->OnMeteoSwissPullFinished.AddRaw(this, &FUnrealSnowEditorModule::OnMeteoSwissPullFinished);
    }

    if (UCsvEditorFetcher* CsvSubsys = GEditor ? GEditor->GetEditorSubsystem<UCsvEditorFetcher>() : nullptr)
    {
        CsvSubsys->OnCsvPullFinished.AddRaw(this, &FUnrealSnowEditorModule::OnCsvPullFinished);
    }
}

void FUnrealSnowEditorModule::ShutdownModule()
{
    UToolMenus::UnregisterOwner(this);
    FMeteoSwissEditorCommands::Unregister();
    FERA5EditorCommands::Unregister();
    FFrostEditorCommands::Unregister();
}

void FUnrealSnowEditorModule::RegisterMenus()
{
    UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar");
    FToolMenuSection& Section = Toolbar->AddSection("Weather", TAttribute<FText>(), FToolMenuInsert("Settings", EToolMenuInsertType::After));

    FToolMenuEntry PullFrostEntry = FToolMenuEntry::InitToolBarButton(FFrostEditorCommands::Get().PullFrost);
    PullFrostEntry.Icon = FSlateIcon(FAppStyle::GetAppStyleSetName(), "PlayToolbar.Refresh");
    PullFrostEntry.SetCommandList(PluginCommands);
    Section.AddEntry(PullFrostEntry);

    FToolMenuEntry PullEraEntry = FToolMenuEntry::InitToolBarButton(FERA5EditorCommands::Get().PullERA5);
    PullEraEntry.Icon = FSlateIcon(FAppStyle::GetAppStyleSetName(), "Level.VisibleIcon16x");
    PullEraEntry.SetCommandList(PluginCommands);
    Section.AddEntry(PullEraEntry);

    FToolMenuEntry PullMeteoSwissEntry = FToolMenuEntry::InitToolBarButton(FMeteoSwissEditorCommands::Get().PullMeteoSwiss);
    PullMeteoSwissEntry.Icon = FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Download");
    PullMeteoSwissEntry.SetCommandList(PluginCommands);
    Section.AddEntry(PullMeteoSwissEntry);

    FToolMenuEntry PullCsvEntry = FToolMenuEntry::InitToolBarButton(FFrostEditorCommands::Get().PullCsv);
    PullCsvEntry.Icon = FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.RefreshAll");
    PullCsvEntry.SetCommandList(PluginCommands);
    Section.AddEntry(PullCsvEntry);

    FToolMenuEntry ToggleEntry = FToolMenuEntry::InitToolBarButton(FFrostEditorCommands::Get().ToggleReloadProviders);
    ToggleEntry.Icon = FSlateIcon(FAppStyle::GetAppStyleSetName(), "Level.SaveAll");
    ToggleEntry.SetCommandList(PluginCommands);
    Section.AddEntry(ToggleEntry);
}
void FUnrealSnowEditorModule::OnPullFrost()
{
    if (IsBusy())
    {
        return;
    }

    if (UFrostEditorFetcher* Subsys = GEditor ? GEditor->GetEditorSubsystem<UFrostEditorFetcher>() : nullptr)
    {
        BeginPull();
        Subsys->PullFrostNow();

        FNotificationInfo Info(NSLOCTEXT("Frost", "Pulling", "Pulling Frost..."));
        Info.ExpireDuration = 1.0f;
        FSlateNotificationManager::Get().AddNotification(Info);
    }
    else
    {
        FNotificationInfo Info(NSLOCTEXT("Frost", "NoSubsys", "Frost fetcher not available"));
        Info.ExpireDuration = 3.0f;
        TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
        if (Notification.IsValid())
        {
            Notification->SetCompletionState(SNotificationItem::CS_Fail);
        }
    }
}

void FUnrealSnowEditorModule::OnPullERA5()
{
    if (IsBusy())
    {
        return;
    }

    if (UERA5EditorFetcher* Subsys = GEditor ? GEditor->GetEditorSubsystem<UERA5EditorFetcher>() : nullptr)
    {
        BeginPull();
        Subsys->PullERA5Now();

        FNotificationInfo Info(NSLOCTEXT("ERA5", "Pulling", "Pulling ERA5..."));
        Info.ExpireDuration = 1.0f;
        FSlateNotificationManager::Get().AddNotification(Info);
    }
    else
    {
        FNotificationInfo Info(NSLOCTEXT("ERA5", "NoSubsys", "ERA5 fetcher not available"));
        Info.ExpireDuration = 3.0f;
        TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
        if (Notification.IsValid())
        {
            Notification->SetCompletionState(SNotificationItem::CS_Fail);
        }
    }
}

void FUnrealSnowEditorModule::OnPullMeteoSwiss()
{
    if (IsBusy())
    {
        return;
    }

    if (UMeteoSwissEditorFetcher* Subsys = GEditor ? GEditor->GetEditorSubsystem<UMeteoSwissEditorFetcher>() : nullptr)
    {
        BeginPull();
        Subsys->PullMeteoSwissNow();

        FNotificationInfo Info(NSLOCTEXT("MeteoSwiss", "Pulling", "Pulling MeteoSwiss..."));
        Info.ExpireDuration = 1.0f;
        FSlateNotificationManager::Get().AddNotification(Info);
    }
    else
    {
        FNotificationInfo Info(NSLOCTEXT("MeteoSwiss", "NoSubsys", "MeteoSwiss fetcher not available"));
        Info.ExpireDuration = 3.0f;
        TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
        if (Notification.IsValid())
        {
            Notification->SetCompletionState(SNotificationItem::CS_Fail);
        }
    }
}

void FUnrealSnowEditorModule::OnPullCsv()
{
    if (IsBusy())
    {
        return;
    }

    if (UCsvEditorFetcher* Subsys = GEditor ? GEditor->GetEditorSubsystem<UCsvEditorFetcher>() : nullptr)
    {
        BeginPull();
        Subsys->PullCsvNow();

        FNotificationInfo Info(NSLOCTEXT("CSV", "Pulling", "Slicing CSV to JSON..."));
        Info.ExpireDuration = 1.0f;
        FSlateNotificationManager::Get().AddNotification(Info);
    }
    else
    {
        FNotificationInfo Info(NSLOCTEXT("CSV", "NoSubsys", "CSV fetcher not available"));
        Info.ExpireDuration = 3.0f;
        TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
        if (Notification.IsValid())
        {
            Notification->SetCompletionState(SNotificationItem::CS_Fail);
        }
    }
}

void FUnrealSnowEditorModule::OnFrostPullFinished(bool bSuccess, int32 NumSamples)
{
    EndPull();

    if (bSuccess)
    {
        FNotificationInfo Info(FText::FromString(FString::Printf(TEXT("Frost: wrote %d samples"), NumSamples)));
        Info.ExpireDuration = 3.0f;
        TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
        if (Notification.IsValid())
        {
            Notification->SetCompletionState(SNotificationItem::CS_Success);
        }

        if (bReloadAfterPull)
        {
            if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
            {
                int32 Reloaded = 0;
                for (TActorIterator<AFrostWeatherProviderActor> It(World); It; ++It)
                {
                    if (It->Provider)
                    {
                        It->RefreshFromJson();
                        ++Reloaded;
                    }
                }
                FNotificationInfo Info2(FText::FromString(FString::Printf(TEXT("Reloaded %d Frost providers"), Reloaded)));
                Info2.ExpireDuration = 2.0f;
                TSharedPtr<SNotificationItem> Notification2 = FSlateNotificationManager::Get().AddNotification(Info2);
                if (Notification2.IsValid())
                {
                    Notification2->SetCompletionState(SNotificationItem::CS_Success);
                }
            }
        }
    }
    else
    {
        FNotificationInfo Info(NSLOCTEXT("Frost", "PullFail", "Frost pull failed"));
        Info.ExpireDuration = 4.0f;
        TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
        if (Notification.IsValid())
        {
            Notification->SetCompletionState(SNotificationItem::CS_Fail);
        }
    }
}

void FUnrealSnowEditorModule::OnERA5PullFinished(bool bSuccess, int32 NumSamples)
{
    EndPull();

    if (bSuccess)
    {
        FNotificationInfo Info(FText::FromString(FString::Printf(TEXT("ERA5: wrote %d samples"), NumSamples)));
        Info.ExpireDuration = 3.0f;
        TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
        if (Notification.IsValid())
        {
            Notification->SetCompletionState(SNotificationItem::CS_Success);
        }

        if (bReloadAfterPull)
        {
            if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
            {
                int32 Reloaded = 0;
                for (TActorIterator<AERA5WeatherProviderActor> It(World); It; ++It)
                {
                    if (It->Provider)
                    {
                        It->RefreshFromJson();
                        ++Reloaded;
                    }
                }
                FNotificationInfo Info2(FText::FromString(FString::Printf(TEXT("Reloaded %d ERA5 providers"), Reloaded)));
                Info2.ExpireDuration = 2.0f;
                TSharedPtr<SNotificationItem> Notification2 = FSlateNotificationManager::Get().AddNotification(Info2);
                if (Notification2.IsValid())
                {
                    Notification2->SetCompletionState(SNotificationItem::CS_Success);
                }
            }
        }
    }
    else
    {
        FNotificationInfo Info(NSLOCTEXT("ERA5", "PullFail", "ERA5 pull failed"));
        Info.ExpireDuration = 4.0f;
        TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
        if (Notification.IsValid())
        {
            Notification->SetCompletionState(SNotificationItem::CS_Fail);
        }
    }
}

void FUnrealSnowEditorModule::OnMeteoSwissPullFinished(bool bSuccess, int32 NumSamples)
{
    EndPull();

    if (bSuccess)
    {
        FNotificationInfo Info(FText::FromString(FString::Printf(TEXT("MeteoSwiss: wrote %d samples"), NumSamples)));
        Info.ExpireDuration = 3.0f;
        TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
        if (Notification.IsValid())
        {
            Notification->SetCompletionState(SNotificationItem::CS_Success);
        }

        if (bReloadAfterPull)
        {
            if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
            {
                int32 Reloaded = 0;
                for (TActorIterator<AMeteoSwissWeatherProviderActor> It(World); It; ++It)
                {
                    if (It->Provider)
                    {
                        It->RefreshFromJson();
                        ++Reloaded;
                    }
                }
                FNotificationInfo Info2(FText::FromString(FString::Printf(TEXT("Reloaded %d MeteoSwiss providers"), Reloaded)));
                Info2.ExpireDuration = 2.0f;
                TSharedPtr<SNotificationItem> Notification2 = FSlateNotificationManager::Get().AddNotification(Info2);
                if (Notification2.IsValid())
                {
                    Notification2->SetCompletionState(SNotificationItem::CS_Success);
                }
            }
        }
    }
    else
    {
        FNotificationInfo Info(NSLOCTEXT("MeteoSwiss", "PullFail", "MeteoSwiss pull failed"));
        Info.ExpireDuration = 4.0f;
        TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
        if (Notification.IsValid())
        {
            Notification->SetCompletionState(SNotificationItem::CS_Fail);
        }
    }
}

void FUnrealSnowEditorModule::OnCsvPullFinished(bool bSuccess, int32 NumSamples)
{
    EndPull();

    if (bSuccess)
    {
        FNotificationInfo Info(FText::FromString(FString::Printf(TEXT("CSV: wrote %d samples"), NumSamples)));
        Info.ExpireDuration = 3.0f;
        TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
        if (Notification.IsValid())
        {
            Notification->SetCompletionState(SNotificationItem::CS_Success);
        }
    }
    else
    {
        FNotificationInfo Info(NSLOCTEXT("CSV", "PullFail", "CSV slicing failed"));
        Info.ExpireDuration = 4.0f;
        TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
        if (Notification.IsValid())
        {
            Notification->SetCompletionState(SNotificationItem::CS_Fail);
        }
    }
}

void FUnrealSnowEditorModule::ToggleReloadProviders_Exec()
{
    bReloadAfterPull = !bReloadAfterPull;
}

void FUnrealSnowEditorModule::BeginPull()
{
    ++ActivePullCount;
}

void FUnrealSnowEditorModule::EndPull()
{
    ActivePullCount = FMath::Max(ActivePullCount - 1, 0);
}

bool FUnrealSnowEditorModule::IsBusy() const
{
    return ActivePullCount > 0;
}

IMPLEMENT_MODULE(FUnrealSnowEditorModule, UnrealSnowEditor)
