#include "PirateShip.h"

#include "BuoyancyComponent.h"
#include "BuoyancyTypes.h"
#include "Camera/CameraComponent.h"
#include "Cannonball.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// Hull half-size in cm: 12 m long, 4 m wide, 2 m tall.
	const FVector HullExtent(600.f, 200.f, 100.f);
}

APirateShip::APirateShip()
{
	PrimaryActorTick.bCanEverTick = true;

	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// Physics body. Unscaled on purpose: pontoon positions are in this component's local space.
	Hull = CreateDefaultSubobject<UBoxComponent>(TEXT("Hull"));
	Hull->InitBoxExtent(HullExtent);
	Hull->SetCollisionProfileName(TEXT("PhysicsActor"));
	Hull->SetGenerateOverlapEvents(true); // required for the buoyancy component to detect water bodies
	Hull->SetSimulatePhysics(true);
	Hull->SetLinearDamping(0.1f);
	Hull->SetAngularDamping(1.f);
	// Tuned against the default buoyancy coefficient: floats roughly half submerged on 4 pontoons.
	Hull->BodyInstance.SetMassOverride(3000.f);
	Hull->BodyInstance.COMNudge = FVector(0.f, 0.f, -80.f); // low centre of mass keeps the ship upright
	SetRootComponent(Hull);

	// Placeholder visuals from engine basic shapes; replace with a real ship mesh in a Blueprint subclass.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));

	HullMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HullMesh"));
	HullMesh->SetupAttachment(Hull);
	HullMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HullMesh->SetRelativeScale3D(HullExtent / 50.f); // basic cube is 100 cm
	if (CubeMesh.Succeeded())
	{
		HullMesh->SetStaticMesh(CubeMesh.Object);
	}

	MastMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MastMesh"));
	MastMesh->SetupAttachment(Hull);
	MastMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MastMesh->SetRelativeLocation(FVector(100.f, 0.f, 500.f)); // slightly forward so the bow is recognisable
	MastMesh->SetRelativeScale3D(FVector(0.4f, 0.4f, 8.f));
	if (CylinderMesh.Succeeded())
	{
		MastMesh->SetStaticMesh(CylinderMesh.Object);
	}

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(Hull);
	CameraBoom->TargetArmLength = 2800.f;
	CameraBoom->SocketOffset = FVector(0.f, 0.f, 400.f);
	CameraBoom->bUsePawnControlRotation = true; // orbit with the mouse, unaffected by the ship rolling
	CameraBoom->bDoCollisionTest = false;
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 4.f;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);

	Buoyancy = CreateDefaultSubobject<UBuoyancyComponent>(TEXT("Buoyancy"));
	const float PontoonRadius = 150.f;
	const float PX = HullExtent.X - PontoonRadius;
	const float PY = HullExtent.Y - 50.f;
	for (const FVector& Location : { FVector(PX, PY, 0.f), FVector(PX, -PY, 0.f), FVector(-PX, PY, 0.f), FVector(-PX, -PY, 0.f) })
	{
		FSphericalPontoon Pontoon;
		Pontoon.RelativeLocation = Location;
		Pontoon.Radius = PontoonRadius;
		Buoyancy->BuoyancyData.Pontoons.Add(Pontoon);
	}

	CannonballClass = ACannonball::StaticClass();
}

void APirateShip::BeginPlay()
{
	Super::BeginPlay();
	Health = MaxHealth;
}

