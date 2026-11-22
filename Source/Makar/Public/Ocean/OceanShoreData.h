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

	/** Per texel: X = water depth (m, negative on land), Y = signed distance to the shoreline (m, positive at sea),
	 *  ZW = unit direction pointing out to sea. Texel i is centred on (i + 0.5) / Resolution, like on the GPU. */
	TArray<FVector4f> Texels;

	/** Bilinear sample, clamped at the borders like the GPU texture; deep open sea outside the grid. */
	FVector4f Sample(const FVector2f& PositionM) const;

	float DepthAt(const FVector2f& PositionM) const { return Sample(PositionM).X; }
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
