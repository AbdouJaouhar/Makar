#pragma once

#include "CoreMinimal.h"
#include "Math/Float16Color.h"
#include "OceanSettings.h"

struct FOceanFrame;
struct FOceanShoreData;

/**
 * Persistent foam in world space around the camera, simulated on the CPU after each wave step.
 *
 * Foam is created where the combined surface (all cascades together, not each one alone) is compressed
 * enough to break and where the shore waves break, rides the water (the wind drift, the shore waves' motion, the
 * swash up and down the beach), ages and decays; the map holds its amount (R) and age (G). Crests travel faster than the foam they leave behind,
 * which draws the streaks that follow each whitecap. The map follows the camera texel by texel.
 * Positions are undisplaced (Lagrangian) surface positions, like the wave textures, so the rendered foam
 * rides the displaced waves.
 */
class MAKAR_API FOceanFoamMap
{
public:
	/**
	 * InSize texels a side of InTexelSizeM metres. bInShore: the fine map of the shore (the coarse one blurs the swash
	 * into a smudge), made of the shore waves' foam only (the whitecaps are the coarse map's) and simulated only
	 * where the shore waves reach; it fills Frame.ShoreFoamMap* instead of Frame.FoamMap*.
	 */
	explicit FOceanFoamMap(int32 InSize = 512, float InTexelSizeM = 0.75f, bool bInShore = false)
		: Size(InSize), TexelSizeM(InTexelSizeM), NumMips(FMath::FloorLog2(uint32(InSize)) + 1), bShore(bInShore)
	{
	}

	const int32 Size;
	const float TexelSizeM;
	const int32 NumMips;
	const bool bShore;

	/**
	 * Advances the foam by DeltaTime around CenterM (metres) and writes the map into Frame. Worker-thread safe.
	 * Shore (optional) calms the waves in shallow water, exactly like the rendered surface.
	 */
	void Simulate(FOceanFrame& Frame, const FOceanSettings& Settings, const FOceanShoreData* Shore, const FVector2f& CenterM, float DeltaTime);

private:
	void Recenter(const FIntPoint& NewOrigin);
	/** Bilinear foam / age at a texel position, 0 outside the map. */
	float SampleFoam(float X, float Y) const;
	float SampleAge(float X, float Y) const;
	float SampleGrid(const TArray<float>& Grid, float X, float Y) const;
	void BuildMips(TArray<TArray<FFloat16Color>>& Mips) const;

	TArray<float> Foam;
	TArray<float> Next;
	TArray<float> Age;     // seconds since this foam was made
	TArray<float> NextAge;
	FIntPoint OriginTexel = FIntPoint::ZeroValue; // world texel index of Foam[0]
	bool bInitialized = false;
};