void APirateShip::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bSunk)
	{
		return;
	}

	Rudder = FMath::FInterpTo(Rudder, RudderInput, DeltaSeconds, 2.f);

	const FVector Velocity = Hull->GetPhysicsLinearVelocity();
	const FVector Forward = Hull->GetForwardVector().GetSafeNormal2D();
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward);
	const float ForwardSpeed = FVector::DotProduct(Velocity, Forward);

	// Sails and rudder only work with the hull in the water.
	if (Buoyancy->IsInWaterBody() && !Forward.IsNearlyZero())
	{
		const float LateralSpeed = FVector::DotProduct(Velocity, Right);
		const float SailFraction = MaxSailLevel > 0 ? float(SailLevel) / float(MaxSailLevel) : 0.f;

		const FVector Acceleration =
			Forward * (SailFraction * SailAcceleration - ForwardSpeed * ForwardDrag)
			- Right * (LateralSpeed * KeelDrag);
		Hull->AddForce(Acceleration, NAME_None, /*bAccelChange=*/ true);

		// A rudder needs water flowing past it: authority scales with speed.
		const float SteerAuthority = FMath::Clamp(ForwardSpeed / FMath::Max(FullSteerSpeed, 1.f), -1.f, 1.f);
		const float YawRate = Hull->GetPhysicsAngularVelocityInRadians().Z;
		const float YawAcceleration = Rudder * TurnAcceleration * SteerAuthority - YawRate * YawDamping;
		Hull->AddTorqueInRadians(FVector(0.f, 0.f, YawAcceleration), NAME_None, /*bAccelChange=*/ true);
	}

	if (IsPlayerControlled() && GEngine)
	{
		const float Knots = ForwardSpeed / 51.44f; // 1 knot = 51.44 cm/s
		GEngine->AddOnScreenDebugMessage(1, 0.f, FColor::White, FString::Printf(
			TEXT("Voiles %d/%d   Vitesse %.0f noeuds   Coque %.0f/%.0f"),
			SailLevel, MaxSailLevel, Knots, Health, MaxHealth));
	}
}

void APirateShip::SetSailLevel(int32 NewLevel)
{
	SailLevel = FMath::Clamp(NewLevel, 0, MaxSailLevel);
}

void APirateShip::SetRudderInput(float Value)
{
	RudderInput = FMath::Clamp(Value, -1.f, 1.f);
}

void APirateShip::FireBroadside(bool bStarboard)
{
	UWorld* World = GetWorld();
	if (bSunk || !World || !CannonballClass || CannonsPerSide <= 0)
	{
		return;
	}

	const int32 SideIndex = bStarboard ? 1 : 0;
	if (World->GetTimeSeconds() < NextFireTime[SideIndex])
	{
		return;
	}
	NextFireTime[SideIndex] = World->GetTimeSeconds() + ReloadTime;

	const float SideSign = bStarboard ? 1.f : -1.f;
	const FTransform HullTransform = Hull->GetComponentTransform();
	const FVector ShipVelocity = Hull->GetPhysicsLinearVelocity();

	// Fire level with the horizon (plus elevation) even when the ship is rolling.
	const FVector Forward = Hull->GetForwardVector().GetSafeNormal2D();
	const FVector Outward = FVector::CrossProduct(FVector::UpVector, Forward) * SideSign;
	const float ElevationRad = FMath::DegreesToRadians(CannonElevationDegrees);
	const FVector FireDirection = Outward * FMath::Cos(ElevationRad) + FVector::UpVector * FMath::Sin(ElevationRad);

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.Instigator = this;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	const float GunDeckLength = HullExtent.X * 1.4f;
	for (int32 Index = 0; Index < CannonsPerSide; ++Index)
	{
		const float Alpha = CannonsPerSide > 1 ? float(Index) / float(CannonsPerSide - 1) - 0.5f : 0.f;
		const FVector LocalMuzzle(Alpha * GunDeckLength, SideSign * (HullExtent.Y + 60.f), 50.f);
		const FVector Muzzle = HullTransform.TransformPosition(LocalMuzzle);

		if (ACannonball* Ball = World->SpawnActor<ACannonball>(CannonballClass, Muzzle, FireDirection.Rotation(), SpawnParams))
		{
			Ball->Launch(FireDirection * MuzzleSpeed + ShipVelocity);
		}
	}

	// Recoil: roll away from the firing side.
	Hull->AddAngularImpulseInRadians(Forward * (-SideSign * 0.15f), NAME_None, /*bVelChange=*/ true);
}

float APirateShip::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent,
	AController* EventInstigator, AActor* DamageCauser)
{
	const float Applied = Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
	if (bSunk || Applied <= 0.f)
	{
		return 0.f;
	}

	Health = FMath::Max(Health - Applied, 0.f);
	if (Health <= 0.f)
	{
		Sink();
	}
	return Applied;
}

