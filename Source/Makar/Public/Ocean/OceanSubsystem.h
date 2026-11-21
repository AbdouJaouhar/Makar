#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "OceanSubsystem.generated.h"

class AOceanActor;

/** Single entry point for "where is the water?" queries (buoyancy, swimming, cameras, AI). */
UCLASS()
class MAKAR_API UOceanSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Ocean")
	AOceanActor* GetOcean() const;

	/** Returns false when the level has no ocean. OutWaterZ is the surface's world Z in cm. */
	UFUNCTION(BlueprintPure, Category = "Ocean")
	bool GetWaterHeightAt(const FVector& WorldLocation, float& OutWaterZ) const;

	/** Positive when below the surface, in cm. */
	UFUNCTION(BlueprintPure, Category = "Ocean")
	float GetDepthBelowSurface(const FVector& WorldLocation) const;

private:
	mutable TWeakObjectPtr<AOceanActor> CachedOcean;
};
