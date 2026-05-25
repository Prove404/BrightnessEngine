#include "Frost/AtmosRadiation.h"

namespace
{
	constexpr float SolarConstant = 1361.0f; // W/m^2
	constexpr float Sigma = 5.670374419e-8f; // Stefan-Boltzmann constant
}

void UAtmosRadiation::EstimateSWLW(const FDateTime& TimeUTC, const FSiteMeta& Site,
	float T_C, float RH_pct,
	float& OutSW, float& OutLW, float CloudCover_pct)
{
	const double LatRad = FMath::DegreesToRadians(static_cast<double>(Site.LatDeg));
	const double LonDeg = static_cast<double>(Site.LonDeg);

	const int32 DayOfYear = TimeUTC.GetDayOfYear();
	const double B = 2.0 * PI * (DayOfYear - 81) / 364.0;
	const double EquationOfTime = 9.87 * FMath::Sin(2.0 * B) - 7.53 * FMath::Cos(B) - 1.5 * FMath::Sin(B);

	const double MinutesUTC = TimeUTC.GetHour() * 60.0 + TimeUTC.GetMinute() + TimeUTC.GetSecond() / 60.0;
	const double TimeCorrection = EquationOfTime + 4.0 * LonDeg; // minutes
	const double LocalSolarMinutes = MinutesUTC + TimeCorrection;
	const double LocalSolarHours = LocalSolarMinutes / 60.0;
	const double HourAngleDeg = 15.0 * (LocalSolarHours - 12.0);
	const double HourAngleRad = FMath::DegreesToRadians(HourAngleDeg);

	const double DeclinationRad = FMath::DegreesToRadians(23.45 * FMath::Sin(FMath::DegreesToRadians(360.0 * (284 + DayOfYear) / 365.0)));
	const double SinAlt = FMath::Sin(LatRad) * FMath::Sin(DeclinationRad) +
		FMath::Cos(LatRad) * FMath::Cos(DeclinationRad) * FMath::Cos(HourAngleRad);

	float Shortwave = 0.0f;
	if (SinAlt > 0.0)
	{
		const float TauClear = 0.75f;
		float CloudFactor = 1.0f;
		if (CloudCover_pct >= 0.0f)
		{
			// Use explicit cloud cover if available
			// Simple linear attenuation: 1.0 at 0% clouds, 0.25 at 100% clouds
			CloudFactor = FMath::Lerp(1.0f, 0.25f, FMath::Clamp(CloudCover_pct / 100.0f, 0.0f, 1.0f));
		}
		else
		{
			// Fallback to RH-based approximation
			CloudFactor = FMath::Clamp(1.0f - (RH_pct / 150.0f), 0.15f, 1.0f);
		}
		Shortwave = static_cast<float>(SolarConstant * TauClear * CloudFactor * SinAlt);
	}
	Shortwave = FMath::Clamp(Shortwave, 0.0f, 1100.0f);

	const double TempK = FMath::Max(T_C + 273.15, 50.0);
	const double Es = 0.6108 * FMath::Exp((17.27 * T_C) / (T_C + 237.3)); // kPa
	const double Ea = FMath::Clamp(RH_pct / 100.0, 0.0, 1.0) * Es; // kPa

	// Brutsaert (1975) formula expects Ea in millibars (hPa). 1 kPa = 10 hPa.
	// Emissivity = 1.24 * (Ea_hPa / Ta_K)^(1/7)
	const double Ea_hPa = Ea * 10.0;
	const double EmissivityClear = 1.24 * FMath::Pow(Ea_hPa / TempK, 1.0 / 7.0);

	// Cloud correction (Bolz, 1949 or similar)
	// LW = LW_clear * (1 + k * Cloud^2)
	// k is typically 0.17 to 0.25. We use 0.22.
	const float Cloud01 = FMath::Clamp(CloudCover_pct / 100.0f, 0.0f, 1.0f);
	const double CloudFactorLW = 1.0 + 0.22 * (Cloud01 * Cloud01);

	float Longwave = static_cast<float>(EmissivityClear * Sigma * FMath::Pow(TempK, 4.0) * CloudFactorLW);
	Longwave = FMath::Clamp(Longwave, 150.0f, 450.0f);

	OutSW = Shortwave;
	OutLW = Longwave;
}

