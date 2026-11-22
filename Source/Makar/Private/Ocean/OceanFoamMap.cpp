#include "Ocean/OceanFoamMap.h"

#include "Async/ParallelFor.h"
#include "Ocean/OceanShoreData.h"
#include "Ocean/OceanSimulation.h"

namespace
{
	// Weight of each cascade in the breaking test. Whitecaps form on the crests of the dominant waves
	// (tens of metres long). Short waves have the steepest slopes, so letting them in sprinkles small
	// flecks evenly over the whole sea instead of clustering foam on the big crests.
	constexpr float CascadeWeights[FOceanSimulation::NumCascades] = { 1.f, 0.35f, 0.f };

	// The map saturates faster than the distant per-cascade foam: each breaking crest gets a dense,
	// opaque core, and the lacy look comes from the decaying trail. Same settings drive both.
	constexpr float CreationGain = 5.f;

	// Surface foam drifts at a few percent of the wind speed.
	constexpr float WindDriftFactor = 0.03f;

	int32 Wrap(int32 Value, int32 N)
	{
		const int32 Result = Value % N;
		return Result < 0 ? Result + N : Result;
	}

	/** Bilinear Jacobian terms of one cascade, same texel convention as the GPU (texel i centred on (i + 0.5) / N). */
	FVector3f SampleJacobianTerms(const FOceanCascadeFrame& Cascade, const FVector2f& PositionM)
	{
		const int32 N = Cascade.Resolution;
		const float U = PositionM.X / Cascade.PatchSize * N - 0.5f;
		const float V = PositionM.Y / Cascade.PatchSize * N - 0.5f;
		const float FloorU = FMath::FloorToFloat(U);
		const float FloorV = FMath::FloorToFloat(V);
		const float FracU = U - FloorU;
		const float FracV = V - FloorV;
		const int32 X0 = Wrap(int32(FloorU), N);
		const int32 Y0 = Wrap(int32(FloorV), N);
		const int32 X1 = (X0 + 1) % N;
		const int32 Y1 = (Y0 + 1) % N;

		const TArray<FVector3f>& Terms = Cascade.JacobianTerms;
		const FVector3f Bottom = FMath::Lerp(Terms[Y0 * N + X0], Terms[Y0 * N + X1], FracU);
		const FVector3f Top = FMath::Lerp(Terms[Y1 * N + X0], Terms[Y1 * N + X1], FracU);
		return FMath::Lerp(Bottom, Top, FracV);
	}

	/** Jacobian of the whole surface: 1 = undisturbed, < 1 compressed, < 0 folding over. */
	float CombinedJacobian(const FOceanFrame& Frame, const FOceanShoreData* Shore, const FVector2f& PositionM)
	{
		const float Depth = Shore ? Shore->DepthAt(PositionM) : FOceanShoreData::DeepWaterM;
		FVector3f Terms = FVector3f::ZeroVector;
		const int32 NumCascades = FMath::Min(Frame.Cascades.Num(), FOceanSimulation::NumCascades);
		for (int32 Index = 0; Index < NumCascades; ++Index)
		{
			if (CascadeWeights[Index] <= 0.f)
			{
				continue; // also the GPU cascade, which has no CPU data
			}
			const FOceanCascadeFrame& Cascade = Frame.Cascades[Index];
			const float Attenuation = OceanDepthAttenuation(Depth, Cascade.SignificantHeight);
			Terms += SampleJacobianTerms(Cascade, PositionM) * (CascadeWeights[Index] * Attenuation);
		}
		return (1.f + Terms.X) * (1.f + Terms.Y) - Terms.Z * Terms.Z;
	}
}

