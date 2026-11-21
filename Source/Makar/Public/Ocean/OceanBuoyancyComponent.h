#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "OceanBuoyancyComponent.generated.h"

/** A sphere of displaced water. Place several along a hull. */
USTRUCT(BlueprintType)
struct FOceanPontoon
{
	GENERATED_BODY()

	/** Relative to the owner's root (physics) component, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy")
	FVector RelativeLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy", meta = (ClampMin = "1", Units = "Centimeters"))
	float Radius = 100.f;
};

/**
 * Archimedes buoyancy against the FFT ocean.
 * Each pontoon pushes up with the weight of the water it displaces (exact sphere-cap volume),
 * plus drag so the body settles instead of bouncing. The owner's root must simulate physics.
 */
UCLASS(ClassGroup = (Ocean), meta = (BlueprintSpawnableComponent))
class MAKAR_API UOceanBuoyancyComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UOceanBuoyancyComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION(BlueprintPure, Category = "Buoyancy")
	bool IsInWater() const { return SubmergedFraction > 0.f; }

	/** 0 = out of the water, 1 = every pontoon fully under. */
	UFUNCTION(BlueprintPure, Category = "Buoyancy")
	float GetSubmergedFraction() const { return SubmergedFraction; }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy")
	TArray<FOceanPontoon> Pontoons;

	/** kg/m³; sea water is about 1025. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy")
	float WaterDensity = 1025.f;

	/** Lower it to make the body sink (e.g. a hull taking on water). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy", meta = (ClampMin = "0"))
	float BuoyancyMultiplier = 1.f;

	/** Water resistance to vertical motion, 1/s, scaled by how submerged each pontoon is. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy", meta = (ClampMin = "0"))
	float VerticalDamping = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy", meta = (ClampMin = "0"))
	float HorizontalDamping = 0.1f;

private:
	float SubmergedFraction = 0.f;
};
