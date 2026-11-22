#include "Ocean/OceanShoreData.h"

namespace
{
	// Integer hash and lattice value noise: identical on the GPU (Shaders/Ocean.ush), so the gaps
	// in the breaking waves are the same for rendering and water queries.
	uint32 ShoreHash(uint32 X)
	{
		X ^= X >> 16;
		X *= 0x7feb352du;
		X ^= X >> 15;
		X *= 0x846ca68bu;
		X ^= X >> 16;
		return X;
	}

	float LatticeValue(int32 X, int32 Y)
	{
		return float(ShoreHash(uint32(X) * 1597334677u ^ ShoreHash(uint32(Y)))) / 4294967295.f;
	}

	float ValueNoiseImpl(const FVector2f& P)
	{
		const float FloorX = FMath::FloorToFloat(P.X);
		const float FloorY = FMath::FloorToFloat(P.Y);
		const int32 X = int32(FloorX);
		const int32 Y = int32(FloorY);
		const float FX = P.X - FloorX;
		const float FY = P.Y - FloorY;
		const float UX = FX * FX * (3.f - 2.f * FX);
		const float UY = FY * FY * (3.f - 2.f * FY);
		const float Bottom = FMath::Lerp(LatticeValue(X, Y), LatticeValue(X + 1, Y), UX);
		const float Top = FMath::Lerp(LatticeValue(X, Y + 1), LatticeValue(X + 1, Y + 1), UX);
		return FMath::Lerp(Bottom, Top, UY);
	}

