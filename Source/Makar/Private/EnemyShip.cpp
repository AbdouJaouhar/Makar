#include "EnemyShip.h"

#include "Kismet/GameplayStatics.h"

void AEnemyShip::Tick(float DeltaSeconds)
{
	if (!bSunk)
	{
		const APirateShip* Target = Cast<APirateShip>(UGameplayStatics::GetPlayerPawn(this, 0));
		const FVector ToTarget = Target ? Target->GetActorLocation() - GetActorLocation() : FVector::ZeroVector;
		const float Distance = ToTarget.Size2D();

		if (!Target || Target->IsSunk() || Distance > DetectionRange)
		{
			SetSailLevel(0);
			SetRudderInput(0.f);
		}
		else
		{
			const FVector TargetDir = ToTarget.GetSafeNormal2D();
			const FVector Forward = GetActorForwardVector().GetSafeNormal2D();
			const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward);

			FVector DesiredDir = TargetDir;
			if (Distance < BroadsideRange)
			{
				// Sail perpendicular to the target, picking the side that needs the smaller turn.
				DesiredDir = FVector::CrossProduct(FVector::UpVector, TargetDir);
				if (FVector::DotProduct(DesiredDir, Forward) < 0.f)
				{
					DesiredDir = -DesiredDir;
				}
			}

			const float Side = FVector::DotProduct(DesiredDir, Right);
			const bool bBehind = FVector::DotProduct(DesiredDir, Forward) < 0.f;
			SetRudderInput(bBehind ? (Side >= 0.f ? 1.f : -1.f) : Side * 2.f);
			SetSailLevel(Distance < BroadsideRange ? 1 : MaxSailLevel);

			const float TargetSide = FVector::DotProduct(TargetDir, Right);
			if (Distance < FireRange && FMath::Abs(TargetSide) > 0.94f)
			{
				FireBroadside(TargetSide > 0.f);
			}
		}
	}

	Super::Tick(DeltaSeconds);
}
