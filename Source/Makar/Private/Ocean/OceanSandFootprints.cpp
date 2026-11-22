#include "Ocean/OceanSandFootprints.h"

namespace
{
	/** Periodic heightfield of sand columns. */
	struct FSandGrid
	{
		int32 N = 0;
		float Cell = 0.f;
		TArray<float> Height;
		TArray<float> Disturbance;

		int32 Wrap(int32 I) const { return ((I % N) + N) % N; }
		int32 Index(int32 X, int32 Y) const { return Wrap(Y) * N + Wrap(X); }
	};

	/**
	 * Approximate signed distance (m) to the outline of a right or left foot: heel and forefoot ellipses joined
	 * by the arch, in foot space (U forward from the middle of the foot, V towards the outside). Negative inside.
	 * Depth weight in 0..1: deepest at the heel and under the ball of the foot.
	 */
	float FootDistance(float U, float V, float Scale, float& OutDepthWeight)
	{
		U /= Scale;
		V /= Scale;
		const auto Ellipse = [](float PU, float PV, float CU, float CV, float RU, float RV)
		{
			const float DU = (PU - CU) / RU;
			const float DV = (PV - CV) / RV;
			return (FMath::Sqrt(DU * DU + DV * DV) - 1.f) * FMath::Min(RU, RV);
		};
		const float Heel = Ellipse(U, V, -0.085f, 0.f, 0.045f, 0.036f);
		const float Arch = Ellipse(U, V, -0.01f, 0.008f, 0.06f, 0.03f);
		const float Ball = Ellipse(U, V, 0.06f, 0.004f, 0.055f, 0.046f);
		const float Distance = FMath::Min3(Heel, Arch, Ball);
		OutDepthWeight = FMath::Max(FMath::Clamp(-Heel / 0.02f, 0.f, 1.f), 0.85f * FMath::Clamp(-Ball / 0.02f, 0.f, 1.f));
		OutDepthWeight = FMath::Max(OutDepthWeight, 0.55f * FMath::Clamp(-Arch / 0.015f, 0.f, 1.f));
		return Distance * Scale;
	}

	/** Moves sand down every slope steeper than the angle of repose, inside a box (periodic). */
	void Erode(FSandGrid& Grid, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY, float TanRepose, float Rate, int32 Iterations)
	{
		const float MaxStep = TanRepose * Grid.Cell;
		static const int32 Offsets[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
		for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
		{
			bool bMoved = false;
			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const int32 Here = Grid.Index(X, Y);
					for (const auto& Offset : Offsets)
					{
						const int32 There = Grid.Index(X + Offset[0], Y + Offset[1]);
						const float Excess = Grid.Height[Here] - Grid.Height[There] - MaxStep;
						if (Excess > 1e-5f)
						{
							const float Moved = Rate * 0.5f * Excess;
							Grid.Height[Here] -= Moved;
							Grid.Height[There] += Moved;
							bMoved = true;
						}
					}
				}
			}
			if (!bMoved)
			{
				break;
			}
		}
	}

	/** One footprint: collision, displacement of the uncompressed sand into a rim, local erosion. */
	void StampFoot(FSandGrid& Grid, const FOceanSandFootprintSettings& Settings, const FVector2f& Center, float Heading,
		bool bLeft, float Depth, float PushOff, float Scale)
	{
		const FVector2f Forward(FMath::Cos(Heading), FMath::Sin(Heading));
		const FVector2f Outside = bLeft ? FVector2f(Forward.Y, -Forward.X) : FVector2f(-Forward.Y, Forward.X);
		const float RimWidth = 0.045f;
		const int32 Radius = FMath::CeilToInt((0.16f * Scale + RimWidth) / Grid.Cell);
		const int32 CX = FMath::FloorToInt(Center.X / Grid.Cell);
		const int32 CY = FMath::FloorToInt(Center.Y / Grid.Cell);

		// The sole sinks to a flat-ish bottom below the sand it lands on.
		float Reference = 0.f;
		int32 Count = 0;
		for (int32 Y = -2; Y <= 2; ++Y)
		{
			for (int32 X = -2; X <= 2; ++X)
			{
				Reference += Grid.Height[Grid.Index(CX + X, CY + Y)];
				++Count;
			}
		}
		Reference /= Count;

		// Collision.
		double Displaced = 0.0;
		TArray<TPair<int32, float>, TInlineAllocator<4096>> Rim;
		float RimWeight = 0.f;
		for (int32 Y = -Radius; Y <= Radius; ++Y)
		{
			for (int32 X = -Radius; X <= Radius; ++X)
			{
				const FVector2f Offset = FVector2f((CX + X + 0.5f) * Grid.Cell, (CY + Y + 0.5f) * Grid.Cell) - Center;
				const float U = FVector2f::DotProduct(Offset, Forward);
				const float V = FVector2f::DotProduct(Offset, Outside);
				float DepthWeight;
				const float Distance = FootDistance(U, V, Scale, DepthWeight);
				const int32 Index = Grid.Index(CX + X, CY + Y);
				if (Distance < 0.f)
				{
					// Toes dig in deeper when running (push-off).
					const float Toe = FMath::Clamp((U / Scale - 0.06f) / 0.05f, 0.f, 1.f);
					const float Bottom = Reference - Depth * (DepthWeight + PushOff * 0.6f * Toe);
					if (Grid.Height[Index] > Bottom)
					{
						Displaced += Grid.Height[Index] - Bottom;
						Grid.Height[Index] = Bottom;
					}
					Grid.Disturbance[Index] = 1.f;
				}
				else if (Distance < RimWidth)
				{
					// The rim: sand pushed out sideways, and back and up at the toe when pushing off.
					const float Falloff = FMath::Square(1.f - Distance / RimWidth);
					const float Front = FMath::Clamp(U / (0.1f * Scale), 0.f, 1.f);
					const float Weight = Falloff * (1.f + (0.6f + 2.5f * PushOff) * Front);
					Rim.Emplace(Index, Weight);
					RimWeight += Weight;
					Grid.Disturbance[Index] = FMath::Max(Grid.Disturbance[Index], Falloff);
				}
			}
		}

		// Displacement: the uncompressed part of the sand goes to the rim.
		if (RimWeight > 0.f)
		{
			const float Heaped = float(Displaced) * (1.f - Settings.Compression) / RimWeight;
			for (const TPair<int32, float>& Cell : Rim)
			{
				Grid.Height[Cell.Key] += Heaped * Cell.Value;
			}
		}

		// Erosion: the walls of the print and the rim slump to the angle of repose.
		Erode(Grid, CX - Radius - 1, CY - Radius - 1, CX + Radius + 1, CY + Radius + 1,
			FMath::Tan(Settings.ReposeAngle), Settings.ErosionRate, 60);
	}

	/** The wind blurs the surface a little (older prints soften, the disturbance fades). */
	void Weather(FSandGrid& Grid, int32 Passes, float DisturbanceFade)
	{
		TArray<float> Next;
		Next.SetNumUninitialized(Grid.Height.Num());
		for (int32 Pass = 0; Pass < Passes; ++Pass)
		{
			for (int32 Y = 0; Y < Grid.N; ++Y)
			{
				for (int32 X = 0; X < Grid.N; ++X)
				{
					Next[Y * Grid.N + X] = 0.5f * Grid.Height[Y * Grid.N + X] + 0.125f * (Grid.Height[Grid.Index(X + 1, Y)]
						+ Grid.Height[Grid.Index(X - 1, Y)] + Grid.Height[Grid.Index(X, Y + 1)] + Grid.Height[Grid.Index(X, Y - 1)]);
				}
			}
			Swap(Grid.Height, Next);
		}
		for (float& Value : Grid.Disturbance)
		{
			Value *= DisturbanceFade;
		}
	}
}

