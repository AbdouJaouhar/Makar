#pragma once

#include "CoreMinimal.h"

/**
 * Footprints in dry sand, after Sumner, O'Brien & Hodgins, "Animating Sand, Mud, and Snow" (1999): a heightfield of
 * sand columns that each foot pushes down (collision), whose displaced sand is partly compressed and partly heaped
 * into a rim around the print, heavier at the toe where the foot pushes off (displacement), then slides down until
 * no slope is steeper than the angle of repose (erosion). Trails of walkers and runners are stamped one after the
 * other, the wind softening the older ones in between: a beach at the end of the day.
 *
 * The result is a periodic tile, tiled over the beach by the terrain material (docs/StrategieSableProcedural.md,
 * layer 2).
 */
struct FOceanSandFootprintSettings
{
	float TileSizeM = 8.f;
	int32 Resolution = 512;
	int32 Trails = 9;
	int32 Seed = 3;
	float FootDepthM = 0.03f;
	/** Fraction of the displaced sand that is compressed rather than heaped (Sumner's alpha). */
	float Compression = 0.3f;
	/** Steepest stable slope after erosion, rad (Sumner's theta_out for sand). */
	float ReposeAngle = 0.436f;
	/** Fraction of the excess height moved per erosion step (Sumner's sigma). */
	float ErosionRate = 0.2f;
};

/** Texels, row-major Resolution^2: X = height offset (m), Y = disturbance 0..1 (trampled sand, no ripples). */
MAKAR_API TArray<FVector2f> OceanSimulateSandFootprints(const FOceanSandFootprintSettings& Settings);
