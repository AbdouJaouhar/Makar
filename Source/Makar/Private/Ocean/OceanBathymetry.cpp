#include "Ocean/OceanBathymetry.h"

#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Materials/MaterialInterface.h"
#include "Ocean/OceanMeshComponent.h"
#include "Ocean/OceanSandFootprints.h"
#include "TextureResource.h"

namespace
{
	// Low-frequency harmonics that bend the circular coastline into bays and headlands.
	constexpr int32 NumCoastHarmonics = 5;
	constexpr float CoastFrequencies[NumCoastHarmonics] = { 2.f, 3.f, 5.f, 7.f, 11.f };
	constexpr float CoastAmplitudes[NumCoastHarmonics] = { 0.45f, 0.3f, 0.15f, 0.07f, 0.03f };

	/**
	 * Euclidean distance (in texels) from every cell to the nearest seed cell, by jump flooding
	 * (Rong & Tan 2006) followed by one extra pass with step 1, which removes most of its errors.
	 * Cells without any seed in the grid get -1.
	 */
	TArray<int32> NearestSeeds(const TArray<bool>& IsSeed, int32 N)
	{
		TArray<int32> Nearest;
		Nearest.SetNumUninitialized(N * N);
		for (int32 Index = 0; Index < N * N; ++Index)
		{
			Nearest[Index] = IsSeed[Index] ? Index : -1;
		}
		TArray<int32> Next;
		Next.SetNumUninitialized(N * N);

		auto Pass = [&](int32 Step)
		{
			ParallelFor(N, [&](int32 Y)
			{
				for (int32 X = 0; X < N; ++X)
				{
					int32 Best = Nearest[Y * N + X];
					int64 BestDistance = MAX_int64;
					if (Best >= 0)
					{
						const int32 DX = Best % N - X;
						const int32 DY = Best / N - Y;
						BestDistance = int64(DX) * DX + int64(DY) * DY;
					}
					for (int32 OY = -1; OY <= 1; ++OY)
					{
						for (int32 OX = -1; OX <= 1; ++OX)
						{
							const int32 SX = X + OX * Step;
							const int32 SY = Y + OY * Step;
							if (SX < 0 || SX >= N || SY < 0 || SY >= N)
							{
								continue;
							}
							const int32 Candidate = Nearest[SY * N + SX];
							if (Candidate < 0)
							{
								continue;
							}
							const int32 DX = Candidate % N - X;
							const int32 DY = Candidate / N - Y;
							const int64 Distance = int64(DX) * DX + int64(DY) * DY;
							if (Distance < BestDistance)
							{
								BestDistance = Distance;
								Best = Candidate;
							}
						}
					}
					Next[Y * N + X] = Best;
				}
			});
			Swap(Nearest, Next);
		};

		for (int32 Step = N / 2; Step >= 1; Step /= 2)
		{
			Pass(Step);
		}
		Pass(1);
		return Nearest;
	}
}

namespace
{
	/**
	 * Time a shallow-water wave takes to reach the shore from each cell: solves the eikonal equation
	 * |grad T| = 1 / sqrt(g h) by fast sweeping (Zhao 2005), with T = 0 on land. Crests placed on its
	 * isolines slow down, bunch up and bend with the sea floor like real refracting swell.
	 */
	TArray<float> WaveTravelTime(const TArray<float>& Heights, int32 N, float TexelM)
	{
		constexpr float Gravity = 9.81f;
		constexpr float MinDepthM = 0.3f; // keeps the speed finite at the waterline
		TArray<float> Time;
		TArray<float> Slowness; // s per metre
		Time.SetNumUninitialized(N * N);
		Slowness.SetNumUninitialized(N * N);
		for (int32 Index = 0; Index < N * N; ++Index)
		{
			const float Depth = -Heights[Index];
			Time[Index] = Depth > 0.f ? MAX_flt : 0.f;
			Slowness[Index] = 1.f / FMath::Sqrt(Gravity * FMath::Max(Depth, MinDepthM));
		}

		auto At = [&](int32 X, int32 Y) { return (X < 0 || X >= N || Y < 0 || Y >= N) ? MAX_flt : Time[Y * N + X]; };
		for (int32 Iteration = 0; Iteration < 3; ++Iteration)
		{
			for (int32 Sweep = 0; Sweep < 4; ++Sweep)
			{
				const bool bReverseX = (Sweep & 1) != 0;
				const bool bReverseY = (Sweep & 2) != 0;
				for (int32 J = 0; J < N; ++J)
				{
					const int32 Y = bReverseY ? N - 1 - J : J;
					for (int32 I = 0; I < N; ++I)
					{
						const int32 X = bReverseX ? N - 1 - I : I;
						const int32 Index = Y * N + X;
						if (Time[Index] == 0.f)
						{
							continue; // land
						}
						const float A = FMath::Min(At(X - 1, Y), At(X + 1, Y));
						const float B = FMath::Min(At(X, Y - 1), At(X, Y + 1));
						if (A == MAX_flt && B == MAX_flt)
						{
							continue;
						}
						// Godunov upwind update.
						const float F = Slowness[Index] * TexelM;
						float Candidate;
						if (FMath::Abs(A - B) >= F)
						{
							Candidate = FMath::Min(A, B) + F;
						}
						else
						{
							Candidate = 0.5f * (A + B + FMath::Sqrt(2.f * F * F - (A - B) * (A - B)));
						}
						Time[Index] = FMath::Min(Time[Index], Candidate);
					}
				}
			}
		}
		for (float& Value : Time)
		{
			if (Value == MAX_flt)
			{
				Value = FOceanShoreData::DeepWaterM; // no shore anywhere in the grid
			}
		}
		return Time;
	}
}

