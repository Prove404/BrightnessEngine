#include "FrostEditorCommands.h"

#include "Framework/Commands/UICommandInfo.h"
#include "Framework/Commands/InputChord.h"
#include "InputCoreTypes.h"

#define LOCTEXT_NAMESPACE "FrostEditorCommands"

void FFrostEditorCommands::RegisterCommands()
{
    UI_COMMAND(PullFrost, "Pull Frost", "Fetch hourly data from Frost and write JSON cache", EUserInterfaceActionType::Button, FInputChord(EKeys::F, EModifierKey::Control | EModifierKey::Alt));
    UI_COMMAND(ToggleReloadProviders, "Reload Providers", "After pull: reload AFrostWeatherProviderActor in level", EUserInterfaceActionType::ToggleButton, FInputChord());
    UI_COMMAND(PullCsv, "Pull CSV", "Read configured CSV, slice to simulation span, write JSON", EUserInterfaceActionType::Button, FInputChord(EKeys::C, EModifierKey::Control | EModifierKey::Alt));
}

#undef LOCTEXT_NAMESPACE
