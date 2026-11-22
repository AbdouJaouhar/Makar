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
 * (Green's law), steepens and pitches forward near the breaking depth, then collapses into a bore in the surf zone.
 * Must match OceanShoreWave in Shaders/Ocean.ush.
 */
MAKAR_API FOceanShoreWaveSample OceanShoreWave(const FOceanShoreData& Shore, const FVector2f& PositionM, float TimeS, const FOceanShoreSwell& Swell);
