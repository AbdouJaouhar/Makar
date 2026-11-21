#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Cannonball.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UProjectileMovementComponent;

UCLASS()
class MAKAR_API ACannonball : public AActor
{
	GENERATED_BODY()

public:
	ACannonball();

	/** Gives the ball its world-space velocity. Call right after spawning. */
	void Launch(const FVector& WorldVelocity);

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Cannonball")
	float Damage = 10.f;

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void OnHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		FVector NormalImpulse, const FHitResult& Hit);

	UPROPERTY(VisibleAnywhere, Category = "Cannonball")
	TObjectPtr<USphereComponent> Collision;

	UPROPERTY(VisibleAnywhere, Category = "Cannonball")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, Category = "Cannonball")
	TObjectPtr<UProjectileMovementComponent> Movement;
};
