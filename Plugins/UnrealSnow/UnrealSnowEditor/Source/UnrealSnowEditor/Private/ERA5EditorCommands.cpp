#include "ERA5/ERA5EditorCommands.h"

#include "Framework/Commands/InputChord.h"
#include "Framework/Commands/UICommandInfo.h"
#include "InputCoreTypes.h"

#define LOCTEXT_NAMESPACE "ERA5EditorCommands"

void FERA5EditorCommands::RegisterCommands()
{
    UI_COMMAND(PullERA5, "Pull ERA5", "Fetch hourly ERA5 data and write JSON cache", EUserInterfaceActionType::Button, FInputChord(EKeys::E, EModifierKey::Control | EModifierKey::Alt));
}

#undef LOCTEXT_NAMESPACE
