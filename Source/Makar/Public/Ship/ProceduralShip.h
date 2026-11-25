#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Ship/ShipDesign.h"
#include "ProceduralShip.generated.h"

class UProceduralMeshComponent;

/**
 * A period ship generated from its type and a seed (Source/Makar/Private/Ship): hull planked plank by plank, decks,
 * masts and rigging, sails, guns and fittings, as one procedural mesh with five sections (/Game/Ship: wood, sail, rope,
 * metal, glass; Scripts/create_ship_materials.py). Built when placed or changed in the editor and on BeginPlay;
 * the viewer's menu builds it on a worker thread (BuildFromDesignAsync): the ship in view stays until the new one is ready.
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

	/** Generates the ship again from Type and Seed. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Ship")
	void Rebuild();

	/** Generates the ship from a design edited by hand (the viewer's menu); Type and Seed follow it. */
	void BuildFromDesign(const FShipDesign& InDesign);

	/** The same, generated on a worker thread; OnDone runs on the game thread once the new ship shows (a newer request
	 *  supersedes an older one, whose OnDone is not called). */
	void BuildFromDesignAsync(const FShipDesign& InDesign, TFunction<void()> OnDone);

	/** Milliseconds the last generation took (geometry, then mesh update), for the menu. */
	float LastBuildMs = 0.f;

	/** The generated ship's bounds (world space). */
	FBox GetShipBounds() const;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;

private:
	/** Created when first built, transient: the generated geometry (hundreds of MB) is never saved with the level. */
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> Mesh;
	UProceduralMeshComponent* GetOrCreateMesh();

	bool bBuilt = false;
	int32 Ticket = 0;
	void ApplyResult(struct FShipBuildResult& Result);

	uint32 BuiltKey = 0;
	uint32 Key() const;
};
