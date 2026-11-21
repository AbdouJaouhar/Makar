#include "Ocean/OceanBuoyancyComponent.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Ocean/OceanSubsystem.h"

UOceanBuoyancyComponent::UOceanBuoyancyComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void UOceanBuoyancyComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	SubmergedFraction = 0.f;

	UPrimitiveComponent* Body = GetOwner() ? Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent()) : nullptr;
	const UOceanSubsystem* Ocean = GetWorld() ? GetWorld()->GetSubsystem<UOceanSubsystem>() : nullptr;
	if (!Body || !Body->IsSimulatingPhysics() || !Ocean || Pontoons.Num() == 0)
	{
		return;
	}

	constexpr float Gravity = 9.81f;
	const FTransform Transform = Body->GetComponentTransform();
	const float MassPerPontoon = Body->GetMass() / Pontoons.Num();
	float TotalVolume = 0.f;
	float TotalSubmerged = 0.f;

	for (const FOceanPontoon& Pontoon : Pontoons)
	{
		const FVector Center = Transform.TransformPosition(Pontoon.RelativeLocation);
		const float R = Pontoon.Radius * 0.01f; // m
		const float FullVolume = 4.f / 3.f * PI * R * R * R;
		TotalVolume += FullVolume;

		float WaterZ = 0.f;
		if (!Ocean->GetWaterHeightAt(Center, WaterZ))
		{
			continue;
		}

		// Height of the submerged sphere cap, m.
		const float Depth = FMath::Clamp((WaterZ - (Center.Z - Pontoon.Radius)) * 0.01f, 0.f, 2.f * R);
		if (Depth <= 0.f)
		{
			continue;
		}

		const float Volume = PI * Depth * Depth * (3.f * R - Depth) / 3.f; // m³
		const float Fraction = Volume / FullVolume;
		TotalSubmerged += Volume;

		// Newtons -> Unreal force units (kg·cm/s²).
		const float Buoyancy = WaterDensity * Gravity * Volume * BuoyancyMultiplier * 100.f;

		const FVector Velocity = Body->GetPhysicsLinearVelocityAtPoint(Center);
		const FVector Drag = FVector(
			-Velocity.X * HorizontalDamping,
			-Velocity.Y * HorizontalDamping,
			-Velocity.Z * VerticalDamping) * MassPerPontoon * Fraction;

		Body->AddForceAtLocation(FVector(0.f, 0.f, Buoyancy) + Drag, Center);
	}

	SubmergedFraction = TotalVolume > 0.f ? TotalSubmerged / TotalVolume : 0.f;
}
