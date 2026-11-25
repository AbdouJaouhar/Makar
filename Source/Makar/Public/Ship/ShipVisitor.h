#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "ShipVisitor.generated.h"

class UCameraComponent;
class UPointLightComponent;

/**
 * Someone aboard, in first person: walks the decks, climbs the ladders, goes below with a lantern in hand. WASD or
 * arrows to walk, the mouse to look, Shift to hurry, Space to jump; Escape back to the orbiting camera.
 */
UCLASS()
class MAKAR_API AShipVisitor : public ACharacter
{
	GENERATED_BODY()

public:
	AShipVisitor();

	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Visitor")
	TObjectPtr<UCameraComponent> Eyes;

	/** A ship's lantern carried at the hip: below decks is dark. */
	UPROPERTY(VisibleAnywhere, Category = "Visitor")
	TObjectPtr<UPointLightComponent> Lantern;
};
