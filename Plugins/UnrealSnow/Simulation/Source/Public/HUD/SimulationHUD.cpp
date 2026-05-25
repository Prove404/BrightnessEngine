#include "SimulationHUD.h"
#include "Simulation.h"
#include "SnowSimulationActor.h"
#include "Engine/Font.h"
#include "Engine/Engine.h"

void ASimulationHUD::InitializeDefaultFont()
{
	if (!UE4Font)
	{
		// Try multiple fallback methods for UE 5.6
		static UFont* DefaultFont = nullptr;

		if (!DefaultFont)
		{
			// Try runtime loading (ConstructorHelpers can't be used outside constructors)
			DefaultFont = LoadObject<UFont>(nullptr, TEXT("/Engine/EngineFonts/Roboto"));
		}

		if (!DefaultFont)
		{
			// Try FiraSans as alternative
			DefaultFont = LoadObject<UFont>(nullptr, TEXT("/Engine/EngineFonts/FiraSans"));
		}

		if (!DefaultFont)
		{
			// Try to find existing font object
			DefaultFont = FindObject<UFont>(nullptr, TEXT("/Engine/EngineFonts/Roboto"));
		}

		if (!DefaultFont)
		{
			// Try other common font locations in UE 5.6
			DefaultFont = LoadObject<UFont>(nullptr, TEXT("/Engine/EngineFonts/NotoSans"));
		}

		if (!DefaultFont)
		{
			// Last resort - try to get from engine
			if (GEngine && GEngine->GetSmallFont())
			{
				DefaultFont = GEngine->GetSmallFont();
			}
		}

		if (DefaultFont)
		{
			UE4Font = DefaultFont;
			UE_LOG(LogTemp, Display, TEXT("[SimulationHUD] Successfully loaded default font"));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[SimulationHUD] Could not load any default font"));
		}
	}
}

void ASimulationHUD::BeginPlay()
{
	Super::BeginPlay();

	// Ensure font is initialized early
	InitializeDefaultFont();
}

void ASimulationHUD::DrawHUD()
{
	// Find the first available SnowSimulationActor in the world
	ASnowSimulationActor* Simulation = nullptr;

	// Try multiple methods to find the simulation actor
	for (TActorIterator<ASnowSimulationActor> SimulationIt(GetWorld()); SimulationIt; ++SimulationIt)
	{
		ASnowSimulationActor* Candidate = *SimulationIt;
		if (Candidate && Candidate->IsValidLowLevel())
		{
			Simulation = Candidate;
			break; // Use the first valid one found
		}
	}

	// If no simulation actor found, try getting it from the game mode or other sources
	if (!Simulation)
	{
		// Log for debugging
		UE_LOG(LogTemp, Warning, TEXT("[SimulationHUD] No SnowSimulationActor found in world"));
		return;
	}

	if (Simulation->DrawDate)
	{
		const FDateTime SimTime = Simulation->CurrentSimulationTime;
		const bool bHasValidTime = SimTime.GetTicks() != 0;
		const FString DisplayString = bHasValidTime ? SimTime.ToString(TEXT("%Y.%m.%d %H:%M")) : TEXT("----.--.-- --:--");

		// Check if font is valid
		UFont* FontToUse = UE4Font;
		if (!FontToUse)
		{
			// Initialize default font if not set
			InitializeDefaultFont();
			FontToUse = UE4Font;
		}

		if (!FontToUse)
		{
			// Last resort fallbacks for UE 5.6
			static UFont* CachedFallbackFont = nullptr;
			if (!CachedFallbackFont)
			{
				// Try to find a font in common UE locations
				CachedFallbackFont = LoadObject<UFont>(nullptr, TEXT("/Engine/EngineFonts/Roboto"));
				if (!CachedFallbackFont)
				{
					CachedFallbackFont = LoadObject<UFont>(nullptr, TEXT("/Engine/EngineFonts/FiraSans"));
				}
				if (!CachedFallbackFont)
				{
					// Try to get from GEngine if available
					if (GEngine && GEngine->GetSmallFont())
					{
						CachedFallbackFont = GEngine->GetSmallFont();
					}
				}
			}
			FontToUse = CachedFallbackFont;

			if (!FontToUse)
			{
				UE_LOG(LogTemp, Error, TEXT("[SimulationHUD] No font available for drawing"));
				return;
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[SimulationHUD] Using fallback font"));
			}
		}

		if (FontToUse && Canvas)
		{
			FText DateText = FText::FromString(DisplayString);
			FCanvasTextItem TextItem(FVector2D(20, 20), DateText, FontToUse, FColor(255, 255, 255, 255));
			TextItem.Scale.Set(1.25f, 1.25f);
			TextItem.bOutlined = true; // Add outline for better visibility
			Canvas->DrawItem(TextItem);

			// Log for debugging
			UE_LOG(LogTemp, Verbose, TEXT("[SimulationHUD] Drew simulation time: %s"), *DisplayString);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("[SimulationHUD] Cannot draw text: Font=%s, Canvas=%s"),
				FontToUse ? TEXT("Valid") : TEXT("Null"), Canvas ? TEXT("Valid") : TEXT("Null"));
		}
	}
}