	float SmoothStep01(float Edge0, float Edge1, float X)
	{
		const float T = FMath::Clamp((X - Edge0) / (Edge1 - Edge0), 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}
}

float OceanValueNoise(const FVector2f& P)
{
	return ValueNoiseImpl(P);
}

float OceanSwashWaveSize(float Phase)
{
	const uint32 Wave = uint32(int32(FMath::FloorToFloat(Phase)));
	return FMath::Lerp(0.65f, 1.1f, float(ShoreHash(Wave * 0x9e3779b9u)) / 4294967295.f);
}

namespace
{
	/** Height in [0, 1] and slope in [-1, 1]^2 at a lattice node. */
	void LatticeNode(int32 X, int32 Y, float& OutValue, FVector2f& OutSlope)
	{
		const uint32 Hash = ShoreHash(uint32(X) * 1597334677u ^ ShoreHash(uint32(Y)));
		OutValue = float(Hash) / 4294967295.f;
		OutSlope.X = float(ShoreHash(Hash ^ 0x68bc21ebu)) / 4294967295.f * 2.f - 1.f;
		OutSlope.Y = float(ShoreHash(Hash ^ 0x02e5be93u)) / 4294967295.f * 2.f - 1.f;
	}
}

float OceanGradientNoise(const FVector2f& P)
{
	const float FloorX = FMath::FloorToFloat(P.X);
	const float FloorY = FMath::FloorToFloat(P.Y);
	const int32 X = int32(FloorX);
	const int32 Y = int32(FloorY);
	const float TX = P.X - FloorX;
	const float TY = P.Y - FloorY;

	// Cubic Hermite basis: value weights H and slope weights T of the cell's corner 0 and corner 1.
	const float HX[2] = { 1.f - TX * TX * (3.f - 2.f * TX), TX * TX * (3.f - 2.f * TX) };
	const float HY[2] = { 1.f - TY * TY * (3.f - 2.f * TY), TY * TY * (3.f - 2.f * TY) };
	const float SX[2] = { TX * (1.f - TX) * (1.f - TX), TX * TX * (TX - 1.f) };
	const float SY[2] = { TY * (1.f - TY) * (1.f - TY), TY * TY * (TY - 1.f) };

	float Sum = 0.f;
	for (int32 J = 0; J < 2; ++J)
	{
		for (int32 I = 0; I < 2; ++I)
		{
			float Value;
			FVector2f Slope;
			LatticeNode(X + I, Y + J, Value, Slope);
			Sum += Value * HX[I] * HY[J] + Slope.X * SX[I] * HY[J] + Slope.Y * HX[I] * SY[J];
		}
	}
	return Sum;
}

float OceanFractalNoise(const FVector2f& P, float WavelengthM, int32 Octaves, float Persistence)
{
	float Sum = 0.f;
	float Amplitude = 1.f;
	float Wavelength = WavelengthM;
	for (int32 Octave = 0; Octave < Octaves; ++Octave)
	{
		// A different offset per octave so the octaves' lattices never line up.
		Sum += Amplitude * (OceanGradientNoise(P / Wavelength + FVector2f(17.3f, 41.9f) * float(Octave)) - 0.5f);
		Amplitude *= Persistence;
		Wavelength *= 0.5f;
	}
	return Sum;
}

FOceanShoreWaveSample OceanShoreWave(const FOceanShoreData& Shore, const FVector2f& PositionM, float TimeS, const FOceanShoreSwell& Swell)
{
	FOceanShoreWaveSample Out;
	if (Shore.Resolution <= 0 || Swell.Height <= 0.f || Swell.Period <= 0.f)
	{
		return Out;
	}

	const FVector4f Here = Shore.Sample(PositionM);
	const float Depth = Here.X;
	if (Depth >= Swell.MaxDepth || Depth < -OceanSwashMaxHeightM)
	{
		return Out;
	}

	// Gaps along the wavefronts, drifting slowly (real sets never break as one straight wall).
	const float Gaps = SmoothStep01(0.35f, 0.6f, ValueNoiseImpl(PositionM / 60.f + FVector2f(0.01f, 0.007f) * TimeS));

	// Swash: after each bore reaches the beach, a thin sheet of water runs up the sand and drains back.
	// The sea surface itself rises over the beach (where it is lower than the water, the sand hides it),
	// so the waterline moves up and down the slope, and water queries see the water on the sand.
	{
		// Each wave runs up its own distance: a set is never uniform.
		const float Phase = (TimeS + FMath::Max(Here.Z, 0.f)) / Swell.Period;
		const float Runup = OceanSwashRunup(Swell.Height, Gaps) * OceanSwashWaveSize(Phase);
		const float Cycle = FMath::Frac(Phase);
		const float Level = Runup * (1.1f * OceanSwashRise(Cycle) - 0.1f);
		const float Weight = 1.f - SmoothStep01(0.5f, 1.5f, Depth); // beach and the last metre of water
		Out.Displacement.Z = Level * Weight;
		// A lace of foam rides the edge of the uprush.
		if (Depth < 0.f && Cycle < 0.6f)
		{
			Out.Foam = 0.6f * Gaps * (1.f - SmoothStep01(0.f, 0.08f, FMath::Abs(Level + Depth)));
		}
	}

	if (Depth <= 0.02f)
	{
		return Out;
	}

	// Crests follow the travel-time field: they move towards the shore at sqrt(g h), slowing, bunching up
	// and bending with the sea floor. Its gradient gives the direction and the local wavenumber (|grad| = 1/c).
	const float Step = Shore.TexelSizeM();
	const float GradX = (Shore.Sample(PositionM + FVector2f(Step, 0.f)).Z - Here.Z) / Step;
	const float GradY = (Shore.Sample(PositionM + FVector2f(0.f, Step)).Z - Here.Z) / Step;
	const float GradLength = FMath::Sqrt(GradX * GradX + GradY * GradY);
	if (GradLength < 1e-5f)
	{
		return Out;
	}
	const FVector2f TowardShore(-GradX / GradLength, -GradY / GradLength);
	const float Phase = 2.f * UE_PI * (TimeS + Here.Z) / Swell.Period;
	const float WaveNumber = 2.f * UE_PI * GradLength / Swell.Period;

	// Height: shoaling (Green's law, H ~ h^-1/4) until it reaches the breaking limit 0.78 h,
	// then a bore whose height keeps shrinking with the depth.
	const float ReferenceDepth = 0.5f * Swell.MaxDepth;
	const float Shoaled = Swell.Height * FMath::Pow(ReferenceDepth / FMath::Max(Depth, 0.05f), 0.25f);
	const float Ratio = Shoaled / (0.78f * Depth);
	const float Pitch = SmoothStep01(0.7f, 1.f, Ratio);              // steepening and plunging
	const float Surf = FMath::Clamp((Ratio - 1.f) / 1.5f, 0.f, 1.f); // broken, in the surf zone
	const float Height = FMath::Min(Shoaled, 0.78f * Depth * (1.f - 0.5f * Surf));

	const float FadeDeep = 1.f - SmoothStep01(0.7f * Swell.MaxDepth, Swell.MaxDepth, Depth);
	const float Amplitude = 0.5f * Height * FadeDeep * Gaps;

	// Gerstner orbit, sharper near breaking, plus a lip thrown forward at the crest.
	const float Steepness = FMath::Lerp(0.5f, 1.f, Pitch);
	const float Orbit = FMath::Min(Steepness / FMath::Max(WaveNumber, 1e-4f), 1.5f * Amplitude);
	const float Crest = 0.5f * (1.f + FMath::Cos(Phase));
	const float Lip = Amplitude * Crest * Crest * Crest * (1.2f * Pitch * (1.f - Surf) + 0.6f * Surf);
	const float Forward = Orbit * FMath::Sin(Phase) + Lip;

	Out.Displacement += FVector3f(TowardShore.X * Forward, TowardShore.Y * Forward, Amplitude * FMath::Cos(Phase));
	// Dense on the face of the bore, below full coverage so the bubble pattern shows; the foam map turns
	// what it leaves behind into a lacy trail.
	Out.Foam = FMath::Max(Out.Foam, FadeDeep * Gaps * FMath::Max(0.75f * FMath::Clamp(2.f * Surf, 0.f, 1.f) * SmoothStep01(0.55f, 0.95f, Crest),
		0.6f * Pitch * (1.f - Surf) * SmoothStep01(0.9f, 1.f, Crest)));
	return Out;
}

FVector4f FOceanShoreData::Sample(const FVector2f& PositionM) const
{
	if (Resolution <= 0 || SizeM <= 0.f || Texels.Num() != Resolution * Resolution)
	{
		return FVector4f(DeepWaterM, DeepWaterM, 0.f, 0.f);
	}

	const FVector2f Uv = (PositionM - OriginM) / SizeM;
	if (Uv.X < 0.f || Uv.X > 1.f || Uv.Y < 0.f || Uv.Y > 1.f)
	{
		return FVector4f(DeepWaterM, DeepWaterM, 0.f, 0.f);
	}

	const float U = FMath::Clamp(Uv.X * Resolution - 0.5f, 0.f, float(Resolution - 1));
	const float V = FMath::Clamp(Uv.Y * Resolution - 0.5f, 0.f, float(Resolution - 1));
	const int32 X0 = int32(U);
	const int32 Y0 = int32(V);
	const int32 X1 = FMath::Min(X0 + 1, Resolution - 1);
	const int32 Y1 = FMath::Min(Y0 + 1, Resolution - 1);
	const float FracU = U - X0;
	const float FracV = V - Y0;

	const FVector4f Bottom = FMath::Lerp(Texels[Y0 * Resolution + X0], Texels[Y0 * Resolution + X1], FracU);
	const FVector4f Top = FMath::Lerp(Texels[Y1 * Resolution + X0], Texels[Y1 * Resolution + X1], FracU);
	return FMath::Lerp(Bottom, Top, FracV);
}
