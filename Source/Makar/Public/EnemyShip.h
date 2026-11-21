#pragma once

#include "CoreMinimal.h"
#include "PirateShip.h"
#include "EnemyShip.generated.h"

/**
 * AI ship: chases the player, then turns to keep them abeam and fires broadsides.
 * Drop it in the level, on the water.
 */
UCLASS()
class MAKAR_API AEnemyShip : public APirateShip
{
	GENERATED_BODY()

public:
	virtual void Tick(float DeltaSeconds) override;

	/** Inside this distance the ship stops closing in and presents its side. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|AI")
	float BroadsideRange = 4000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|AI")
	float FireRange = 6000.f;

	/** Beyond this distance the ship ignores the player. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|AI")
	float DetectionRange = 30000.f;
};
