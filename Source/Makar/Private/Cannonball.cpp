#include "Cannonball.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/ConstructorHelpers.h"

ACannonball::ACannonball()
{
	PrimaryActorTick.bCanEverTick = false;
	InitialLifeSpan = 8.f;

	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(15.f);
	Collision->SetCollisionProfileName(TEXT("BlockAllDynamic"));
	// Query only: the ball must not physically shove the ship that fires it.
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->OnComponentHit.AddDynamic(this, &ACannonball::OnHit);
	SetRootComponent(Collision);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Collision);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetRelativeScale3D(FVector(0.3f));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		Mesh->SetStaticMesh(SphereMesh.Object);
	}

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->UpdatedComponent = Collision;
	Movement->InitialSpeed = 0.f; // velocity is set explicitly in Launch()
	Movement->MaxSpeed = 0.f;     // no limit
	Movement->ProjectileGravityScale = 1.f;
	Movement->bRotationFollowsVelocity = true;
}

void ACannonball::BeginPlay()
{
	Super::BeginPlay();

	if (AActor* Shooter = GetOwner())
	{
		Collision->IgnoreActorWhenMoving(Shooter, true);
	}
}

void ACannonball::Launch(const FVector& WorldVelocity)
{
	Movement->Velocity = WorldVelocity;
	Movement->UpdateComponentVelocity();
}

void ACannonball::OnHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	FVector NormalImpulse, const FHitResult& Hit)
{
	if (OtherActor && OtherActor != this && OtherActor != GetOwner())
	{
		UGameplayStatics::ApplyDamage(OtherActor, Damage, GetInstigatorController(), this, nullptr);
	}
	Destroy();
}
