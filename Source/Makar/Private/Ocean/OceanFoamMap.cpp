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

	// The swash's horizontal run over the beach face per metre it rises (a 1:8 face; = OceanSurfFoam's Shift).
	constexpr float SwashRunPerRise = 8.f;

	/**
	 * Where the water at PositionM moved this step (m): the shore waves' horizontal motion (their orbit and the lip
	 * thrown forward, from their displacement now and a step ago), and on the beach the swash running up and back
	 * down the face. Foam rides the water, so it rides up the beach with each uprush, back down with the backwash and
	 * is dragged out into streaks wherever neighbouring water moves differently.
	 */
	FVector2f ShoreWaterMotion(const FOceanShoreData& Shore, const FOceanShoreSwell& Swell, const FVector2f& PositionM, float Time, float Dt)
	{
		const float Depth = Shore.DepthAt(PositionM);
		if (Depth >= Swell.MaxDepth || Depth < -OceanSwashMaxHeightM || Dt <= 0.f)
		{
			return FVector2f::ZeroVector;
		}
		const FOceanShoreWaveSample Now = OceanShoreWave(Shore, PositionM, Time, Swell);
		const FOceanShoreWaveSample Before = OceanShoreWave(Shore, PositionM, Time - Dt, Swell);
		FVector2f Motion(Now.Displacement.X - Before.Displacement.X, Now.Displacement.Y - Before.Displacement.Y);
		if (Depth < 0.6f)
		{
			// Up the slope: down the depth gradient.
			const FVector2f Gradient(Shore.DepthAt(PositionM + FVector2f(1.f, 0.f)) - Depth, Shore.DepthAt(PositionM + FVector2f(0.f, 1.f)) - Depth);
			const float Length = Gradient.Size();
			if (Length > 1e-4f)
			{
				const float InSwash = 1.f - FMath::SmoothStep(0.3f, 0.6f, Depth);
				Motion += -Gradient / Length * (SwashRunPerRise * (Now.Displacement.Z - Before.Displacement.Z) * InSwash);
			}
		}
		return Motion;
	}

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
		Age.SetNumZeroed(Size * Size);
		NextAge.SetNumZeroed(Size * Size);
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
	const FOceanShoreSwell Swell = Settings.GetShoreSwell();
	// Distance travelled this step, in texels.
	const FVector2f Drift = FVector2f(FMath::Cos(WindAngle), FMath::Sin(WindAngle)) * (Settings.WindSpeed * WindDriftFactor * Dt / TexelSizeM);

	ParallelFor(Size, [&](int32 Y)
	{
		for (int32 X = 0; X < Size; ++X)
		{
			const FVector2f PositionM((float(OriginTexel.X + X) + 0.5f) * TexelSizeM, (float(OriginTexel.Y + Y) + 0.5f) * TexelSizeM);
			// Semi-Lagrangian advection: the foam here is the foam that was where this water came from a moment
			// ago, carried by the wind drift and, near the shore, by the waves and the swash.
			FVector2f Back = Drift;
			if (Shore)
			{
				Back += ShoreWaterMotion(*Shore, Swell, PositionM, Frame.Time, Dt) / TexelSizeM;
			}
			const float FromX = float(X) - Back.X;
			const float FromY = float(Y) - Back.Y;
			const float Advected = SampleFoam(FromX, FromY);
			const float AdvectedAge = SampleAge(FromX, FromY);

			const float Jacobian = CombinedJacobian(Frame, Shore, PositionM);
			float Created = FMath::Clamp((Settings.FoamThreshold - Jacobian) * Settings.FoamIntensity * CreationGain, 0.f, 1.f);
			if (Shore)
			{
				// Whitewater of the waves breaking on the beach; it lingers, rides the water and drifts.
				Created = FMath::Max(Created, OceanShoreWave(*Shore, PositionM, Frame.Time, Swell).Foam);
			}

			const float Kept = Advected * Fade;
			const int32 Index = Y * Size + X;
			Next[Index] = FMath::Max(Created, Kept);
			// Age (s): fresh where new foam was made, else that of the foam carried here.
			NextAge[Index] = Created >= Kept ? 0.f : FMath::Min(AdvectedAge + Dt, 60.f);
		}
	});
	Swap(Foam, Next);
	Swap(Age, NextAge);

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
			NextAge[Y * Size + X] = bInside ? Age[SourceY * Size + SourceX] : 0.f;
		}
	});
	Swap(Foam, Next);
	Swap(Age, NextAge);
	OriginTexel = NewOrigin;
}