namespace
{
	/**
	 * Box-counting dimension of a curve drawn on the grid: number of boxes of side S (in cells) that touch it,
	 * for S = 2..64, and D = -slope of log N against log S (least squares). Box sizes start at 2 cells because
	 * the shoreline mask is two cells thick.
	 */
	float BoxCountingDimension(const TArray<bool>& Mask, int32 N)
	{
		TArray<float> LogSize, LogCount;
		for (int32 Size = 2; Size <= 64; Size *= 2)
		{
			int32 Count = 0;
			for (int32 BY = 0; BY < N; BY += Size)
			{
				for (int32 BX = 0; BX < N; BX += Size)
				{
					bool bTouches = false;
					for (int32 Y = BY; Y < FMath::Min(BY + Size, N) && !bTouches; ++Y)
					{
						for (int32 X = BX; X < FMath::Min(BX + Size, N); ++X)
						{
							if (Mask[Y * N + X])
							{
								bTouches = true;
								break;
							}
						}
					}
					Count += bTouches ? 1 : 0;
				}
			}
			if (Count > 0)
			{
				LogSize.Add(FMath::Loge(float(Size)));
				LogCount.Add(FMath::Loge(float(Count)));
			}
		}
		if (LogSize.Num() < 2)
		{
			return 0.f;
		}
		float MeanX = 0.f, MeanY = 0.f;
		for (int32 I = 0; I < LogSize.Num(); ++I)
		{
			MeanX += LogSize[I];
			MeanY += LogCount[I];
		}
		MeanX /= LogSize.Num();
		MeanY /= LogSize.Num();
		float Covariance = 0.f, Variance = 0.f;
		for (int32 I = 0; I < LogSize.Num(); ++I)
		{
			Covariance += (LogSize[I] - MeanX) * (LogCount[I] - MeanY);
			Variance += (LogSize[I] - MeanX) * (LogSize[I] - MeanX);
		}
		return Variance > 0.f ? -Covariance / Variance : 0.f;
	}
}

AOceanBathymetry::AOceanBathymetry()
{
	PrimaryActorTick.bCanEverTick = false;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	// UOceanMeshComponent does not save its geometry: the terrain is rebuilt on load.
	Terrain = CreateDefaultSubobject<UOceanMeshComponent>(TEXT("Terrain"));
	Terrain->SetupAttachment(RootComponent);
	Terrain->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	TerrainMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/M_Seabed.M_Seabed")));
}

void AOceanBathymetry::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Build(); // the island is centred on the actor: moving it moves the whole coastline
}

void AOceanBathymetry::BeginPlay()
{
	Super::BeginPlay();
	// OnConstruction does not run again in a cooked game, and the mesh is not saved.
	EnsureBuilt();
}

#if WITH_EDITOR
void AOceanBathymetry::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	Build();
}
#endif

void AOceanBathymetry::EnsureBuilt()
{
	if (!bBuilt)
	{
		Build();
	}
}

float AOceanBathymetry::GetTerrainHeight(const FVector2f& PositionM) const
{
	const FOceanTerrainSample Sample = SampleTerrain(PositionM);
	return Sample.Ground + UnderstoryRelief * Sample.Understory;
}

FOceanTerrainSample AOceanBathymetry::SampleTerrain(const FVector2f& PositionM) const
{
	FOceanTerrainSample Out = SampleShape(PositionM);
	if (Relief.Contains(PositionM))
	{
		Out.Flow = Relief.SampleFlow(PositionM);
	}
	if (Erosion.Contains(PositionM))
	{
		Out.Erosion = Erosion.Sample(PositionM);
		Out.Ground += Out.Erosion;
	}
	return Out;
}

