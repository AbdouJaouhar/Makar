#include "Ship/ShipVisitor.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"

AShipVisitor::AShipVisitor()
{
	PrimaryActorTick.bCanEverTick = true;
	// An adult of the time: 1.70 m, shoulders 0.5 m.
	GetCapsuleComponent()->InitCapsuleSize(25.f, 85.f);
	Eyes = CreateDefaultSubobject<UCameraComponent>(TEXT("Eyes"));
	Eyes->SetupAttachment(GetCapsuleComponent());
	Eyes->SetRelativeLocation(FVector(0.f, 0.f, 72.f));
	Eyes->bUsePawnControlRotation = true;
	Eyes->SetFieldOfView(80.f);
	Lantern = CreateDefaultSubobject<UPointLightComponent>(TEXT("Lantern"));
	Lantern->SetupAttachment(GetCapsuleComponent());
	Lantern->SetRelativeLocation(FVector(25.f, 18.f, 10.f));
	Lantern->SetIntensityUnits(ELightUnits::Lumens);
	Lantern->SetIntensity(120.f); // a candle lantern
	Lantern->SetAttenuationRadius(900.f);
	Lantern->SetLightColor(FLinearColor(1.f, 0.72f, 0.42f));
	Lantern->SetSourceRadius(4.f);
	Lantern->SetCastShadows(true);
	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->MaxWalkSpeed = 160.f;
	Move->MaxStepHeight = 40.f;
	Move->SetWalkableFloorAngle(62.f); // ship's ladders are steep
	Move->JumpZVelocity = 330.f;
	Move->AirControl = 0.2f;
	Move->bCanWalkOffLedges = true;
	bUseControllerRotationYaw = true;
}

void AShipVisitor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		return;
	}
	float DX = 0.f, DY = 0.f;
	PC->GetInputMouseDelta(DX, DY);
	AddControllerYawInput(DX * 0.6f);
	AddControllerPitchInput(-DY * 0.6f);
	auto Down = [PC](const FKey& A, const FKey& B) { return PC->IsInputKeyDown(A) || PC->IsInputKeyDown(B); };
	const float Forward = (Down(EKeys::W, EKeys::Up) || PC->IsInputKeyDown(EKeys::Z) ? 1.f : 0.f) - (Down(EKeys::S, EKeys::Down) ? 1.f : 0.f);
	const float Right = (Down(EKeys::D, EKeys::Right) ? 1.f : 0.f) - (Down(EKeys::A, EKeys::Left) || PC->IsInputKeyDown(EKeys::Q) ? 1.f : 0.f);
	const FRotator Yaw(0.f, PC->GetControlRotation().Yaw, 0.f);
	AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::X), Forward);
	AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y), Right);
	GetCharacterMovement()->MaxWalkSpeed = PC->IsInputKeyDown(EKeys::LeftShift) ? 380.f : 160.f;
	if (PC->WasInputKeyJustPressed(EKeys::SpaceBar))
	{
		Jump();
	}
}
