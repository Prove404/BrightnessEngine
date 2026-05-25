#pragma once

/**
* Returns if the given value is almost zero according to a given epsilon.
* 
* @param Value the value to check
* @param Epsilon the epsilon the value has to be smaller than to be considered zero
* @return if the given value is almost zero according to a given epsilon
*/
FORCEINLINE bool IsAlmostZero(float Value, float Espilon = 0.000001f)
{
	return FMath::Abs(Value) < Espilon;
}

FORCEINLINE float NormalizeAngle360(float A)
{
	const float TwoPi = 2.0f * PI;
	A = FMath::Fmod(A, TwoPi);
	return (A < 0.0f) ? A + TwoPi : A;
}
