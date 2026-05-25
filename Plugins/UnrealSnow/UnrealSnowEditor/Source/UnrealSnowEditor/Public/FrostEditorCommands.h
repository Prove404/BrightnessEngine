#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Styling/AppStyle.h"

class FFrostEditorCommands : public TCommands<FFrostEditorCommands>
{
public:
    FFrostEditorCommands()
        : TCommands<FFrostEditorCommands>(TEXT("FrostEditor"), NSLOCTEXT("Frost", "FrostEditor", "Frost Editor"), NAME_None, FAppStyle::GetAppStyleSetName())
    {
    }

    virtual void RegisterCommands() override;

public:
    TSharedPtr<FUICommandInfo> PullFrost;
    TSharedPtr<FUICommandInfo> ToggleReloadProviders;
    TSharedPtr<FUICommandInfo> PullCsv;
};


