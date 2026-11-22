#pragma once

#include "CoreMinimal.h"
#include "Math/Float16Color.h"
#include "OceanSettings.h"

struct FOceanFrame;

/**
 * Persistent foam in world space around the camera, simulated on the CPU after each wave step.
 *
 * Foam is created where the combined surface (all cascades together, not each one alone) is compressed
 * enough to break, drifts with the wind and decays. Crests travel faster than the foam they leave behind,
 * which draws the streaks that follow each whitecap. The map follows the camera texel by texel.
 * Positions are undisplaced (Lagrangian) surface positions, like the wave textures, so the rendered foam
 * rides the displaced waves.
 */
class MAKAR_API FOceanFoamMap
{
public:
	static constexpr int32 Size = 512;
	static constexpr float TexelSizeM = 0.75f;
	static constexpr int32 NumMips = 10; // 512 -> 1

	/** Advances the foam by DeltaTime around CenterM (metres) and writes the map into Frame. Worker-thread safe. */
	void Simulate(FOceanFrame& Frame, const FOceanSettings& Settings, const FVector2f& CenterM, float DeltaTime);

private:
	void Recenter(const FIntPoint& NewOrigin);
	/** Bilinear foam at a texel position, 0 outside the map. */
	float SampleFoam(float X, float Y) const;
	void BuildMips(FOceanFrame& Frame) const;

	TArray<float> Foam;
	TArray<float> Next;
	FIntPoint OriginTexel = FIntPoint::ZeroValue; // world texel index of Foam[0]
	bool bInitialized = false;
};
