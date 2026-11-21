#include "Ocean/OceanSubsystem.h"

#include "EngineUtils.h"
#include "Ocean/OceanActor.h"

AOceanActor* UOceanSubsystem::GetOcean() const
{
	if (!CachedOcean.IsValid())
	{
		if (UWorld* World = GetWorld())
		{
			TActorIterator<AOceanActor> It(World);
			CachedOcean = It ? *It : nullptr;
		}
	}
	return CachedOcean.Get();
}

bool UOceanSubsystem::GetWaterHeightAt(const FVector& WorldLocation, float& OutWaterZ) const
{
	if (const AOceanActor* Ocean = GetOcean())
	{
		OutWaterZ = Ocean->GetWaterHeightAt(WorldLocation);
		return true;
	}
	OutWaterZ = 0.f;
	return false;
}

float UOceanSubsystem::GetDepthBelowSurface(const FVector& WorldLocation) const
{
	float WaterZ = 0.f;
	return GetWaterHeightAt(WorldLocation, WaterZ) ? WaterZ - WorldLocation.Z : -BIG_NUMBER;
}
