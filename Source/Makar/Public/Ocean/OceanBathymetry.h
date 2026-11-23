#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Ocean/OceanErosion.h"
#include "Ocean/OceanShoreData.h"
#include "OceanBathymetry.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UOceanMeshComponent;
class UTexture2D;

/** The island's shape at one point, m. */
struct FOceanTerrainSample
{
	float Ground = 0.f;     // ground height above sea level (negative under water)
	float Understory = 0.f; // bush mass above the ground (UnderstoryRelief of it in the terrain mesh, the rest shrubs)
	float Rock = 0.f;       // 0..1, rocky shore
	float Inland = 0.f;     // approximate distance from the coastline, positive on land
	float Hills = 0.f;      // the massif above the beach before erosion, m (its uplift in the landscape evolution)
	float Erosion = 0.f;    // what erosion changed here, m (+ deposited sediment, - carved gully)
	float Flow = 0.f;       // runoff, 0 on ridges, ~1 in the main gullies
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

	/** The terrain's material instance (sand, sea floor), for the ocean to pass its wave slopes (caustics). */
	UMaterialInstanceDynamic* GetTerrainMaterialInstance() const { return TerrainMaterialInstance; }

	/** Terrain height above sea level (m) from the analytic island shape: ground plus the understory's relief. */
	float GetTerrainHeight(const FVector2f& PositionM) const;

	/** Share of the understory mass that is terrain (UnderstoryRelief). */
	float GetUnderstoryRelief() const { return UnderstoryRelief; }

	/** Ground height (without understory) under a world location, cm, for scripts placing cameras or props. */
	UFUNCTION(BlueprintPure, Category = "Island")
	float GetGroundHeightAt(const FVector& WorldLocation) const
	{
		return SampleTerrain(FVector2f(float(WorldLocation.X * 0.01), float(WorldLocation.Y * 0.01))).Ground * 100.f;
	}

	/** Ground, understory, rockiness and distance inland, for placing vegetation and rocks. */
	FOceanTerrainSample SampleTerrain(const FVector2f& PositionM) const;

	virtual void OnConstruction(const FTransform& Transform) override;

protected:
	virtual void BeginPlay() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Radius of the island at the waterline, along its long axis. */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "10", Units = "Meters"))
	float IslandRadius = 850.f;

	/** Long axis over short axis: islands are rarely round (a ridge, a chain of hills). */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "1", ClampMax = "4"))
	float IslandAspect = 1.6f;

	/** Direction of the long axis, degrees from +X. */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (Units = "Degrees"))
	float IslandAngle = 20.f;

	/**
	 * Share of the fractal coastline detail kept on the sandy coasts: beaches are smooth arcs between the headlands
	 * (the swash files them straight), only the rocky shores are jagged. Full detail everywhere left sand bars and
	 * pools all along the beaches.
	 */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "0", ClampMax = "1"))
	float SandyCoastDetail = 0.12f;

	/** Distance from the back of the beaches over which the massif rises to most of its height. */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "10", Units = "Meters"))
	float MassifWidth = 230.f;

	/** Height of the limestone cliffs of the rocky points (varies 0.6-1.4x along the coast). */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "0", Units = "Meters"))
	float CliffHeight = 13.f;

	/** How far the coastline wanders in and out of the circle (large bays and headlands). */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "0", Units = "Meters"))
	float CoastlineVariation = 170.f;

	/** Amplitude of the fractal detail of the coastline (coves, points), from 120 m wavelength down. */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "0", Units = "Meters"))
	float CoastlineDetail = 70.f;

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
	float HillHeight = 85.f;

	/** 0 = sandy coast all around, 1 = mostly rocky points. */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "0", ClampMax = "1"))
	float Rockiness = 0.5f;

	/**
	 * Share of the forest's understory mass (1.5-3 m) built into the terrain mesh. The rest is real shrubs
	 * (AOceanIslandFlora): a full mass reads as smooth green mounds between them.
	 */
	UPROPERTY(EditAnywhere, Category = "Island", meta = (ClampMin = "0", ClampMax = "1"))
	float UnderstoryRelief = 0.1f;

	/** Carves the land with rain and gravity (OceanErosion) when the island is built. */
	UPROPERTY(EditAnywhere, Category = "Island|Erosion")
	bool bErode = true;

	/** Raindrops simulated over the island (~1 per square metre of land gives a well-drained relief). */
	UPROPERTY(EditAnywhere, Category = "Island|Erosion", meta = (EditCondition = "bErode", ClampMin = "0"))
	int32 ErosionDroplets = 2500000;

	/** Scales how much each droplet carries: deeper gullies, larger fans. */
	UPROPERTY(EditAnywhere, Category = "Island|Erosion", meta = (EditCondition = "bErode", ClampMin = "0"))
	float ErosionStrength = 1.f;

	/** Iterations of uplift and incision growing the massif's relief (more = closer to the steady state). */
	UPROPERTY(EditAnywhere, Category = "Island|Erosion", meta = (EditCondition = "bErode", ClampMin = "0"))
	int32 LandscapeIterations = 250;

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
	float GridSize = 4096.f;

	/** Samples per side (power of two keeps the distance transform exact). */
	UPROPERTY(EditAnywhere, Category = "Grid", meta = (ClampMin = "64", ClampMax = "2048"))
	int32 GridResolution = 2048;

	UPROPERTY(EditAnywhere, Category = "Rendering")
	TSoftObjectPtr<UMaterialInterface> TerrainMaterial;

	/** Trails of footprints simulated in the sand tile (OceanSimulateSandFootprints); 0 = untouched beach. */
	UPROPERTY(EditAnywhere, Category = "Sand", meta = (ClampMin = "0", ClampMax = "40"))
	int32 FootprintTrails = 14;

	/** Width of the periodic footprint tile laid over the beach. */
	UPROPERTY(EditAnywhere, Category = "Sand", meta = (ClampMin = "2", Units = "Meters"))
	float FootprintTileSize = 8.f;

	UPROPERTY(VisibleAnywhere, Category = "Rendering")
	TObjectPtr<UOceanMeshComponent> Terrain;

	/** Sea spray: vertical strips along the line where the swell breaks, drawn by M_OceanSpray (OceanSpray). */
	UPROPERTY(VisibleAnywhere, Category = "Rendering")
	TObjectPtr<UOceanMeshComponent> Spray;

	UPROPERTY(EditAnywhere, Category = "Rendering")
	TSoftObjectPtr<UMaterialInterface> SprayMaterial;

	/**
	 * Depth where the swell breaks (shoaled height ~0.78 x depth: ~1.2 m for the 0.6 m trade-wind swell of
	 * OceanShoreWave): the spray curtain follows this isobath.
	 */
	UPROPERTY(EditAnywhere, Category = "Spray", meta = (ClampMin = "0.2", Units = "Meters"))
	float SprayBreakDepth = 1.2f;

	/** Height of the spray curtain (the splashes on the rocky points reach a few metres). */
	UPROPERTY(EditAnywhere, Category = "Spray", meta = (ClampMin = "1", Units = "Meters"))
	float SprayCurtainHeight = 6.f;