void AOceanBathymetry::BuildErosion()
{
	Erosion = FOceanHeightfield();
	Relief = FOceanHeightfield();
	if (!bErode)
	{
		return;
	}
	const double StartTime = FPlatformTime::Seconds();
	const FVector2f Center(float(GetActorLocation().X * 0.01), float(GetActorLocation().Y * 0.01));
	const float Extent = IslandRadius + CoastlineVariation + CoastlineDetail + 60.f;

	// 1) The massif's relief, grown at 2 m by uplift against fluvial incision. The uplift follows the analytic massif,
	// unevenly (its noise seeds the ridges and valleys); the beaches and the sea are the base level.
	{
		FOceanHeightfield Grown;
		Grown.CellM = 2.f;
		Grown.Width = Grown.Height = FMath::CeilToInt(2.f * Extent / Grown.CellM) + 1;
		Grown.OriginM = Center - FVector2f(Extent);
		const int32 GrownCount = Grown.Width * Grown.Height;
		Grown.Heights.SetNumUninitialized(GrownCount);
		TArray<float> Uplift, Target;
		Uplift.SetNumUninitialized(GrownCount);
		Target.SetNumUninitialized(GrownCount);
		ParallelFor(Grown.Height, [&](int32 Y)
		{
			for (int32 X = 0; X < Grown.Width; ++X)
			{
				const int32 Index = Y * Grown.Width + X;
				const FVector2f Position = Grown.OriginM + FVector2f(float(X), float(Y)) * Grown.CellM;
				const FVector2f Seeded = Position + FVector2f(Seed * 37.1f, Seed * 11.3f);
				const FOceanTerrainSample Shape = SampleShape(Position);
				Target[Index] = Shape.Hills;
				Uplift[Index] = Shape.Hills > 0.05f
					? 0.05f * (Shape.Hills / HillHeight) * (0.75f + 0.5f * OceanGradientNoise(Seeded / 55.f + 11.3f)) : 0.f;
				Grown.Heights[Index] = Uplift[Index] > 0.f ? 0.002f + 0.01f * OceanGradientNoise(Seeded / 6.f + 4.1f) : 0.f;
			}
		});
		OceanErosion::LandscapeEvolution(Grown, Uplift, LandscapeIterations, 0.5f * ErosionStrength, 0.5f, 0.15f);
		// The grown relief keeps its shape; the analytic massif gives its height.
		float GrownTop = 0.f, TargetTop = 0.f;
		for (int32 Index = 0; Index < GrownCount; ++Index)
		{
			GrownTop = FMath::Max(GrownTop, Grown.Heights[Index]);
			TargetTop = FMath::Max(TargetTop, Target[Index]);
		}
		const float Scale = GrownTop > 0.f ? TargetTop / GrownTop : 0.f;
		for (float& Height : Grown.Heights)
		{
			Height = FMath::Max(Height * Scale, 0.f);
		}
		Relief = MoveTemp(Grown);
		UE_LOG(LogTemp, Display, TEXT("OceanBathymetry: relief grown over %d x %d cells of %.0f m in %.1f s, summit %.1f m"),
			Relief.Width, Relief.Height, Relief.CellM, FPlatformTime::Seconds() - StartTime, TargetTop);
	}
	if (ErosionDroplets <= 0)
	{
		return;
	}

	// 2) A 1 m grid of the shape (with the grown relief), worn by rain and screes.
	FOceanHeightfield Field;
	Field.CellM = 1.f;
	Field.Width = Field.Height = FMath::CeilToInt(2.f * Extent / Field.CellM) + 1;
	Field.OriginM = Center - FVector2f(Extent);
	const int32 Count = Field.Width * Field.Height;
	Field.Heights.SetNumUninitialized(Count);
	// Erodibility: the beach sand barely (the swash reshapes it, not the rain), soil fully, rock little.
	// Angle of repose (tangent): dry sand ~33 degrees, soil ~37, limestone cliffs stay steep; nothing moves under water.
	TArray<float> Erodibility, Talus;
	Erodibility.SetNumUninitialized(Count);
	Talus.SetNumUninitialized(Count);
	ParallelFor(Field.Height, [&](int32 Y)
	{
		for (int32 X = 0; X < Field.Width; ++X)
		{
			const int32 Index = Y * Field.Width + X;
			const FOceanTerrainSample Shape = SampleShape(Field.OriginM + FVector2f(float(X), float(Y)) * Field.CellM);
			Field.Heights[Index] = Shape.Ground;
			Erodibility[Index] = FMath::SmoothStep(BermHeight * 0.7f, BermHeight + 3.f, Shape.Ground) * (1.f - 0.85f * Shape.Rock);
			Talus[Index] = Shape.Ground < -0.3f ? 100.f
				: (Shape.Ground < BermHeight + 1.f ? 0.65f : FMath::Lerp(0.75f, 2.5f, Shape.Rock));
		}
	});
	const TArray<float> Original = Field.Heights;

	// Gullies from the droplets, then screes at the angle of repose (the drainage network is the relief's).
	OceanErosion::FHydraulicSettings Settings;
	Settings.Droplets = ErosionDroplets;
	Settings.CapacityFactor *= ErosionStrength;
	Settings.RainAbove = BermHeight;
	OceanErosion::Hydraulic(Field, Settings, Erodibility, Seed);
	OceanErosion::Thermal(Field, Talus, 30);
	Field.Flow.Reset();

	// Keep the change only.
	float Deepest = 0.f, Highest = 0.f;
	double Moved = 0.0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float Change = Field.Heights[Index] - Original[Index];
		Field.Heights[Index] = Change;
		Deepest = FMath::Min(Deepest, Change);
		Highest = FMath::Max(Highest, Change);
		Moved += FMath::Abs(Change);
	}
	UE_LOG(LogTemp, Display, TEXT("OceanBathymetry: erosion of %d x %d m by %d droplets in %.1f s: carved up to %.2f m, deposited up to %.2f m, %.0f m3 moved"),
		Field.Width, Field.Height, ErosionDroplets, FPlatformTime::Seconds() - StartTime, -Deepest, Highest, Moved * Field.CellM * Field.CellM);