void FOceanFoamMap::Simulate(FOceanFrame& Frame, const FOceanSettings& Settings, const FOceanShoreData* Shore, const FVector2f& CenterM, float DeltaTime)
{
	if (!Frame.bValid || Frame.Cascades.Num() == 0)
	{
		return;
	}
	for (int32 Index = 0; Index < FMath::Min(Frame.Cascades.Num(), FOceanSimulation::NumCascades); ++Index)
	{
		const FOceanCascadeFrame& Cascade = Frame.Cascades[Index];
		if (CascadeWeights[Index] > 0.f && (Cascade.JacobianTerms.Num() != Cascade.Resolution * Cascade.Resolution || Cascade.Resolution <= 0))
		{
			return;
		}
	}

	const FIntPoint NewOrigin(FMath::FloorToInt(CenterM.X / TexelSizeM) - Size / 2, FMath::FloorToInt(CenterM.Y / TexelSizeM) - Size / 2);
	if (!bInitialized)
	{
		Foam.SetNumZeroed(Size * Size);
		Next.SetNumZeroed(Size * Size);
		OriginTexel = NewOrigin;
		bInitialized = true;
	}
	else if (NewOrigin != OriginTexel)
	{
		Recenter(NewOrigin);
	}

	const float Dt = FMath::Clamp(DeltaTime, 0.f, 0.1f);
	const float Fade = FMath::Exp(-Settings.FoamDecay * Dt);
	const float WindAngle = FMath::DegreesToRadians(Settings.WindDirection);
	// Distance travelled this step, in texels.
	const FVector2f Drift = FVector2f(FMath::Cos(WindAngle), FMath::Sin(WindAngle)) * (Settings.WindSpeed * WindDriftFactor * Dt / TexelSizeM);

	ParallelFor(Size, [&](int32 Y)
	{
		for (int32 X = 0; X < Size; ++X)
		{
			// Semi-Lagrangian advection: the foam here is the foam that was upwind a moment ago.
			const float Advected = SampleFoam(float(X) - Drift.X, float(Y) - Drift.Y);

			const FVector2f PositionM((float(OriginTexel.X + X) + 0.5f) * TexelSizeM, (float(OriginTexel.Y + Y) + 0.5f) * TexelSizeM);
			const float Jacobian = CombinedJacobian(Frame, Shore, PositionM);
			float Created = FMath::Clamp((Settings.FoamThreshold - Jacobian) * Settings.FoamIntensity * CreationGain, 0.f, 1.f);
			if (Shore)
			{
				// Whitewater of the waves breaking on the beach; it lingers and drifts like the whitecaps.
				Created = FMath::Max(Created, OceanShoreWave(*Shore, PositionM, Frame.Time, Settings.GetShoreSwell()).Foam);
			}

			Next[Y * Size + X] = FMath::Max(Created, Advected * Fade);
		}
	});
	Swap(Foam, Next);

	BuildMips(Frame);
	Frame.FoamMapOrigin = FVector2f(float(OriginTexel.X), float(OriginTexel.Y)) * TexelSizeM;
	Frame.FoamMapSize = Size * TexelSizeM;
}

void FOceanFoamMap::Recenter(const FIntPoint& NewOrigin)
{
	const FIntPoint Shift = NewOrigin - OriginTexel;
	ParallelFor(Size, [&](int32 Y)
	{
		const int32 SourceY = Y + Shift.Y;
		for (int32 X = 0; X < Size; ++X)
		{
			const int32 SourceX = X + Shift.X;
			const bool bInside = SourceX >= 0 && SourceX < Size && SourceY >= 0 && SourceY < Size;
			Next[Y * Size + X] = bInside ? Foam[SourceY * Size + SourceX] : 0.f;
		}
	});
	Swap(Foam, Next);
	OriginTexel = NewOrigin;
}

float FOceanFoamMap::SampleFoam(float X, float Y) const
{
	const float FloorX = FMath::FloorToFloat(X);
	const float FloorY = FMath::FloorToFloat(Y);
	const float FracX = X - FloorX;
	const float FracY = Y - FloorY;
	const int32 X0 = int32(FloorX);
	const int32 Y0 = int32(FloorY);

	auto At = [this](int32 TexelX, int32 TexelY)
	{
		return TexelX >= 0 && TexelX < Size && TexelY >= 0 && TexelY < Size ? Foam[TexelY * Size + TexelX] : 0.f;
	};
	const float Bottom = FMath::Lerp(At(X0, Y0), At(X0 + 1, Y0), FracX);
	const float Top = FMath::Lerp(At(X0, Y0 + 1), At(X0 + 1, Y0 + 1), FracX);
	return FMath::Lerp(Bottom, Top, FracY);
}

void FOceanFoamMap::BuildMips(FOceanFrame& Frame) const
{
	Frame.FoamMapMips.SetNum(NumMips);

	TArray<float> Level = Foam;
	int32 LevelSize = Size;
	for (int32 Mip = 0; Mip < NumMips; ++Mip)
	{
		TArray<FFloat16Color>& Out = Frame.FoamMapMips[Mip];
		Out.SetNumUninitialized(LevelSize * LevelSize);
		for (int32 Index = 0; Index < LevelSize * LevelSize; ++Index)
		{
			Out[Index] = FFloat16Color(FLinearColor(Level[Index], 0.f, 0.f, 0.f));
		}

		if (Mip + 1 < NumMips)
		{
			const int32 NextSize = FMath::Max(LevelSize / 2, 1);
			TArray<float> Smaller;
			Smaller.SetNumUninitialized(NextSize * NextSize);
			for (int32 Y = 0; Y < NextSize; ++Y)
			{
				for (int32 X = 0; X < NextSize; ++X)
				{
					const int32 SX = X * 2;
					const int32 SY = Y * 2;
					Smaller[Y * NextSize + X] = 0.25f * (Level[SY * LevelSize + SX] + Level[SY * LevelSize + SX + 1]
						+ Level[(SY + 1) * LevelSize + SX] + Level[(SY + 1) * LevelSize + SX + 1]);
				}
			}
			Level = MoveTemp(Smaller);
			LevelSize = NextSize;
		}
	}
}
