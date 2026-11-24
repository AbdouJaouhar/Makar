#include "Ship/ShipViewer.h"

#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "ShipMenu.h"
#include "Ship/ProceduralShip.h"

AShipViewerPawn::AShipViewerPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(RootComponent);
	Camera->SetFieldOfView(50.f);
}

void AShipViewerPawn::Frame(const FBox& Bounds)
{
	if (!Bounds.IsValid)
	{
		return;
	}
	Target = Bounds.GetCenter();
	const float Radius = Bounds.GetExtent().Size();
	MinDistance = Radius * 0.35f;
	MaxDistance = Radius * 5.f;
	TargetDistance = FMath::Clamp(Radius * 2.6f, MinDistance, MaxDistance);
	if (Distance <= 0.f || Distance > MaxDistance)
	{
		Distance = TargetDistance;
	}
}

void AShipViewerPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (PC)
	{
		const bool bDrag = PC->IsInputKeyDown(EKeys::LeftMouseButton) || PC->IsInputKeyDown(EKeys::RightMouseButton);
		float DX = 0.f, DY = 0.f;
		PC->GetInputMouseDelta(DX, DY);
		if (bDrag && (DX != 0.f || DY != 0.f))
		{
			Yaw += DX * 2.5f;
			Pitch = FMath::Clamp(Pitch + DY * 2.5f, -80.f, 35.f);
			IdleTime = 0.f;
		}
		if (PC->WasInputKeyJustPressed(EKeys::MouseScrollUp))
		{
			TargetDistance = FMath::Max(TargetDistance * 0.88f, MinDistance);
			IdleTime = 0.f;
		}
		if (PC->WasInputKeyJustPressed(EKeys::MouseScrollDown))
		{
			TargetDistance = FMath::Min(TargetDistance / 0.88f, MaxDistance);
			IdleTime = 0.f;
		}
	}
	IdleTime += DeltaSeconds;
	if (IdleTime > 4.f)
	{
		Yaw += AutoRotateSpeed * DeltaSeconds * FMath::Min((IdleTime - 4.f) * 0.5f, 1.f);
	}
	Distance = FMath::FInterpTo(Distance, TargetDistance, DeltaSeconds, 6.f);
	const FRotator Rotation(Pitch, Yaw, 0.f);
	// The ship a little right of centre: the menu covers the left of the screen.
	const FVector Right = FRotationMatrix(Rotation).GetUnitAxis(EAxis::Y);
	const FVector Location = Target - Rotation.Vector() * Distance - Right * (Distance * 0.12f);
	SetActorLocationAndRotation(Location, Rotation);
	if (PC)
	{
		PC->SetControlRotation(Rotation);
	}
}

AShipViewerController::AShipViewerController()
{
	bShowMouseCursor = true;
	bEnableClickEvents = false;
}

void AShipViewerController::BeginPlay()
{
	Super::BeginPlay();
	if (!IsLocalController())
	{
		return;
	}
	for (TActorIterator<AProceduralShip> It(GetWorld()); It; ++It)
	{
		Ship = *It;
		break;
	}
	if (!Ship)
	{
		Ship = GetWorld()->SpawnActor<AProceduralShip>(FVector::ZeroVector, FRotator::ZeroRotator);
	}
	// Afloat in the studio: no berth; built now (the ship's own BeginPlay may not have run yet).
	Ship->bOnCradle = false;
	Ship->Rebuild();
	if (AShipViewerPawn* Viewer = Cast<AShipViewerPawn>(GetPawn()))
	{
		Viewer->Frame(Ship->GetShipBounds());
	}
	Menu = SNew(SShipMenu).Controller(this).Design(Ship->Design);
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->AddViewportWidgetContent(Menu.ToSharedRef(), 10);
	}
	FInputModeGameAndUI Mode;
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	Mode.SetHideCursorDuringCapture(true);
	SetInputMode(Mode);
}

void AShipViewerController::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Menu.IsValid() && GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(Menu.ToSharedRef());
	}
	Menu.Reset();
	Super::EndPlay(Reason);
}

void AShipViewerController::ApplyDesign(const FShipDesign& Design)
{
	if (!Ship)
	{
		return;
	}
	Ship->BuildFromDesign(Design);
	if (AShipViewerPawn* Viewer = Cast<AShipViewerPawn>(GetPawn()))
	{
		Viewer->Frame(Ship->GetShipBounds());
	}
}

AShipViewerGameMode::AShipViewerGameMode()
{
	DefaultPawnClass = AShipViewerPawn::StaticClass();
	PlayerControllerClass = AShipViewerController::StaticClass();
}
