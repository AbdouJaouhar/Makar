#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Ship/ShipDesign.h"
#include "ProceduralShip.generated.h"

class UStaticMesh;
class UStaticMeshComponent;

/**
 * A period ship generated from its type and a seed (Source/Makar/Private/Ship): hull planked plank by plank, decks,
 * masts and rigging, sails, guns and fittings, as one static mesh with five materials (/Game/Ship: wood, sail, rope,
 * metal, glass; Scripts/create_ship_materials.py). Built when placed or changed in the editor and on BeginPlay.
 * bOnCradle stands it on keel blocks and shores with the ground at the actor's origin; otherwise the actor's origin
 * is on the waterline, midships.
 */
UCLASS()
class MAKAR_API AProceduralShip : public AActor
{
	GENERATED_BODY()

public:
	AProceduralShip();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship")
	EShipType Type = EShipType::Sloop;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship")
	int32 Seed = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship")
	bool bSailsSet = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship")
	bool bOnCradle = true;

	/** Height of the keel blocks under the keel (m). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship", meta = (EditCondition = "bOnCradle", ClampMin = "0.3"))
	float CradleHeight = 1.1f;

	/** The design drawn from Type and Seed (read only). */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Ship")
	FShipDesign Design;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Ship")
	int32 Triangles = 0;

	/** Generates the ship again. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Ship")
	void Rebuild();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Ship")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> Generated;

	uint32 BuiltKey = 0;
	uint32 Key() const;
};