void APirateShip::Sink()
{
	bSunk = true;
	SailLevel = 0;
	RudderInput = 0.f;

	// Without buoyancy, gravity takes the wreck down.
	Buoyancy->SetComponentTickEnabled(false);
	Hull->SetLinearDamping(2.f);
	Hull->SetAngularDamping(2.f);

	OnSunk();

	if (!IsPlayerControlled())
	{
		SetLifeSpan(20.f);
	}
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

void APirateShip::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		UE_LOG(LogTemp, Error, TEXT("PirateShip needs the Enhanced Input component (see Config/DefaultInput.ini)."));
		return;
	}

	auto MakeAction = [this](const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* Action = NewObject<UInputAction>(this, Name);
		Action->ValueType = Type;
		return Action;
	};

	SteerAction = MakeAction(TEXT("IA_Steer"), EInputActionValueType::Axis1D);
	LookAction = MakeAction(TEXT("IA_Look"), EInputActionValueType::Axis2D);
	RaiseSailsAction = MakeAction(TEXT("IA_RaiseSails"), EInputActionValueType::Boolean);
	LowerSailsAction = MakeAction(TEXT("IA_LowerSails"), EInputActionValueType::Boolean);
	FirePortAction = MakeAction(TEXT("IA_FirePort"), EInputActionValueType::Boolean);
	FireStarboardAction = MakeAction(TEXT("IA_FireStarboard"), EInputActionValueType::Boolean);

	MappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Ship"));

	// Both ZQSD (AZERTY) and WASD (QWERTY) layouts, plus the arrow keys.
	for (const FKey& Key : { EKeys::D, EKeys::Right })
	{
		MappingContext->MapKey(SteerAction, Key);
	}
	for (const FKey& Key : { EKeys::Q, EKeys::A, EKeys::Left })
	{
		FEnhancedActionKeyMapping& Mapping = MappingContext->MapKey(SteerAction, Key);
		Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(MappingContext));
	}
	for (const FKey& Key : { EKeys::Z, EKeys::W, EKeys::Up })
	{
		MappingContext->MapKey(RaiseSailsAction, Key);
	}
	for (const FKey& Key : { EKeys::S, EKeys::Down })
	{
		MappingContext->MapKey(LowerSailsAction, Key);
	}
	MappingContext->MapKey(LookAction, EKeys::Mouse2D);
	MappingContext->MapKey(FirePortAction, EKeys::LeftMouseButton);
	MappingContext->MapKey(FireStarboardAction, EKeys::RightMouseButton);

	Input->BindAction(SteerAction, ETriggerEvent::Triggered, this, &APirateShip::OnSteer);
	Input->BindAction(SteerAction, ETriggerEvent::Completed, this, &APirateShip::OnSteerReleased);
	Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &APirateShip::OnLook);
	Input->BindAction(RaiseSailsAction, ETriggerEvent::Started, this, &APirateShip::OnRaiseSails);
	Input->BindAction(LowerSailsAction, ETriggerEvent::Started, this, &APirateShip::OnLowerSails);
	Input->BindAction(FirePortAction, ETriggerEvent::Started, this, &APirateShip::OnFirePort);
	Input->BindAction(FireStarboardAction, ETriggerEvent::Started, this, &APirateShip::OnFireStarboard);

	if (const APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (const ULocalPlayer* LocalPlayer = PC->GetLocalPlayer())
		{
			if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
			{
				Subsystem->AddMappingContext(MappingContext, 0);
			}
		}
	}
}

void APirateShip::OnSteer(const FInputActionValue& Value)
{
	SetRudderInput(Value.Get<float>());
}

void APirateShip::OnSteerReleased(const FInputActionValue& Value)
{
	SetRudderInput(0.f);
}

void APirateShip::OnLook(const FInputActionValue& Value)
{
	const FVector2D Look = Value.Get<FVector2D>();
	AddControllerYawInput(Look.X);
	AddControllerPitchInput(-Look.Y);
}

void APirateShip::OnRaiseSails(const FInputActionValue& Value)
{
	SetSailLevel(SailLevel + 1);
}

void APirateShip::OnLowerSails(const FInputActionValue& Value)
{
	SetSailLevel(SailLevel - 1);
}

void APirateShip::OnFirePort(const FInputActionValue& Value)
{
	FireBroadside(false);
}

void APirateShip::OnFireStarboard(const FInputActionValue& Value)
{
	FireBroadside(true);
}
