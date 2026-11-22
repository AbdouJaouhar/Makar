#pragma once

#include "CoreMinimal.h"
#include "Ocean/OceanShoreData.h"
#include "OceanSettings.generated.h"

/**
 * Sea state. Distances in metres, speeds in m/s.
 * The wave spectrum is JONSWAP with Hasselmann directional spreading
 * (see Horvath 2015, "Empirical directional wave spectra for computer graphics").
 */
USTRUCT(BlueprintType)
struct FOceanSettings
{
	GENERATED_BODY()

	// --- Wind / sea state ---

	/** Wind speed 10 m above the sea. 5 = light breeze, 10 = fresh, 20 = gale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind", meta = (ClampMin = "0.5", ClampMax = "40", Units = "MetersPerSecond"))
	float WindSpeed = 10.f;

	/** Direction the wind blows towards, degrees in the XY plane (0 = +X). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind")
	float WindDirection = 0.f;

	/** Distance over which the wind has been blowing. Longer fetch = bigger, longer waves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind", meta = (ClampMin = "1000", Units = "Meters"))
	float Fetch = 100000.f;

	/** Water depth. Shallow water slows waves down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind", meta = (ClampMin = "1", Units = "Meters"))
	float Depth = 500.f;

	/** JONSWAP peak sharpening (3.3 is the measured North Sea average). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind", meta = (ClampMin = "1", ClampMax = "7"))
	float PeakEnhancement = 3.3f;

	/** 0 = pure wind sea, 1 = long aligned swell. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind", meta = (ClampMin = "0", ClampMax = "1"))
	float Swell = 0.2f;

	/** Blend towards waves coming from every direction (0 = physical spreading, 1 = isotropic). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind", meta = (ClampMin = "0", ClampMax = "1"))
	float DirectionalSpread = 0.f;

	/** Damps wavelengths shorter than roughly this length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind", meta = (ClampMin = "0", Units = "Meters"))
	float ShortWaveFade = 0.02f;

	/** Artistic multiplier on wave height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind", meta = (ClampMin = "0"))
	float AmplitudeScale = 1.f;

	/** Horizontal displacement: sharp crests, wide troughs. Above ~1.2 crests fold over. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind", meta = (ClampMin = "0", ClampMax = "2"))
	float Choppiness = 1.f;

	// --- Foam ---

	/** Foam appears where the surface is compressed below this (1 = flat, <0 = folding crest). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foam")
	float FoamThreshold = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foam", meta = (ClampMin = "0"))
	float FoamIntensity = 2.f;

	/** How fast foam fades, 1/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foam", meta = (ClampMin = "0"))
	float FoamDecay = 0.6f;

	// --- Shore (needs an AOceanBathymetry in the level) ---

	/** Height of the swell rolling onto the coast, over the shelf. It grows and breaks in shallow water. 0 = none. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shore", meta = (ClampMin = "0", Units = "Meters"))
	float ShoreSwellHeight = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shore", meta = (ClampMin = "2", Units = "Seconds"))
	float ShoreSwellPeriod = 8.f;

	/** The swell fades out in deeper water (off the shelf). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shore", meta = (ClampMin = "1", Units = "Meters"))
	float ShoreSwellMaxDepth = 12.f;

	FOceanShoreSwell GetShoreSwell() const { return { ShoreSwellHeight, ShoreSwellPeriod, ShoreSwellMaxDepth }; }

	// --- Simulation ---

	/** FFT grid size per cascade (power of two). 128 is cheap, 256 sharper. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simulation", meta = (ClampMin = "32", ClampMax = "512"))
	int32 Resolution = 128;

	/**
	 * Tile size of each of the 3 cascades. Large = swell, small = ripples.
	 * Keep them non-multiples of each other so the tiling never lines up.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simulation", meta = (Units = "Meters"))
	float Cascade0Size = 500.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simulation", meta = (Units = "Meters"))
	float Cascade1Size = 85.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simulation", meta = (Units = "Meters"))
	float Cascade2Size = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simulation")
	int32 Seed = 1234;

	/**
	 * Resolution of the finest cascade (Cascade2Size), simulated on the GPU by compute shaders (power of two, up to
	 * 1024); 0 = on the CPU with the others, at Resolution. That cascade only shades the surface (normals, glints,
	 * foam, caustics): geometry and water queries use the CPU cascades, so they stay exactly consistent.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simulation", meta = (ClampMin = "0", ClampMax = "1024"))
	int32 GpuDetailResolution = 512;
};
