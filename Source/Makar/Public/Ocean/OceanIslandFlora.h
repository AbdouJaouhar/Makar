#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OceanIslandFlora.generated.h"

class AOceanBathymetry;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;

/**
 * Dresses the level's AOceanBathymetry island with Caribbean flora and coastal rocks (see OceanFlora):
 * coconut palms on the back of the beaches leaning seawards, sea grape shrubs along the forest edge,
 * broadleaf trees inland, and limestone boulders on the rocky points and at the waterline.
 * Meshes and instances are generated on load (nothing is saved), deterministically from Seed.
 */
UCLASS()
class MAKAR_API AOceanIslandFlora : public AActor
{
	GENERATED_BODY()

public:
	AOceanIslandFlora();

	virtual void OnConstruction(const FTransform& Transform) override;

protected:
	virtual void BeginPlay() override;

	UPROPERTY(EditAnywhere, Category = "Flora")
	int32 Seed = 11;

	UPROPERTY(EditAnywhere, Category = "Flora", meta = (ClampMin = "0"))
	float PalmDensity = 1.f;

	UPROPERTY(EditAnywhere, Category = "Flora", meta = (ClampMin = "0"))
	float TreeDensity = 1.f;

	UPROPERTY(EditAnywhere, Category = "Flora", meta = (ClampMin = "0"))
	float ShrubDensity = 1.f;

	UPROPERTY(EditAnywhere, Category = "Flora", meta = (ClampMin = "0"))
	float RockDensity = 1.f;

	UPROPERTY(EditAnywhere, Category = "Rendering")
	TSoftObjectPtr<UMaterialInterface> BarkMaterial;

	UPROPERTY(EditAnywhere, Category = "Rendering")
	TSoftObjectPtr<UMaterialInterface> FoliageMaterial;

	UPROPERTY(EditAnywhere, Category = "Rendering")
	TSoftObjectPtr<UMaterialInterface> RockMaterial;

private:
	void Build();
	UHierarchicalInstancedStaticMeshComponent* AddInstancer(UStaticMesh* Mesh, float CullDistanceM);

	UPROPERTY(Transient)
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> Instancers;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> Meshes;

	bool bBuilt = false;
};
