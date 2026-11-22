#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Ocean/OceanFoamMap.h"
#include "Ocean/OceanSettings.h"
#include "Ocean/OceanShoreData.h"
#include "Ocean/OceanSimulation.h"
#include "Tasks/Task.h"
#include "OceanActor.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UProceduralMeshComponent;
class UTexture2D;

/**
 * Infinite FFT ocean. Place one in the level; its Z is sea level.
 *
 * The simulation runs on worker threads, then the same data is
 *  - uploaded to textures that the ocean material uses to displace and shade the surface,
 *  - kept on the CPU so GetWaterHeightAt() returns exactly the surface that is drawn
 *    (used by buoyancy, swimming, cameras...).
 */
UCLASS()
class MAKAR_API AOceanActor : public AActor
{
	GENERATED_BODY()

public:
	AOceanActor();

	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return bPreviewInEditor; }
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void BeginDestroy() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** World Z (cm) of the water surface above/below this location. */
	UFUNCTION(BlueprintPure, Category = "Ocean")
	float GetWaterHeightAt(const FVector& WorldLocation) const;

	UFUNCTION(BlueprintPure, Category = "Ocean")
	float GetSeaLevel() const { return float(GetActorLocation().Z); }

	/** Change the sea state at runtime (storm, calm...). */
	UFUNCTION(BlueprintCallable, Category = "Ocean")
	void SetSettings(const FOceanSettings& NewSettings);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ocean", meta = (ShowOnlyInnerProperties))
	FOceanSettings Settings;

	/** Created by Scripts/create_ocean_material.py. */
	UPROPERTY(EditAnywhere, Category = "Ocean|Rendering")
	TSoftObjectPtr<UMaterialInterface> OceanMaterial;

	/** Cascades used for geometry and CPU queries; finer ones only shade (normals, foam). */
	UPROPERTY(EditAnywhere, Category = "Ocean|Rendering", meta = (ClampMin = "1", ClampMax = "3"))
	int32 NumGeometryCascades = 2;

	/** Vertex spacing around the camera, metres. */
	UPROPERTY(EditAnywhere, Category = "Ocean|Rendering", meta = (ClampMin = "0.1"))
	float GridSpacing = 0.5f;

	/** Quads from the centre to the edge of the uniform inner area. */
	UPROPERTY(EditAnywhere, Category = "Ocean|Rendering", meta = (ClampMin = "8"))
	int32 GridInnerQuads = 128;

	/** Quads in the outer area, whose spacing grows geometrically up to the horizon. */
	UPROPERTY(EditAnywhere, Category = "Ocean|Rendering", meta = (ClampMin = "8"))
	int32 GridOuterQuads = 64;

	/** Distance from the centre to the edge of the ocean mesh, metres. */
	UPROPERTY(EditAnywhere, Category = "Ocean|Rendering", meta = (ClampMin = "500"))
	float GridFarDistance = 20000.f;

	/** Waves flatten between these distances (metres) so the horizon stays clean. */
	UPROPERTY(EditAnywhere, Category = "Ocean|Rendering")
	float DisplacementFadeStart = 3000.f;

	UPROPERTY(EditAnywhere, Category = "Ocean|Rendering")
	float DisplacementFadeEnd = 8000.f;

	UPROPERTY(EditAnywhere, Category = "Ocean|Rendering")
	bool bPreviewInEditor = true;

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, Category = "Ocean")
	TObjectPtr<UProceduralMeshComponent> OceanMesh;

private:
	void InitializeSimulation();
	void BuildMesh();
	void UpdateMaterialParameters();
	void UploadFrame(const FOceanFrame& Frame);
	void FollowCamera();
	void WaitForSimulation();
	FVector3f SampleDisplacement(const FVector2f& PositionMeters) const;

	TUniquePtr<FOceanSimulation> Simulation;
	TUniquePtr<FOceanFrame> FrontFrame; // published: rendered and queried
	TUniquePtr<FOceanFrame> BackFrame;  // being written by the worker task
	TUniquePtr<FOceanFoamMap> FoamMap;  // advanced by the worker task, after the waves

	/** Coastal data from the level's AOceanBathymetry, if any: calms the waves in shallow water. */
	TSharedPtr<const FOceanShoreData> ShoreData;

	/** The level's island, whose sea floor material receives the wave slopes for its caustics. */
	TWeakObjectPtr<class AOceanBathymetry> Bathymetry;
	UE::Tasks::FTask PendingTask;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> DisplacementTextures;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> DerivativeTextures;

	/** Slope second moments, see FOceanCascadeFrame::MomentMips. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> MomentTextures;

	/** World-space foam around the camera, see FOceanFoamMap. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> FoamMapTexture;

	/** Owned by the AOceanBathymetry; see FOceanShoreData. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> ShoreTexture;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> MaterialInstance;

	float SimulationTime = 0.f;

	/** Running cost of the simulation, logged every few seconds. */
	float CostWorkerMs = 0.f;
	float CostUploadMs = 0.f;
	int32 CostFrames = 0;
	float CostReportTime = 0.f;
	float LastLaunchTime = 0.f;
	float GridGrowth = 0.f;
	bool bNeedsInitialization = true;
	bool bMeshBuilt = false;
};