#if WITH_EDITOR
	// Shaded relief before / after and the runoff, to look at the erosion: Saved/Erosion/*.bmp.
	auto WriteRelief = [&](const TCHAR* Name, TFunctionRef<float(int32)> HeightAt, bool bFlow)
	{
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Count);
		const FVector3f Light = FVector3f(-1.f, -1.f, 1.2f).GetSafeNormal();
		for (int32 Y = 0; Y < Field.Height; ++Y)
		{
			for (int32 X = 0; X < Field.Width; ++X)
			{
				const int32 Index = Y * Field.Width + X;
				const float DX = HeightAt(Y * Field.Width + FMath::Min(X + 1, Field.Width - 1)) - HeightAt(Y * Field.Width + FMath::Max(X - 1, 0));
				const float DY = HeightAt(FMath::Min(Y + 1, Field.Height - 1) * Field.Width + X) - HeightAt(FMath::Max(Y - 1, 0) * Field.Width + X);
				const FVector3f Normal = FVector3f(-2.f * DX, -2.f * DY, 2.f * Field.CellM).GetSafeNormal(); // relief x2
				const float Shade = FMath::Clamp(0.25f + 0.85f * FVector3f::DotProduct(Normal, Light), 0.f, 1.f);
				const float H = HeightAt(Index);
				FLinearColor Color = H < 0.f ? FLinearColor(0.2f, 0.45f, 0.6f)
					: (H < BermHeight + 0.5f ? FLinearColor(0.9f, 0.85f, 0.7f) : FMath::Lerp(FLinearColor(0.45f, 0.6f, 0.3f), FLinearColor(0.6f, 0.5f, 0.4f), FMath::Clamp(H / 30.f, 0.f, 1.f)));
				Color *= Shade;
				if (bFlow)
				{
					const float Flow = Relief.SampleFlow(Field.OriginM + FVector2f(float(X), float(Y)) * Field.CellM);
					Color = FMath::Lerp(Color, FLinearColor(0.1f, 0.3f, 0.9f), FMath::SmoothStep(0.45f, 0.8f, Flow));
				}
				Pixels[(Field.Height - 1 - Y) * Field.Width + X] = Color.ToFColor(true);
			}
		}
		const FString Path = FPaths::ProjectSavedDir() / TEXT("Erosion") / Name;
		FFileHelper::CreateBitmap(*Path, Field.Width, Field.Height, Pixels.GetData());
	};
	WriteRelief(TEXT("relief_avant.bmp"), [&](int32 Index) { return Original[Index]; }, false);
	WriteRelief(TEXT("relief_apres.bmp"), [&](int32 Index) { return Original[Index] + Field.Heights[Index]; }, false);
	WriteRelief(TEXT("ruissellement.bmp"), [&](int32 Index) { return Original[Index] + Field.Heights[Index]; }, true);
#endif
	Erosion = MoveTemp(Field);
}

