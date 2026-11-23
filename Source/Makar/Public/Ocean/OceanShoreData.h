#pragma once

#include "CoreMinimal.h"

/**
 * Coastal data on a square grid, built once by AOceanBathymetry and then read-only:
 * shared by the renderer (as a texture), the CPU water queries and the foam simulation worker.
 */
struct MAKAR_API FOceanShoreData
{
	/** Returned outside the grid: open, deep sea far from any shore. */
	static constexpr float DeepWaterM = 1000.f;

	FVector2f OriginM = FVector2f::ZeroVector; // world position of the grid's lower corner
	float SizeM = 0.f;                        // grid width
	int32 Resolution = 0;

	/**
	 * Per texel: X = water depth (m, negative on land), Y = signed distance to the shoreline (m, positive at sea),
	 * Z = travel time of a wave from there to the shore (s, shallow-water speed sqrt(g h)), W = unused.
	 * Texel i is centred on (i + 0.5) / Resolution, like on the GPU.
	 */
	TArray<FVector4f> Texels;

	/** Bilinear sample, clamped at the borders like the GPU texture; deep open sea outside the grid. */
	FVector4f Sample(const FVector2f& PositionM) const;

	float DepthAt(const FVector2f& PositionM) const { return Sample(PositionM).X; }

	float TexelSizeM() const { return Resolution > 0 ? SizeM / Resolution : 1.f; }
};

/**
 * Waves feel the bottom and break once they are higher than ~0.78 x the water depth (McCowan's breaker criterion),
 * so shallow water cannot carry the open-sea waves. Each cascade is scaled so its significant height stays below
 * that limit. Must match OceanDepthAttenuation in Shaders/Ocean.ush.
 */
inline float OceanDepthAttenuation(float DepthM, float SignificantHeightM)
{
	return FMath::Clamp(0.78f * DepthM / FMath::Max(SignificantHeightM, 0.01f), 0.f, 1.f);
}

/** Smooth lattice value noise in [0, 1], one lattice cell per unit. Identical to OceanValueNoise in Shaders/Ocean.ush. */
MAKAR_API float OceanValueNoise(const FVector2f& P);

/**
 * Value-gradient noise, ~[0, 1] (slightly beyond at the extremes), one lattice cell per unit.
 * Bicubic Hermite cell constrained in height *and slope* at its corners (Thorimbert & Chopard, "Polynomial
 * methods for fast procedural terrain generation", configuration D2M1N3). OceanValueNoise is the zero-slope
 * case, whose flat spots at the lattice nodes show as grid-aligned patterns; random slopes remove them.
 * Identical to OceanGradientNoise in Shaders/Ocean.ush.
 */
MAKAR_API float OceanGradientNoise(const FVector2f& P);

/** Fractal sum of OceanGradientNoise, centred on 0: octaves halve the wavelength, Persistence scales the amplitude. */
MAKAR_API float OceanFractalNoise(const FVector2f& P, float WavelengthM, int32 Octaves, float Persistence);

/** Highest point of the beach the swash can reach is well below this, m above sea level. */
constexpr float OceanSwashMaxHeightM = 1.5f;

/** Vertical run-up of the swash on the beach, m: a fraction of the breaking wave height (gentle sandy beach). */
/** Run-up of each swash relative to the average: 0.65..1.1, one value per wave (Phase = time / period). Identical to OceanSwashWaveSize in Shaders/Ocean.ush. */
MAKAR_API float OceanSwashWaveSize(float Phase);

/** Tongues of the uprush: lobes a few metres wide, a new pattern per wave; multiplies the run-up (~0.75..1.25). Identical to OceanSwashTongues in Shaders/Ocean.ush. */
MAKAR_API float OceanSwashTongues(const FVector2f& PositionM, float Phase);

inline float OceanSwashRunup(float SwellHeightM, float Gaps)
{
	return 0.6f * SwellHeightM * Gaps;
}

/** Water level of the swash over one wave period, 0..1: fast uprush, slow backwash. Cycle 0 = the bore arrives. */
inline float OceanSwashRise(float Cycle)
{
	if (Cycle < 0.3f)
	{
		return FMath::Sin(0.5f * UE_PI * Cycle / 0.3f);
	}
	const float T = FMath::Clamp((Cycle - 0.3f) / 0.7f, 0.f, 1.f);
	return 1.f - T * T * (3.f - 2.f * T);
}

/** Swell rolling onto the coast and breaking on the beach (see OceanShoreWave). */
struct FOceanShoreSwell
{
	float Height = 0.8f;   // wave height over the shelf, m
	float Period = 8.f;    // s
	float MaxDepth = 12.f; // the swell fades out in deeper water, m
};

struct FOceanShoreWaveSample
{
	FVector3f Displacement = FVector3f::ZeroVector; // m
	float Foam = 0.f;                               // whitewater of the breaking crest, 0..1
};

/**
 * Breaking waves on the shore, the same shape everywhere (like the single baked cross-section of
 * Horizon Forbidden West, but analytic so the CPU evaluates exactly what the GPU draws):
 * a Gerstner swell whose crests follow the shore's wave travel time, grows as the water gets shallower
 * (Green's law), steepens and pitches forward near the breaking depth, then collapses into a bore in the surf zone;
 * on the beach, the swash raises the water over the sand after each bore.
 * Must match OceanShoreWave in Shaders/Ocean.ush.
 */
MAKAR_API FOceanShoreWaveSample OceanShoreWave(const FOceanShoreData& Shore, const FVector2f& PositionM, float TimeS, const FOceanShoreSwell& Swell);