float FOceanFoamMap::SampleFoam(float X, float Y) const
{
	return SampleGrid(Foam, X, Y);
}

float FOceanFoamMap::SampleAge(float X, float Y) const
{
	return SampleGrid(Age, X, Y);
}

float FOceanFoamMap::SampleGrid(const TArray<float>& Grid, float X, float Y) const
{
	const float FloorX = FMath::FloorToFloat(X);
	const float FloorY = FMath::FloorToFloat(Y);
	const float FracX = X - FloorX;
	const float FracY = Y - FloorY;
	const int32 X0 = int32(FloorX);
	const int32 Y0 = int32(FloorY);

	auto At = [&Grid](int32 TexelX, int32 TexelY)
	{
		return TexelX >= 0 && TexelX < Size && TexelY >= 0 && TexelY < Size ? Grid[TexelY * Size + TexelX] : 0.f;
	};
	const float Bottom = FMath::Lerp(At(X0, Y0), At(X0 + 1, Y0), FracX);
	const float Top = FMath::Lerp(At(X0, Y0 + 1), At(X0 + 1, Y0 + 1), FracX);
	return FMath::Lerp(Bottom, Top, FracY);
}

void FOceanFoamMap::BuildMips(FOceanFrame& Frame) const
{
	Frame.FoamMapMips.SetNum(NumMips);

	TArray<float> Level = Foam;
	TArray<float> LevelAge = Age;
	int32 LevelSize = Size;
	for (int32 Mip = 0; Mip < NumMips; ++Mip)
	{
		TArray<FFloat16Color>& Out = Frame.FoamMapMips[Mip];
		Out.SetNumUninitialized(LevelSize * LevelSize);
		for (int32 Index = 0; Index < LevelSize * LevelSize; ++Index)
		{
			// R = foam, G = its age (s): the shader turns old foam into a lace of streaks.
			Out[Index] = FFloat16Color(FLinearColor(Level[Index], LevelAge[Index], 0.f, 0.f));
		}

		if (Mip + 1 < NumMips)
		{
			const int32 NextSize = FMath::Max(LevelSize / 2, 1);
			TArray<float> Smaller, SmallerAge;
			Smaller.SetNumUninitialized(NextSize * NextSize);
			SmallerAge.SetNumUninitialized(NextSize * NextSize);
			for (int32 Y = 0; Y < NextSize; ++Y)
			{
				for (int32 X = 0; X < NextSize; ++X)
				{
					const int32 SX = X * 2;
					const int32 SY = Y * 2;
					const int32 Corners[4] = { SY * LevelSize + SX, SY * LevelSize + SX + 1, (SY + 1) * LevelSize + SX, (SY + 1) * LevelSize + SX + 1 };
					float Sum = 0.f;
					float AgeSum = 0.f;
					for (const int32 J : Corners)
					{
						Sum += Level[J];
						AgeSum += Level[J] * LevelAge[J];
					}
					Smaller[Y * NextSize + X] = 0.25f * Sum;
					SmallerAge[Y * NextSize + X] = Sum > 1e-4f ? AgeSum / Sum : 0.f; // the age of the foam there
				}
			}
			Level = MoveTemp(Smaller);
			LevelAge = MoveTemp(SmallerAge);
			LevelSize = NextSize;
		}
	}
}
