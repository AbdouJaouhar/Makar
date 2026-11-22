#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Ocean/OceanShoreData.h"
#include "OceanBathymetry.generated.h"

class UMaterialInterface;
class UOceanMeshComponent;
class UTexture2D;

/** The island's shape at one point, m. */
struct FOceanTerrainSample
{
	float Ground = 0.f;     // ground height above sea level (negative under water)
	float Understory = 0.f; // bush mass above the ground, part of the terrain mesh
	float Rock = 0.f;       // 0..1, rocky shore
	float Inland = 0.f;     // approximate distance from the coastline, positive on land
};

/**
 * A procedural island and its sea floor, centred on the actor: land rising from a sandy beach,
 * a shallow shelf (clear turquoise water) and a reef drop-off into deep water.
 *
 * Builds the terrain mesh and the coastal data (depth, distance to shore, wave travel time to shore) that the
 * ocean uses to calm its waves in shallow water and to break swell on the beach, consistently on the GPU and in
 * UOceanSubsystem queries.
 * Sea level is Z = 0.
 */
UCLASS()
class MAKAR_API AOceanBathymetry : public AActor
{
	GENERATED_BODY()

public:
	AOceanBathymetry();

	/** Builds the terrain and shore data if needed. Safe to call several times. */
	void EnsureBuilt();

	/** Immutable once built; safe to read from worker threads. Null before the first build. */
	TSharedPtr<const FOceanShoreData> GetShoreData() const { return ShoreData; }

	/** RGBA = depth, signed distance to shore, wave travel time to shore, unused (see FOceanShoreData). */
	UTexture2D* GetShoreTexture() const { return ShoreTexture; }

	/** Terrain height above sea level (m) from the analytic island shape: ground plus understory. */
	float GetTerrainHeight(const FVector2f& PositionM) const;

	/** Ground, understory, rockiness and distance inland, for placing vegetation and rocks. */
	FOceanTerrainSample SampleTerrain(const FVector2f& PositionM) const;

	virtual void OnConstruction(const FTransform& Transform) override;

protected:
	virtual void BeginPlay() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Radius of the island at the waterline. */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "10", Units = "Meters"))
	float IslandRadius = 250.f;

	/** How far the coastline wanders in and out of the circle (large bays and headlands). */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "0", Units = "Meters"))
	float CoastlineVariation = 45.f;

	/** Amplitude of the fractal detail of the coastline (coves, points), from 120 m wavelength down. */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "0", Units = "Meters"))
	float CoastlineDetail = 45.f;

	/**
	 * Amplitude ratio between successive octaves of the coastline detail. In theory a contour of a fractal surface
	 * with Hurst exponent H has dimension 2 - H with Persistence = 2^-H (0.57 for D = 1.2), but on a 250 m island
	 * sampled at 2.5 m the large circle dominates. Measured (box counting, detail 45 m): 0.57 -> 1.08,
	 * 0.65 -> 1.12, 0.72 -> 1.18, 0.8 -> 1.27; 0.75 gives ~1.2, like real coasts.
	 */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "0.1", ClampMax = "0.9"))
	float CoastlinePersistence = 0.75f;

	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "1", ClampMax = "10"))
	int32 CoastlineOctaves = 6;

	/** Box-counting dimension of the coastline measured on the last build (real coasts: ~1.15-1.25). */
	UPROPERTY(VisibleAnywhere, Category = "Island")
	float CoastlineFractalDimension = 0.f;

	/** Height of the beach berm, the sand bank the swash builds above the waterline. */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "0.1", Units = "Meters"))
	float BermHeight = 1.8f;

	/** Rise per metre inland at the waterline. */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "0.001"))
	float LandSlope = 0.08f;

	/** Height of the wooded hills inland. */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "0", Units = "Meters"))
	float HillHeight = 28.f;

	/** 0 = sandy coast all around, 1 = mostly rocky points. */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "0", ClampMax = "1"))
	float Rockiness = 0.5f;

	/** Drop per metre offshore at the waterline (0.05 = 1:20, a gentle sandy beach). */
	UPROPERTY(EditAnywhere, Category = "Sea floor", meta = (ClampMin = "0.001"))
	float BeachSlope = 0.05f;

	/** Depth the sea floor levels off at, over the shelf. */
	UPROPERTY(EditAnywhere, Category = "Sea floor", meta = (ClampMin = "0.5", Units = "Meters"))
	float ShelfDepth = 7.f;

	/** Distance from the shore to the reef drop-off. */
	UPROPERTY(EditAnywhere, Category = "Sea floor", meta = (ClampMin = "0", Units = "Meters"))
	float ShelfWidth = 450.f;

	UPROPERTY(EditAnywhere, Category = "Sea floor", meta = (ClampMin = "0", Units = "Meters"))
	float DropOffDepth = 300.f;

	/** Horizontal extent of the drop-off slope. */
	UPROPERTY(EditAnywhere, Category = "Sea floor", meta = (ClampMin = "1", Units = "Meters"))
	float DropOffWidth = 250.f;

	UPROPERTY(EditAnywhere, Category = "Island")
	int32 Seed = 7;

	/** Width of the area covered by the terrain mesh and the shore data. */
	UPROPERTY(EditAnywhere, Category = "Grid", meta = (ClampMin = "100", Units = "Meters"))
	float GridSize = 2560.f;

	/** Samples per side (power of two keeps the distance transform exact). */
	UPROPERTY(EditAnywhere, Category = "Grid", meta = (ClampMin = "64", ClampMax = "1024"))
	int32 GridResolution = 1024;

	UPROPERTY(EditAnywhere, Category = "Rendering")
	TSoftObjectPtr<UMaterialInterface> TerrainMaterial;

	UPROPERTY(VisibleAnywhere, Category = "Rendering")
	TObjectPtr<UOceanMeshComponent> Terrain;

private:
	void Build();
	void BuildShoreTexture(const FOceanShoreData& Data);

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> ShoreTexture;

	TSharedPtr<const FOceanShoreData> ShoreData;
	bool bBuilt = false;
};