FOceanTerrainSample AOceanBathymetry::SampleShape(const FVector2f& PositionM) const
{
	FOceanTerrainSample Out;
	const FVector2f Center(float(GetActorLocation().X * 0.01), float(GetActorLocation().Y * 0.01));
	const FVector2f Delta = PositionM - Center;
	const float Angle = FMath::Atan2(Delta.Y, Delta.X);

	FRandomStream Random(Seed);
	float Coast = 0.f;
	for (int32 Index = 0; Index < NumCoastHarmonics; ++Index)
	{
		Coast += CoastAmplitudes[Index] * FMath::Sin(CoastFrequencies[Index] * Angle + Random.FRandRange(0.f, 2.f * UE_PI));
	}

	// Signed distance-like coordinate across the coast: negative inland, positive at sea.
	// Fractal gradient noise adds coves and points at every scale to the large bays of the harmonics
	// (its dimension is measured on build, see CoastlineFractalDimension).
	const FVector2f Seeded = PositionM + FVector2f(Seed * 37.1f, Seed * 11.3f);
	const float Across = Delta.Size() - (IslandRadius + CoastlineVariation * Coast)
		+ CoastlineDetail * OceanFractalNoise(Seeded, 120.f, CoastlineOctaves, CoastlinePersistence);
	Out.Inland = -Across;
	if (Across < 0.f)
	{
		const float Inland = -Across;
		// Beach: rises with LandSlope at the waterline and levels off on the berm.
		const float Berm = FMath::Max(BermHeight, 0.1f);
		const float Beach = Berm * (1.f - FMath::Exp(-Inland * LandSlope / Berm));
		// Wooded hills behind beaches of varying width: one massif rising from the back of the beaches to the island's
		// centre, steeper low down and rounded at the top, uneven only at the scale of a few hundred metres. The
		// erosion (BuildErosion) carves it into ridges and valleys; mid-scale noise here would only leave pits and
		// bumps that no water could have shaped.
		// The massif is laid out from the smooth coastline (harmonics only): the fractal detail of the coast would
		// otherwise roughen the whole land with bumps a few metres wide.
		const float BeachWidth = 15.f + 35.f * OceanGradientNoise(Seeded / 90.f + 5.3f);
		const float SmoothInland = IslandRadius + CoastlineVariation * Coast - Delta.Size();
		const float Rise = FMath::Max(SmoothInland - BeachWidth, 0.f);
		const float Massif = (1.f - FMath::Exp(-Rise / 90.f)) * FMath::SmoothStep(0.f, 25.f, Rise);
		const float MassifHeight = Massif * (HillHeight * (0.6f + 0.55f * OceanGradientNoise(Seeded / 160.f + 1.7f))
			+ 2.5f * (OceanGradientNoise(Seeded / 45.f + 9.2f) - 0.5f));
		// Once built, the relief grown from it by landscape evolution (BuildErosion) takes its place.
		const float Hills = Relief.Contains(PositionM) ? Relief.Sample(PositionM) : MassifHeight;
		Out.Hills = MassifHeight;
		// Rocky points: the ground rises steeply right from the water (~45 degrees, shaded as rock by slope) to a
		// low headland that joins the massif: the cliff fades out where the hills are already higher. Added over the
		// whole land, the rock mask raised 4 m terraces along its contours far inland.
		const float Rock = Rockiness * FMath::SmoothStep(0.45f, 0.7f, OceanGradientNoise(Seeded / 70.f + 3.1f));
		const float CliffMask = Rock * FMath::SmoothStep(0.f, 8.f, Inland) * (1.f - FMath::SmoothStep(2.f, 8.f, Hills));
		const float Cliff = 8.f * CliffMask;
		// Coral limestone weathers into a jagged, pitted surface (ironshore): sharp ridges and hollows a few metres
		// across on the rocky points (ridged noise: creases where the noise crosses its middle), not smooth lumps.
		const float Ridges = 1.f - FMath::Abs(2.f * OceanGradientNoise(Seeded / 4.f + 7.7f) - 1.f);
		const float Pits = 1.f - FMath::Abs(2.f * OceanGradientNoise(Seeded / 1.6f + 3.3f) - 1.f);
		const float Jagged = CliffMask * (1.4f * Ridges + 0.5f * Pits - 0.9f);
		const float Ground = Beach + Hills + Cliff + Jagged;
		// Understory: 1.5-3 m of lumpy bush mass behind the beach, under the trees of AOceanIslandFlora, which covers
		// it with shrubs (only UnderstoryRelief of it is terrain).
		// Thinner on the rocky points. Uses the same vegetation line as OceanTerrainSurface (Shaders/Ocean.ush),
		// minus its fine noise, with a gradual edge (a sudden step reads as a dark cliff).
		const float ForestRamp = FMath::SmoothStep(2.f, 6.f, Ground + 1.5f * (OceanValueNoise(PositionM / 40.f) - 0.5f));
		const float Forest = ForestRamp * ForestRamp * (1.f - 0.7f * Rock);
		Out.Ground = Ground;
		Out.Understory = Forest * (1.5f + 1.f * OceanGradientNoise(Seeded / 12.f + 2.2f) + 0.5f * OceanGradientNoise(Seeded / 5.f + 8.1f));
		Out.Rock = Rock;
		return Out;
	}

	// Drops with BeachSlope at the waterline, levels off at ShelfDepth, then the reef drop-off.
	const float Shelf = FMath::Max(ShelfDepth, 0.01f);
	const float Depth = Shelf * (1.f - FMath::Exp(-Across * BeachSlope / Shelf))
		+ DropOffDepth * FMath::SmoothStep(ShelfWidth, ShelfWidth + DropOffWidth, Across);
	Out.Ground = -Depth;
	// Rocky points continue a little under water: boulders on the rocky shores.
	Out.Rock = Rockiness * FMath::SmoothStep(0.45f, 0.7f, OceanGradientNoise(Seeded / 70.f + 3.1f)) * (1.f - FMath::SmoothStep(5.f, 25.f, Across));
	return Out;
}

