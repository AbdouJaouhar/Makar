#include "MakarFreeCamera.h"

#include "Components/InputComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/PlayerInput.h"

AMakarFreeCamera::AMakarFreeCamera()
{
	PrimaryActorTick.bCanEverTick = true;
}

void AMakarFreeCamera::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent); // ADefaultPawn's flight (WASD, mouse, E/Q)

	static bool bMappingsAdded = false;
	if (!bMappingsAdded)
	{
		bMappingsAdded = true;
		UPlayerInput::AddEngineDefinedActionMapping(FInputActionKeyMapping("MakarCamera_SpeedUp", EKeys::MouseScrollUp));
		UPlayerInput::AddEngineDefinedActionMapping(FInputActionKeyMapping("MakarCamera_SlowDown", EKeys::MouseScrollDown));
		UPlayerInput::AddEngineDefinedActionMapping(FInputActionKeyMapping("MakarCamera_Fast", EKeys::LeftShift));
		UPlayerInput::AddEngineDefinedActionMapping(FInputActionKeyMapping("MakarCamera_Slow", EKeys::LeftControl));
	}
	PlayerInputComponent->BindAction("MakarCamera_SpeedUp", IE_Pressed, this, &AMakarFreeCamera::SpeedUp);
	PlayerInputComponent->BindAction("MakarCamera_SlowDown", IE_Pressed, this, &AMakarFreeCamera::SlowDown);
	PlayerInputComponent->BindAction("MakarCamera_Fast", IE_Pressed, this, &AMakarFreeCamera::FastOn);
	PlayerInputComponent->BindAction("MakarCamera_Fast", IE_Released, this, &AMakarFreeCamera::FastOff);
	PlayerInputComponent->BindAction("MakarCamera_Slow", IE_Pressed, this, &AMakarFreeCamera::SlowOn);
	PlayerInputComponent->BindAction("MakarCamera_Slow", IE_Released, this, &AMakarFreeCamera::SlowOff);
}

void AMakarFreeCamera::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (UFloatingPawnMovement* Movement = Cast<UFloatingPawnMovement>(GetMovementComponent()))
	{
		const float Speed = SpeedMs * 100.f * (bFast ? 4.f : 1.f) * (bSlow ? 0.25f : 1.f);
		Movement->MaxSpeed = Speed;
		// Reaches its speed in ~0.3 s and stops as quickly, whatever the speed.
		Movement->Acceleration = Speed * 3.f;
		Movement->Deceleration = Speed * 4.f;
	}
}

AMakarViewerGameMode::AMakarViewerGameMode()
{
	DefaultPawnClass = AMakarFreeCamera::StaticClass();
}
