#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "ShipViewer.generated.h"

class AProceduralShip;
class AShipVisitor;
class UCameraComponent;
class SShipMenu;

/**
 * A camera orbiting the ship: drag with the left or right mouse button to turn around it, the wheel to come closer or
 * step back; left alone, it turns slowly by itself.
 */
UCLASS()
class MAKAR_API AShipViewerPawn : public APawn
{
	GENERATED_BODY()

public:
	AShipViewerPawn();

	virtual void Tick(float DeltaSeconds) override;

	/** Frames the ship: the orbit's centre and distance from its bounds. */
	void Frame(const FBox& Bounds);

	UPROPERTY(EditAnywhere, Category = "Viewer")
	float AutoRotateSpeed = 6.f; // degrees per second

private:
	UPROPERTY(VisibleAnywhere, Category = "Viewer")
	TObjectPtr<UCameraComponent> Camera;

	FVector Target = FVector::ZeroVector;
	float Yaw = -35.f;
	float Pitch = -12.f;
	float Distance = 6000.f;
	float TargetDistance = 6000.f;
	float MinDistance = 1500.f;
	float MaxDistance = 20000.f;
	float IdleTime = 10.f;
};

/** Shows the cursor and the options menu (SShipMenu), drives the ship's generation from it. */
UCLASS()
class MAKAR_API AShipViewerController : public APlayerController
{
	GENERATED_BODY()

public:
	AShipViewerController();

	/** Generates the ship again from the menu's design (on a worker thread) and frames it; OnDone once it shows. */
	void ApplyDesign(const struct FShipDesign& Design, TFunction<void()> OnDone = nullptr);

	AProceduralShip* GetShip() const { return Ship; }

	/** Goes aboard in first person (Tab or Escape to come back to the orbiting camera). */
	UFUNCTION(BlueprintCallable, Category = "Viewer")
	void StartVisit();
	UFUNCTION(BlueprintCallable, Category = "Viewer")
	void EndVisit();
	bool IsVisiting() const { return Visitor != nullptr; }

	virtual void PlayerTick(float DeltaTime) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<AProceduralShip> Ship;

	UPROPERTY(Transient)
	TObjectPtr<APawn> Orbit;

	UPROPERTY(Transient)
	TObjectPtr<AShipVisitor> Visitor;

	TSharedPtr<SShipMenu> Menu;
};

/** The ship viewer: an orbiting camera around one generated ship and its options menu (/Game/Maps/Shipyard). */
UCLASS()
class MAKAR_API AShipViewerGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AShipViewerGameMode();
};
