#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "OceanShoreWater.generated.h"

class AOceanBathymetry;
class FRHIGPUTextureReadback;
class UMaterialInstanceDynamic;
class UTexture2D;
class UTextureRenderTarget2D;
struct FOceanShoreData;
struct FOceanShoreSwell;

/**
 * The shore's water simulated on the GPU around the camera (Shaders/OceanShoreSim.usf, Jeschke & Wojtan 2023): the
 * swell entering from the open sea, shoaling, refracting, crossing, running up the beach as swash and draining back,
 * instead of the analytic shore wave. Owned and stepped by AOceanActor; the ocean material draws its water surface
 * (ShoreWater, ShoreWaterOrigin, ShoreWaterSize) and the CPU water queries read it back, so rendering and physics
 * agree (a couple of frames late).
 */
UCLASS()
class MAKAR_API UOceanShoreWaterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UOceanShoreWaterComponent();

	/** Cells a side (power of two) and their size: 512 x 0.5 m = 256 m around the camera. */
	UPROPERTY(EditAnywhere, Category = "Shore Water", meta = (ClampMin = "64", ClampMax = "1024"))
	int32 Resolution = 512;

	UPROPERTY(EditAnywhere, Category = "Shore Water", meta = (ClampMin = "0.1", ClampMax = "4"))
	float CellSizeM = 0.5f;

	/** Explicit diffusion iterations of the wave decomposition (the paper: 128). */
	UPROPERTY(EditAnywhere, Category = "Shore Water", meta = (ClampMin = "8", ClampMax = "128"))
	int32 DiffusionIterations = 48;

	UPROPERTY(EditAnywhere, Category = "Shore Water")
	bool bEnabled = true;

	/**
	 * Advances the water to SimulationTime (the ocean's clock, which also phases the swell entering at the edges)
	 * around CenterM, and feeds the material. Game thread.
	 */
	void Step(float SimulationTime, const FVector2f& CenterM, const FOceanShoreData* Shore, const AOceanBathymetry* Bathymetry,
		float SeaLevelCm, const FOceanShoreSwell& Swell, UMaterialInstanceDynamic* Material);

	/** Water surface height (m above sea level) from the last read-back frame; false outside the simulated square. */
	bool SampleHeight(const FVector2f& PositionM, float& OutHeightM) const;

	/** The square simulated now (m): lower corner and width; width 0 when off. */
	FVector2f GetOriginM() const { return FVector2f(OriginCell) * CellSizeM; }
	float GetSizeM() const { return bInitialized ? Resolution * CellSizeM : 0.f; }

protected:
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	void CreateTargets();
	/** Bed height and swell travel time over the square at OriginCell, uploaded to TerrainTexture. */
	void BuildTerrain(const FOceanShoreData* Shore, const AOceanBathymetry* Bathymetry, float SeaLevelCm);

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> StateTarget;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> AuxTarget;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> WaterTarget;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> TerrainTexture;

	FIntPoint OriginCell = FIntPoint::ZeroValue; // world cell index of cell (0, 0)
	bool bInitialized = false;
	bool bNeedsReset = true;
	FIntPoint PendingShift = FIntPoint::ZeroValue;
	float LastTime = -1.f;
	float Accumulator = 0.f;

	/** CPU copy of the water surface height, for the queries: filled on the render thread from a GPU readback. */
	struct FHeights
	{
		FCriticalSection Lock;
		TArray<float> Data;
		FIntPoint Origin = FIntPoint::ZeroValue;
		// Render thread only.
		TUniquePtr<FRHIGPUTextureReadback> Readback;
		FIntPoint PendingOrigin = FIntPoint::ZeroValue;
		bool bPending = false;
	};
	TSharedPtr<FHeights, ESPMode::ThreadSafe> Heights;
};