private:
	void Build();
	void BuildShoreTexture(const FOceanShoreData& Data);
	void BuildFootprintTexture();

	/**
	 * The island's shape before erosion, analytic (coastline, beach, hills, rocky points, sea floor). Without fine detail
	 * (the limestone's ledges and pits, finer than the erosion grids) for the erosion's own input.
	 */
	FOceanTerrainSample SampleShape(const FVector2f& PositionM, bool bFineDetail = true) const;

	/** Where the coast is rocky (0..Rockiness), from the seeded position. */
	float RockMask(const FVector2f& Seeded) const;

	/** Erodes the analytic shape over the island (OceanErosion) and keeps the change in Erosion. */
	void BuildErosion();

	/**
	 * The erosion takes a minute on a large island and every load (and Play In Editor) rebuilds the terrain: its result
	 * is cached in Saved/OceanCache, keyed by every setting that shapes it. Returns true when loaded.
	 */
	bool LoadErosionCache(const FString& Path);
	void SaveErosionCache(const FString& Path) const;
	FString ErosionCachePath() const;

	/** Builds the land and coast meshes: chunks at 0.5 m (coast), 1 m (land), grouped into tiles that cull separately. */
	void BuildLandTiles(const TArray<float>& Heights, int32 N, float Texel, int32 Stride);

	/** Land and coast meshes, one component per tile (created on build, not saved). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UOceanMeshComponent>> LandTiles;

	/**
	 * The massif grown by landscape evolution (OceanErosion::LandscapeEvolution, 2 m), scaled to the analytic massif's
	 * height, and its drainage network (Flow). Replaces the analytic hills in SampleShape once built.
	 */
	FOceanHeightfield Relief;

	/**
	 * What the rain and the screes then changed (m, + deposited, - carved), at 1 m. SampleTerrain adds the change to
	 * the shape, which keeps its fine detail (and the beaches, which barely erode, stay exactly analytic).
	 */
	FOceanHeightfield Erosion;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> ShoreTexture;

	/** RG16F: height offset (m), disturbance; tiled by the terrain material (SandFootprints). */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> FootprintTexture;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> TerrainMaterialInstance;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> SprayMaterialInstance;

	/** The spray curtain: marching squares on the shore data's depth at SprayBreakDepth, a few strips deep. */
	void BuildSprayCurtain(const FOceanShoreData& Data);

	/** Settings the footprint texture was built with, to rebuild it only when they change. */
	int32 BuiltFootprintTrails = -1;
	float BuiltFootprintTileSize = 0.f;

	TSharedPtr<const FOceanShoreData> ShoreData;
	bool bBuilt = false;
};
