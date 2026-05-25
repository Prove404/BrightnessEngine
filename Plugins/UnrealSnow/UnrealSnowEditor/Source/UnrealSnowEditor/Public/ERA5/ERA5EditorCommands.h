#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Styling/AppStyle.h"

class FERA5EditorCommands : public TCommands<FERA5EditorCommands>
{
public:
    FERA5EditorCommands()
        : TCommands<FERA5EditorCommands>(TEXT("ERA5Editor"), NSLOCTEXT("ERA5", "ERA5Editor", "ERA5 Editor"), NAME_None, FAppStyle::GetAppStyleSetName())
    {
    }

    virtual void RegisterCommands() override;

public:
    TSharedPtr<FUICommandInfo> PullERA5;
};
