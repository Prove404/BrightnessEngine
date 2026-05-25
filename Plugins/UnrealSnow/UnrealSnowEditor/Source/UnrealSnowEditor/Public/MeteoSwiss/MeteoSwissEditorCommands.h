#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Styling/AppStyle.h"

class FMeteoSwissEditorCommands : public TCommands<FMeteoSwissEditorCommands>
{
public:
	FMeteoSwissEditorCommands()
		: TCommands<FMeteoSwissEditorCommands>(TEXT("MeteoSwissEditor"), NSLOCTEXT("MeteoSwiss", "MeteoSwissEditor", "MeteoSwiss Editor"), NAME_None, FAppStyle::GetAppStyleSetName())
	{
	}

	virtual void RegisterCommands() override;

public:
	TSharedPtr<FUICommandInfo> PullMeteoSwiss;
};
