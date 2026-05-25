#include "MeteoSwiss/MeteoSwissEditorCommands.h"

#include "Framework/Commands/InputChord.h"
#include "Framework/Commands/UICommandInfo.h"
#include "InputCoreTypes.h"

#define LOCTEXT_NAMESPACE "MeteoSwissEditorCommands"

void FMeteoSwissEditorCommands::RegisterCommands()
{
	UI_COMMAND(PullMeteoSwiss, "Pull MeteoSwiss", "Fetch hourly data from MeteoSwiss and write JSON cache", EUserInterfaceActionType::Button, FInputChord(EKeys::M, EModifierKey::Control | EModifierKey::Alt));
}

#undef LOCTEXT_NAMESPACE
