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

	/** Where a visitor comes aboard (ship space, cm): on the weather deck by the open hatch. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Ship")
	FVector Boarding = FVector::ZeroVector;
	int32 CollisionBoxes = 0;

	/** Milliseconds the last generation took (geometry, then mesh update), for the menu. */
	float LastBuildMs = 0.f;

	/** The true wind: the direction it comes from (degrees from the bow, clockwise: 90 from starboard, 180 from astern)
	 *  and its speed (m/s). The sails are cloth it fills. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind", meta = (ClampMin = "0", ClampMax = "360"))
	float WindFrom = 210.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind", meta = (ClampMin = "0", ClampMax = "25"))
	float WindSpeed = 7.f;

	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

	/** The generated ship's bounds (world space). */
	FBox GetShipBounds() const;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;

private:
	/** Created when first built, transient: the generated geometry (hundreds of MB) is never saved with the level. */
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> Mesh;
	UProceduralMeshComponent* GetOrCreateMesh();

	UPROPERTY(Transient)
	TObjectPtr<class UShipCollisionComponent> Collision;

	bool bBuilt = false;
	TSharedPtr<class FSailAnimation> Sails;
	float SimulationTime = 0.f;
	/** The wind's strength for the sails' movement (0 to 1). */
	float SailStrength() const { return FMath::Clamp(0.35f + WindSpeed / 12.f, 0.35f, 1.f); }
	/** Where the wind goes, in ship space (the ship does not move yet: true wind = apparent wind). */
	FVector3f WindTowards() const
	{
		const float From = FMath::DegreesToRadians(WindFrom);
		return -FVector3f(FMath::Cos(From), FMath::Sin(From), 0.f);
	}
	void UpdateSails();
	int32 Ticket = 0;
	void ApplyResult(struct FShipBuildResult& Result);

	uint32 BuiltKey = 0;
	/** A build on a worker thread not yet shown; when the rig was last trimmed to a new wind (s). */
	bool bBuildPending = false;
	double LastTrimTime = -1e6;
	/** The design as given, with the ship's wind for the trim. */
	FShipDesign Trimmed(const FShipDesign& InDesign) const;
	uint32 Key() const;
};