TArray<FVector2f> OceanSimulateSandFootprints(const FOceanSandFootprintSettings& Settings)
{
	FSandGrid Grid;
	Grid.N = FMath::Max(Settings.Resolution, 16);
	Grid.Cell = Settings.TileSizeM / Grid.N;
	Grid.Height.Init(0.f, Grid.N * Grid.N);
	Grid.Disturbance.Init(0.f, Grid.N * Grid.N);

	FRandomStream Random(Settings.Seed);
	for (int32 Trail = 0; Trail < Settings.Trails; ++Trail)
	{
		// A third of the people run: longer steps, deeper toes, a bigger heap thrown back. Stride = one step.
		const bool bRunning = Random.FRand() < 0.3f;
		const float Stride = bRunning ? Random.FRandRange(1.0f, 1.3f) : Random.FRandRange(0.6f, 0.75f);
		const float Spread = bRunning ? 0.06f : Random.FRandRange(0.08f, 0.11f);
		const float PushOff = bRunning ? 1.f : Random.FRandRange(0.1f, 0.3f);
		const float Scale = Random.FRandRange(0.85f, 1.1f); // children to tall adults
		const float Depth = Settings.FootDepthM * Random.FRandRange(0.7f, 1.2f) * (bRunning ? 1.3f : 1.f);

		FVector2f Position(Random.FRand() * Settings.TileSizeM, Random.FRand() * Settings.TileSizeM);
		float Heading = Random.FRand() * 2.f * UE_PI;
		// Long enough to cross the tile and wrap around, so trails run on across tile borders.
		const int32 Steps = FMath::CeilToInt(1.3f * Settings.TileSizeM / Stride);
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			Heading += Random.FRandRange(-0.12f, 0.12f);
			const bool bLeft = (Step % 2) == 0;
			const FVector2f Forward(FMath::Cos(Heading), FMath::Sin(Heading));
			const FVector2f Side(-Forward.Y, Forward.X);
			const FVector2f Foot = Position + Side * (bLeft ? Spread : -Spread);
			const float Toe = Heading + (bLeft ? 0.12f : -0.12f) + Random.FRandRange(-0.08f, 0.08f); // feet turn out
			StampFoot(Grid, Settings, Foot, Toe, bLeft, Depth, PushOff, Scale);
			Position += Forward * Stride; // one step, left then right
		}

		// Time passes until the next walker: the wind softens what is there.
		Weather(Grid, Random.FRand() < 0.3f ? 1 : 0, 0.93f);
	}

	TArray<FVector2f> Texels;
	Texels.SetNumUninitialized(Grid.N * Grid.N);
	for (int32 Index = 0; Index < Texels.Num(); ++Index)
	{
		Texels[Index] = FVector2f(Grid.Height[Index], FMath::Clamp(Grid.Disturbance[Index], 0.f, 1.f));
	}
	return Texels;
}
