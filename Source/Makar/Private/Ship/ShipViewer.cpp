#include "Ship/ShipViewer.h"

#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "ShipMenu.h"
#include "Ship/ProceduralShip.h"
#include "Ship/ShipVisitor.h"

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
	FramedCenter = Target;
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
		const FRotator View(Pitch, Yaw, 0.f);
		const FVector ViewRight = FRotationMatrix(View).GetUnitAxis(EAxis::Y);
		const FVector ViewUp = FRotationMatrix(View).GetUnitAxis(EAxis::Z);
		const bool bPan = PC->IsInputKeyDown(EKeys::MiddleMouseButton)
			|| (bDrag && (PC->IsInputKeyDown(EKeys::LeftShift) || PC->IsInputKeyDown(EKeys::RightShift)));
		if (bPan && (DX != 0.f || DY != 0.f))
		{
			// The ship follows the mouse: as far as the cursor moves on screen, at the ship's distance.
			const float Scale = Distance * 0.0045f;
			Target -= ViewRight * (DX * Scale) + ViewUp * (DY * Scale);
			IdleTime = 0.f;
		}
		else if (bDrag && (DX != 0.f || DY != 0.f))
		{
			Yaw += DX * 2.5f;
			Pitch = FMath::Clamp(Pitch + DY * 2.5f, -80.f, 35.f);
			IdleTime = 0.f;
		}
		// Keys: horizontal along the view, E / C vertical; Shift for faster.
		auto Down = [PC](std::initializer_list<FKey> Keys) { for (const FKey& K : Keys) { if (PC->IsInputKeyDown(K)) { return true; } } return false; };
		const float Forward = (Down({ EKeys::W, EKeys::Z, EKeys::Up }) ? 1.f : 0.f) - (Down({ EKeys::S, EKeys::Down }) ? 1.f : 0.f);
		const float Side = (Down({ EKeys::D, EKeys::Right }) ? 1.f : 0.f) - (Down({ EKeys::A, EKeys::Q, EKeys::Left }) ? 1.f : 0.f);
		const float Lift = (Down({ EKeys::E, EKeys::PageUp }) ? 1.f : 0.f) - (Down({ EKeys::C, EKeys::PageDown }) ? 1.f : 0.f);
		if (Forward != 0.f || Side != 0.f || Lift != 0.f)
		{
			const FVector Flat = FRotator(0.f, Yaw, 0.f).Vector();
			const FVector FlatRight = FRotator(0.f, Yaw + 90.f, 0.f).Vector();
			const float Speed = Distance * 0.5f * (Down({ EKeys::LeftShift, EKeys::RightShift }) ? 3.f : 1.f);
			Target += (Flat * Forward + FlatRight * Side + FVector::UpVector * Lift) * (Speed * DeltaSeconds);
			IdleTime = 0.f;
		}
		if (PC->WasInputKeyJustPressed(EKeys::R))
		{
			bAutoRotate = !bAutoRotate;
		}
		if (PC->WasInputKeyJustPressed(EKeys::F))
		{
			Recenter();
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
	if (bAutoRotate && IdleTime > 4.f)
	{
		Yaw += AutoRotateSpeed * DeltaSeconds * FMath::Min((IdleTime - 4.f) * 0.5f, 1.f);
	}
	Distance = FMath::FInterpTo(Distance, TargetDistance, DeltaSeconds, 6.f);
	const FRotator Rotation(Pitch, Yaw, 0.f);
	// The ship a little right of centre: the menu covers the left of the screen.
	const FVector Right = FRotationMatrix(Rotation).GetUnitAxis(EAxis::Y);
	const FVector Location = Target - Rotation.Vector() * Distance - Right * (bNoMenuOffset ? 0.f : Distance * 0.12f);
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

void AShipViewerController::ApplyDesign(const FShipDesign& Design, TFunction<void()> OnDone)
{
	if (!Ship)
	{
		return;
	}
	TWeakObjectPtr<AShipViewerController> Self(this);
	Ship->BuildFromDesignAsync(Design, [Self, OnDone = MoveTemp(OnDone)]()
	{
		AShipViewerController* PC = Self.Get();
		if (!PC || !PC->Ship)
		{
			return;
		}
		// The camera stays where it is: framed once at the start, then only moved by the player.
		if (OnDone)
		{
			OnDone();
		}
	});
}

void AShipViewerController::StartVisit()
{
	if (Visitor || !Ship)
	{
		return;
	}
	Orbit = GetPawn();
	// On the weather deck by the open hatch, facing forward.
	const FVector At = Ship->GetActorTransform().TransformPosition(Ship->Boarding) + FVector(0.f, 0.f, 95.f);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	Visitor = GetWorld()->SpawnActor<AShipVisitor>(At, Ship->GetActorRotation(), Params);
	if (!Visitor)
	{
		return;
	}
	Possess(Visitor);
	SetControlRotation(Ship->GetActorRotation());
	UE_LOG(LogTemp, Display, TEXT("ShipViewer: aboard at %s (ship bounds %s)"), *At.ToString(), *Ship->GetShipBounds().ToString());
	if (Menu.IsValid())
	{
		Menu->SetVisibility(EVisibility::Collapsed);
	}
	bShowMouseCursor = false;
	SetInputMode(FInputModeGameOnly());
}

void AShipViewerController::EndVisit()
{
	if (!Visitor)
	{
		return;
	}
	if (Orbit)
	{
		Possess(Orbit);
	}
	Visitor->Destroy();
	Visitor = nullptr;
	if (Menu.IsValid())
	{
		Menu->SetVisibility(EVisibility::SelfHitTestInvisible);
	}
	bShowMouseCursor = true;
	FInputModeGameAndUI Mode;
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	Mode.SetHideCursorDuringCapture(true);
	SetInputMode(Mode);
}

void AShipViewerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);
	if (Visitor && (WasInputKeyJustPressed(EKeys::Tab) || WasInputKeyJustPressed(EKeys::Escape)))
	{
		EndVisit();
	}
}

AShipViewerGameMode::AShipViewerGameMode()
{
	DefaultPawnClass = AShipViewerPawn::StaticClass();
	PlayerControllerClass = AShipViewerController::StaticClass();
}
