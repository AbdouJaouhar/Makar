#pragma once

#include "CoreMinimal.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/GameModeBase.h"
#include "MakarFreeCamera.generated.h"

/**
 * Free-flying camera of the ocean map (no ship, no player yet): ADefaultPawn's WASD / mouse flight at an adjustable speed.
 * Mouse wheel: base speed up or down (x1.25 a notch); Shift: four times faster; Ctrl: four times slower (looking closely
 * at the sand or the leaves).
 */
UCLASS()
class MAKAR_API AMakarFreeCamera : public ADefaultPawn
{
	GENERATED_BODY()

public:
	AMakarFreeCamera();

	/** Base speed, m/s (ADefaultPawn: 12). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.5", ClampMax = "500"))
	float SpeedMs = 15.f;

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

private:
	void SpeedUp() { SpeedMs = FMath::Min(SpeedMs * 1.25f, 500.f); }
	void SlowDown() { SpeedMs = FMath::Max(SpeedMs / 1.25f, 0.5f); }
	void FastOn() { bFast = true; }
	void FastOff() { bFast = false; }
	void SlowOn() { bSlow = true; }
	void SlowOff() { bSlow = false; }

	bool bFast = false;
	bool bSlow = false;
};

/** Game mode of the ocean map: the free camera above. */
UCLASS()
class MAKAR_API AMakarViewerGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AMakarViewerGameMode();
};