void AOceanBathymetry::Build()
{
	// First the eroded relief: everything below (shore data, terrain mesh, flora) samples it through SampleTerrain.
	BuildErosion();
	const int32 N = FMath::RoundUpToPowerOfTwo(FMath::Clamp(GridResolution, 64, 1024));
	const float Texel = GridSize / N;
	const FVector2f Center(float(GetActorLocation().X * 0.01), float(GetActorLocation().Y * 0.01));

	TSharedPtr<FOceanShoreData> Data = MakeShared<FOceanShoreData>();
	Data->OriginM = Center - FVector2f(GridSize * 0.5f);
	Data->SizeM = GridSize;
	Data->Resolution = N;
	Data->Texels.SetNumUninitialized(N * N);

	auto TexelCenter = [&](int32 X, int32 Y)
	{
		return Data->OriginM + FVector2f((X + 0.5f) * Texel, (Y + 0.5f) * Texel);
	};

	// Heights.
	TArray<float> Heights;
	Heights.SetNumUninitialized(N * N);
	ParallelFor(N, [&](int32 Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			Heights[Y * N + X] = GetTerrainHeight(TexelCenter(X, Y));
		}
	});

	// Shoreline cells: land and water on either side.
	TArray<bool> IsShore;
	IsShore.SetNumZeroed(N * N);
	for (int32 Y = 0; Y < N; ++Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			const bool bWater = Heights[Y * N + X] < 0.f;
			const bool bRight = X + 1 < N && (Heights[Y * N + X + 1] < 0.f) != bWater;
			const bool bUp = Y + 1 < N && (Heights[(Y + 1) * N + X] < 0.f) != bWater;
			if (bRight || bUp)
			{
				IsShore[Y * N + X] = true;
				if (bRight) { IsShore[Y * N + X + 1] = true; }
				if (bUp) { IsShore[(Y + 1) * N + X] = true; }
			}
		}
	}

	CoastlineFractalDimension = BoxCountingDimension(IsShore, N);
	UE_LOG(LogTemp, Display, TEXT("OceanBathymetry: coastline fractal dimension %.3f (real coasts ~1.15-1.25)"), CoastlineFractalDimension);

	// Signed distance to the shore, positive at sea.
	const TArray<int32> Nearest = NearestSeeds(IsShore, N);
	TArray<float> Distance;
	Distance.SetNumUninitialized(N * N);
	for (int32 Index = 0; Index < N * N; ++Index)
	{
		const float Sign = Heights[Index] < 0.f ? 1.f : -1.f;
		if (Nearest[Index] < 0)
		{
			Distance[Index] = Sign * FOceanShoreData::DeepWaterM;
			continue;
		}
		const float DX = float(Nearest[Index] % N - Index % N);
		const float DY = float(Nearest[Index] / N - Index / N);
		Distance[Index] = Sign * FMath::Sqrt(DX * DX + DY * DY) * Texel;
	}

	const TArray<float> TravelTime = WaveTravelTime(Heights, N, Texel);
	for (int32 Index = 0; Index < N * N; ++Index)
	{
		Data->Texels[Index] = FVector4f(-Heights[Index], Distance[Index], TravelTime[Index], 0.f);
	}

	// Terrain mesh, one vertex per texel, local to the actor (sea level is world Z = 0).
	TArray<FVector> Vertices;
	TArray<FVector> Normals;
	TArray<FProcMeshTangent> Tangents;
	TArray<int32> Triangles;
	Vertices.SetNumUninitialized(N * N);
	Normals.SetNumUninitialized(N * N);
	Tangents.Init(FProcMeshTangent(1.f, 0.f, 0.f), N * N);
	const double ActorZ = GetActorLocation().Z;
	ParallelFor(N, [&](int32 Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			const int32 Index = Y * N + X;
			const FVector2f Position = TexelCenter(X, Y) - Center;
			Vertices[Index] = FVector(Position.X * 100.0, Position.Y * 100.0, Heights[Index] * 100.0 - ActorZ);
			const float DHX = Heights[Y * N + FMath::Min(X + 1, N - 1)] - Heights[Y * N + FMath::Max(X - 1, 0)];
			const float DHY = Heights[FMath::Min(Y + 1, N - 1) * N + X] - Heights[FMath::Max(Y - 1, 0) * N + X];
			Normals[Index] = FVector(-DHX, -DHY, 2.f * Texel).GetSafeNormal();
		}
	});
	// Along the coast the 2.5 m triangles show: the swash sheet and the waterline cut them along long straight
	// edges; on land they would smooth away the gullies of the erosion. Chunks of the grid that hold the coast or
	// land (ground above -CoastBand) are rebuilt Subdivision times finer (section 1); the sea floor keeps one vertex
	// per texel (section 0).
	constexpr int32 ChunkCells = 16;
	constexpr int32 Subdivision = 5;
	constexpr float CoastBand = 3.f;
	const int32 Chunks = FMath::DivideAndRoundUp(N - 1, ChunkCells);
	TArray<bool> FineChunk;
	FineChunk.SetNumZeroed(Chunks * Chunks);
	for (int32 CY = 0; CY < Chunks; ++CY)
	{
		for (int32 CX = 0; CX < Chunks; ++CX)
		{
			bool bCoast = false;
			for (int32 Y = CY * ChunkCells; Y <= FMath::Min((CY + 1) * ChunkCells, N - 1) && !bCoast; ++Y)
			{
				for (int32 X = CX * ChunkCells; X <= FMath::Min((CX + 1) * ChunkCells, N - 1); ++X)
				{
					if (Heights[Y * N + X] > -CoastBand)
					{
						bCoast = true;
						break;
					}
				}
			}
			FineChunk[CY * Chunks + CX] = bCoast;
		}
	}
	auto IsFine = [&](int32 CX, int32 CY)
	{
		return CX >= 0 && CY >= 0 && CX < Chunks && CY < Chunks && FineChunk[CY * Chunks + CX];
	};

	Triangles.Reserve((N - 1) * (N - 1) * 6);
	for (int32 Y = 0; Y + 1 < N; ++Y)
	{
		for (int32 X = 0; X + 1 < N; ++X)
		{
			if (IsFine(X / ChunkCells, Y / ChunkCells))
			{
				continue;
			}
			const int32 V00 = Y * N + X;
			const int32 V10 = V00 + 1;
			const int32 V01 = V00 + N;
			const int32 V11 = V01 + 1;
			Triangles.Append({ V00, V01, V10, V10, V01, V11 });
		}
	}
	Terrain->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, TArray<FVector2D>(), TArray<FLinearColor>(), Tangents, false);

	// Fine coast chunks. Edges shared with a coarse chunk follow the coarse edge (linear between its vertices), so
	// the two meshes meet without cracks; edges between fine chunks sample the same heights on both sides.
	TArray<FVector> FineVertices;
	TArray<FVector> FineNormals;
	TArray<int32> FineTriangles;
	const float FineStep = Texel / Subdivision;
	for (int32 CY = 0; CY < Chunks; ++CY)
	{
		for (int32 CX = 0; CX < Chunks; ++CX)
		{
			if (!IsFine(CX, CY))
			{
				continue;
			}
			const int32 X0 = CX * ChunkCells;
			const int32 Y0 = CY * ChunkCells;
			const int32 X1 = FMath::Min(X0 + ChunkCells, N - 1);
			const int32 Y1 = FMath::Min(Y0 + ChunkCells, N - 1);
			const int32 W = (X1 - X0) * Subdivision + 1;
			const int32 H = (Y1 - Y0) * Subdivision + 1;
			const FVector2f Origin = TexelCenter(X0, Y0);

			// Heights with a one-sample ring around the chunk, for the normals.
			const int32 RW = W + 2;
			TArray<float> Local;
			Local.SetNumUninitialized(RW * (H + 2));
			ParallelFor(H + 2, [&](int32 J)
			{
				for (int32 I = 0; I < RW; ++I)
				{
					Local[J * RW + I] = GetTerrainHeight(Origin + FVector2f((I - 1) * FineStep, (J - 1) * FineStep));
				}
			});
			const auto CoarseLerp = [&](int32 XA, int32 YA, int32 XB, int32 YB, int32 Step)
			{
				const int32 Cell = Step / Subdivision;
				const float T = float(Step % Subdivision) / Subdivision;
				const float A = Heights[(YA + (YB - YA) * Cell) * N + XA + (XB - XA) * Cell];
				const float B = T > 0.f ? Heights[(YA + (YB - YA) * (Cell + 1)) * N + XA + (XB - XA) * (Cell + 1)] : A;
				return FMath::Lerp(A, B, T);
			};
			for (int32 J = 0; J < H; ++J)
			{
				if (!IsFine(CX - 1, CY)) { Local[(J + 1) * RW + 1] = CoarseLerp(X0, Y0, X0, Y0 + 1, J); }
				if (!IsFine(CX + 1, CY)) { Local[(J + 1) * RW + W] = CoarseLerp(X1, Y0, X1, Y0 + 1, J); }
			}
			for (int32 I = 0; I < W; ++I)
			{
				if (!IsFine(CX, CY - 1)) { Local[RW + I + 1] = CoarseLerp(X0, Y0, X0 + 1, Y0, I); }
				if (!IsFine(CX, CY + 1)) { Local[H * RW + I + 1] = CoarseLerp(X0, Y1, X0 + 1, Y1, I); }
			}

			const int32 Base = FineVertices.Num();
			for (int32 J = 0; J < H; ++J)
			{
				for (int32 I = 0; I < W; ++I)
				{
					const int32 L = (J + 1) * RW + I + 1;
					const FVector2f Position = Origin + FVector2f(I * FineStep, J * FineStep) - Center;
					FineVertices.Add(FVector(Position.X * 100.0, Position.Y * 100.0, Local[L] * 100.0 - ActorZ));
					FineNormals.Add(FVector(-(Local[L + 1] - Local[L - 1]), -(Local[L + RW] - Local[L - RW]), 2.f * FineStep).GetSafeNormal());
				}
			}
			for (int32 J = 0; J + 1 < H; ++J)
			{
				for (int32 I = 0; I + 1 < W; ++I)
				{
					const int32 V00 = Base + J * W + I;
					const int32 V10 = V00 + 1;
					const int32 V01 = V00 + W;
					const int32 V11 = V01 + 1;
					FineTriangles.Append({ V00, V01, V10, V10, V01, V11 });
				}
			}
		}
	}
	if (FineVertices.Num() > 0)
	{
		TArray<FProcMeshTangent> FineTangents;
		FineTangents.Init(FProcMeshTangent(1.f, 0.f, 0.f), FineVertices.Num());
		Terrain->CreateMeshSection_LinearColor(1, FineVertices, FineTriangles, FineNormals, TArray<FVector2D>(), TArray<FLinearColor>(), FineTangents, false);
	}
	else
	{
		Terrain->ClearMeshSection(1);
	}
	UE_LOG(LogTemp, Display, TEXT("OceanBathymetry: coast mesh %d vertices at %.2f m (%d chunks)"),
		FineVertices.Num(), FineStep, FineChunk.FilterByPredicate([](bool bFine) { return bFine; }).Num());
	BuildShoreTexture(*Data);
	ShoreData = Data;
	BuildFootprintTexture();
	if (UMaterialInterface* Material = TerrainMaterial.LoadSynchronous())
	{
		// A dynamic instance carries the simulated footprints and the coastal data (wave ripples) to the sand shader.
		if (!TerrainMaterialInstance || TerrainMaterialInstance->Parent != Material)
		{
			TerrainMaterialInstance = UMaterialInstanceDynamic::Create(Material, this);
		}
		if (FootprintTexture)
		{
			TerrainMaterialInstance->SetTextureParameterValue(TEXT("SandFootprints"), FootprintTexture);
			TerrainMaterialInstance->SetScalarParameterValue(TEXT("SandFootprintTileSize"), FootprintTileSize);
		}
		if (ShoreTexture)
		{
			TerrainMaterialInstance->SetTextureParameterValue(TEXT("ShoreMap"), ShoreTexture);
			TerrainMaterialInstance->SetVectorParameterValue(TEXT("ShoreMapOrigin"), FLinearColor(Data->OriginM.X, Data->OriginM.Y, 0.f, 0.f));
			TerrainMaterialInstance->SetScalarParameterValue(TEXT("ShoreMapSize"), Data->SizeM);
		}
		for (int32 Section = 0; Section < Terrain->GetNumSections(); ++Section)
		{
			Terrain->SetMaterial(Section, TerrainMaterialInstance);
		}
	}
	bBuilt = true;
}

