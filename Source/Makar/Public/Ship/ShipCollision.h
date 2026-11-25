#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "ShipCollision.generated.h"

class UBodySetup;

/**
 * The ship's collision for walking aboard: thousands of oriented boxes (decks, sides, bulkheads, ladders, masts) as
 * physics primitives of one body. Boxes need no cooking: set in a few milliseconds, unlike convex hulls. Invisible.
 */
UCLASS()
class MAKAR_API UShipCollisionComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UShipCollisionComponent();

	/** Boxes in component space (cm): centre, axes, half sizes. */
	struct FBox3
	{
		FVector Center;
		FRotator Rotation;
		FVector Half;
	};
	void SetBoxes(const TArray<FBox3>& Boxes);

	virtual UBodySetup* GetBodySetup() override { return Body; }
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override { return nullptr; }

private:
	UPROPERTY(Transient)
	TObjectPtr<UBodySetup> Body;

	FBox LocalBounds = FBox(ForceInit);
};