void AOceanBathymetry::BuildFootprintTexture()
{
	if (FootprintTexture && BuiltFootprintTrails == FootprintTrails && BuiltFootprintTileSize == FootprintTileSize)
	{
		return; // ~1 s of simulation: only redone when its settings change
	}
	FOceanSandFootprintSettings Settings;
	Settings.Trails = FootprintTrails;
	Settings.TileSizeM = FootprintTileSize;
	Settings.Seed = Seed;
	const double Start = FPlatformTime::Seconds();
	const TArray<FVector2f> Texels = OceanSimulateSandFootprints(Settings);
	const int32 N = Settings.Resolution;
	float MinHeight = 0.f, MaxHeight = 0.f, Trampled = 0.f;
	for (const FVector2f& Texel : Texels)
	{
		MinHeight = FMath::Min(MinHeight, Texel.X);
		MaxHeight = FMath::Max(MaxHeight, Texel.X);
		Trampled += Texel.Y > 0.5f ? 1.f : 0.f;
	}
	UE_LOG(LogTemp, Display, TEXT("OceanBathymetry: %d footprint trails in %.2f s, height %.1f..%.1f cm, %.1f%% trampled"),
		FootprintTrails, FPlatformTime::Seconds() - Start, MinHeight * 100.f, MaxHeight * 100.f, 100.f * Trampled / FMath::Max(Texels.Num(), 1));
#if WITH_EDITOR
	// Shaded relief of the tile (Saved/SandFootprints.pgm), to judge the simulation without rendering the beach.
	{
		TArray<uint8> Image;
		const FString Header = FString::Printf(TEXT("P5 %d %d 255\n"), N, N);
		Image.Append(reinterpret_cast<const uint8*>(TCHAR_TO_ANSI(*Header)), Header.Len());
		const float Cell = FootprintTileSize / N;
		for (int32 Y = 0; Y < N; ++Y)
		{
			for (int32 X = 0; X < N; ++X)
			{
				const float DX = (Texels[Y * N + (X + 1) % N].X - Texels[Y * N + X].X) / Cell;
				const float DY = (Texels[((Y + 1) % N) * N + X].X - Texels[Y * N + X].X) / Cell;
				const FVector3f Normal = FVector3f(-DX, -DY, 1.f).GetSafeNormal();
				const float Light = FMath::Max(FVector3f::DotProduct(Normal, FVector3f(-0.6f, -0.3f, 0.75f).GetSafeNormal()), 0.f);
				Image.Add(uint8(FMath::Clamp(Light * 255.f, 0.f, 255.f)));
			}
		}
		FFileHelper::SaveArrayToFile(Image, *(FPaths::ProjectSavedDir() / TEXT("SandFootprints.pgm")));
	}
#endif

	UTexture2D* Texture = UTexture2D::CreateTransient(N, N, PF_G32R32F);
	if (!Texture)
	{
		return;
	}
	Texture->SRGB = false;
	Texture->Filter = TF_Bilinear;
	Texture->AddressX = TA_Wrap; // periodic tile
	Texture->AddressY = TA_Wrap;
	Texture->CompressionSettings = TC_HDR;
	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* Destination = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Destination, Texels.GetData(), int64(Texels.Num()) * sizeof(FVector2f));
	Mip.BulkData.Unlock();
	Texture->UpdateResource();
	FootprintTexture = Texture;
	BuiltFootprintTrails = FootprintTrails;
	BuiltFootprintTileSize = FootprintTileSize;
}

void AOceanBathymetry::BuildShoreTexture(const FOceanShoreData& Data)
{
	// A new texture on every build: a transient texture's CPU copy may be released once uploaded.
	const int32 N = Data.Resolution;
	UTexture2D* Texture = UTexture2D::CreateTransient(N, N, PF_A32B32G32R32F);
	if (!Texture)
	{
		return;
	}
	Texture->SRGB = false;
	Texture->Filter = TF_Bilinear;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->CompressionSettings = TC_HDR;

	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* Destination = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Destination, Data.Texels.GetData(), int64(Data.Texels.Num()) * sizeof(FVector4f));
	Mip.BulkData.Unlock();
	Texture->UpdateResource();
	ShoreTexture = Texture;
}
