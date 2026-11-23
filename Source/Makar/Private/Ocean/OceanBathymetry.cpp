#include "Ocean/OceanBathymetry.h"

#include "Async/ParallelFor.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#if WITH_EDITOR
#include "LevelEditorViewport.h"
#endif
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Crc.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Materials/MaterialInterface.h"
#include "Ocean/OceanMeshComponent.h"
#include "Ocean/OceanSandFootprints.h"
#include "Ocean/OceanSandDeformation.h"
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
	// Tick picks each land tile's level of detail from the viewer's distance.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.1f;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	// UOceanMeshComponent does not save its geometry: the terrain is rebuilt on load.
	Terrain = CreateDefaultSubobject<UOceanMeshComponent>(TEXT("Terrain"));
	Terrain->SetupAttachment(RootComponent);
	Terrain->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	Spray = CreateDefaultSubobject<UOceanMeshComponent>(TEXT("Spray"));
	Spray->SetupAttachment(RootComponent);
	Spray->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Spray->SetCastShadow(false);

	SandDeformation = CreateDefaultSubobject<UOceanSandDeformation>(TEXT("SandDeformation"));

	TerrainMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/M_Seabed.M_Seabed")));
	SprayMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/M_OceanSpray.M_OceanSpray")));
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
	// The sand deformation reads the analytic ground (its offsets are relative to it) and shows through the terrain material.
	SandDeformation->GroundHeight = [this](const FVector2f& PositionM) { return SampleTerrain(PositionM).Ground; };
	SandDeformation->OnWindowChanged.AddUObject(this, &AOceanBathymetry::UpdateSandDeformationParameters);
}

#if WITH_EDITOR
void AOceanBathymetry::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	Build();
}
#endif

float AOceanBathymetry::GetGroundHeightAt(const FVector& WorldLocation) const
{
	const float Ground = SampleTerrain(FVector2f(float(WorldLocation.X * 0.01), float(WorldLocation.Y * 0.01))).Ground * 100.f;
	return Ground + (SandDeformation ? SandDeformation->GetHeightOffsetAt(WorldLocation) : 0.f);
}

void AOceanBathymetry::UpdateSandDeformationParameters()
{
	UE_LOG(LogTemp, Verbose, TEXT("OceanBathymetry: sand deformation window %s, material %s"),
		SandDeformation ? *SandDeformation->GetWindow().ToString() : TEXT("none"), TerrainMaterialInstance ? *TerrainMaterialInstance->GetName() : TEXT("none"));
	if (TerrainMaterialInstance && SandDeformation && SandDeformation->GetTexture())
	{
		const FVector3f Window = SandDeformation->GetWindow();
		TerrainMaterialInstance->SetTextureParameterValue(TEXT("SandDeformation"), SandDeformation->GetTexture());
		TerrainMaterialInstance->SetVectorParameterValue(TEXT("SandDeformationWindow"), FLinearColor(Window.X, Window.Y, Window.Z, 0.f));
	}
}

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
	const FString CachePath = ErosionCachePath();
	if (LoadErosionCache(CachePath))
	{
		UE_LOG(LogTemp, Display, TEXT("OceanBathymetry: erosion loaded from %s"), *CachePath);
		return;
	}
	const double StartTime = FPlatformTime::Seconds();
	const FVector2f Center(float(GetActorLocation().X * 0.01), float(GetActorLocation().Y * 0.01));
	const float Extent = IslandRadius + CoastlineVariation + CoastlineDetail + 60.f;

	// 1) The massif's relief, grown at 2 m by uplift against fluvial incision. The uplift follows the analytic massif,
	// unevenly (its noise seeds the ridges and valleys); the beaches and the sea are the base level.
	{
		FOceanHeightfield Grown;
		Grown.CellM = 3.f;
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
				const FOceanTerrainSample Shape = SampleShape(Position, false);
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
			const FOceanTerrainSample Shape = SampleShape(Field.OriginM + FVector2f(float(X), float(Y)) * Field.CellM, false);
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
	SaveErosionCache(CachePath);
}

FString AOceanBathymetry::ErosionCachePath() const
{
	// Every setting the erosion depends on, and a version to bump whenever SampleShape or OceanErosion change.
	const FString Key = FString::Printf(TEXT("v6|%.2f|%.2f|%.2f|%.2f|%.2f|%.3f|%d|%.3f|%.3f|%.3f|%.3f|%.3f|%.2f|%.2f|%.3f|%d|%.3f|%d|%d|%.1f|%.1f|%.3f|%.3f|%.2f|%.2f|%.2f"),
		IslandRadius, IslandAspect, IslandAngle, CoastlineVariation, CoastlineDetail, CoastlinePersistence, CoastlineOctaves,
		SandyCoastDetail, MassifWidth, CliffHeight, BermHeight, LandSlope, HillHeight, Rockiness, ErosionStrength,
		ErosionDroplets, UnderstoryRelief, LandscapeIterations, Seed, GetActorLocation().X, GetActorLocation().Y,
		BeachSlope, ShelfDepth, ShelfWidth, DropOffDepth, DropOffWidth);
	return FPaths::ProjectSavedDir() / TEXT("OceanCache") / FString::Printf(TEXT("erosion_%08x.bin"), FCrc::StrCrc32(*Key));
}

namespace
{
	void SerializeField(FArchive& Archive, FOceanHeightfield& Field)
	{
		Archive << Field.OriginM << Field.CellM << Field.Width << Field.Height << Field.Heights << Field.Flow;
	}
}

bool AOceanBathymetry::LoadErosionCache(const FString& Path)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
	{
		return false;
	}
	FMemoryReader Reader(Bytes);
	FOceanHeightfield LoadedRelief, LoadedErosion;
	SerializeField(Reader, LoadedRelief);
	SerializeField(Reader, LoadedErosion);
	if (Reader.IsError() || LoadedRelief.Heights.Num() != LoadedRelief.Width * LoadedRelief.Height)
	{
		return false;
	}
	Relief = MoveTemp(LoadedRelief);
	Erosion = MoveTemp(LoadedErosion);
	return true;
}

void AOceanBathymetry::SaveErosionCache(const FString& Path) const
{
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	FOceanHeightfield R = Relief, E = Erosion;
	SerializeField(Writer, R);
	SerializeField(Writer, E);
	FFileHelper::SaveArrayToFile(Bytes, *Path);
}

FOceanTerrainSample AOceanBathymetry::SampleShape(const FVector2f& PositionM, bool bFineDetail) const
{
	FOceanTerrainSample Out;
	const FVector2f Center(float(GetActorLocation().X * 0.01), float(GetActorLocation().Y * 0.01));
	const FVector2f Delta = PositionM - Center;

	// An elongated island: an ellipse (IslandRadius along IslandAngle, IslandRadius / IslandAspect across), whose
	// radius the harmonics bend into bays and headlands. Its radial coordinate is divided by its gradient so that it
	// reads as a distance to the coast in metres on every side.
	const float AxisAngle = FMath::DegreesToRadians(IslandAngle);
	const float CosAxis = FMath::Cos(AxisAngle);
	const float SinAxis = FMath::Sin(AxisAngle);
	const FVector2f Local(Delta.X * CosAxis + Delta.Y * SinAxis, -Delta.X * SinAxis + Delta.Y * CosAxis);
	const float Aspect = FMath::Max(IslandAspect, 1.f);
	const FVector2f Scaled(Local.X, Local.Y * Aspect);
	const float Radial = Scaled.Size();
	const float Stretch = Radial > 1.f ? FVector2f(Scaled.X, Scaled.Y * Aspect).Size() / Radial : 1.f;
	const float Angle = FMath::Atan2(Scaled.Y, Scaled.X);

	FRandomStream Random(Seed);
	float Coast = 0.f;
	for (int32 Index = 0; Index < NumCoastHarmonics; ++Index)
	{
		Coast += CoastAmplitudes[Index] * FMath::Sin(CoastFrequencies[Index] * Angle + Random.FRandRange(0.f, 2.f * UE_PI));
	}

	// Signed distance-like coordinate across the coast: negative inland, positive at sea.
	// Fractal gradient noise adds coves and points at every scale to the large bays of the harmonics (its dimension is
	// measured on build, see CoastlineFractalDimension), but mostly on the rocky shores: beaches are smooth arcs.
	const FVector2f Seeded = PositionM + FVector2f(Seed * 37.1f, Seed * 11.3f);
	const float Rock = RockMask(Seeded);
	const float Rocky = Rockiness > 0.f ? FMath::Clamp(Rock / Rockiness, 0.f, 1.f) : 0.f;
	const float SmoothAcross = (Radial - (IslandRadius + CoastlineVariation * Coast)) / Stretch;
	const float Across = SmoothAcross
		+ CoastlineDetail * FMath::Lerp(SandyCoastDetail, 1.f, Rocky) * OceanFractalNoise(Seeded, 120.f, CoastlineOctaves, CoastlinePersistence);
	Out.Inland = -Across;
	if (Across < 0.f)
	{
		const float Inland = -Across;
		// Beach: rises with LandSlope at the waterline and levels off on the berm.
		const float Berm = FMath::Max(BermHeight, 0.1f);
		const float Beach = Berm * (1.f - FMath::Exp(-Inland * LandSlope / Berm));
		// Wooded hills behind beaches of varying width: one massif rising from the back of the beaches to the island's
		// spine, steeper low down and rounded at the top, uneven only at the scale of a few hundred metres (summits and
		// saddles along the long axis). The erosion (BuildErosion) carves it into ridges and valleys; mid-scale noise
		// here would only leave pits and bumps that no water could have shaped.
		// The massif is laid out from the smooth coastline (harmonics only): the fractal detail of the coast would
		// otherwise roughen the whole land with bumps a few metres wide.
		const float BeachWidth = 20.f + 45.f * OceanGradientNoise(Seeded / 150.f + 5.3f);
		const float Rise = FMath::Max(-SmoothAcross - BeachWidth, 0.f);
		const float Massif = (1.f - FMath::Exp(-Rise / FMath::Max(MassifWidth, 10.f))) * FMath::SmoothStep(0.f, 40.f, Rise);
		const float Summits = 0.45f + 0.75f * OceanGradientNoise(FVector2f(Local.X / 420.f, Local.Y / 900.f) + FVector2f(Seed * 0.31f, 1.7f));
		// Lower behind the rocky points, so their cliffs stand as flat-topped headlands rather than the flanks of a hill.
		const float Headland = 1.f - 0.7f * Rocky * (1.f - FMath::SmoothStep(60.f, 220.f, Inland));
		const float MassifHeight = Massif * Headland * (HillHeight * Summits + 6.f * (OceanGradientNoise(Seeded / 110.f + 9.2f) - 0.5f));
		// Once built, the relief grown from it by landscape evolution (BuildErosion) takes its place.
		const float Hills = Relief.Contains(PositionM) ? Relief.Sample(PositionM) : MassifHeight;
		Out.Hills = MassifHeight;
		// Rocky points: the ground rises steeply right from the water (~70 degrees, shaded as rock by slope) to a
		// headland that joins the massif: the cliff fades out where the hills are already higher. Added over the
		// whole land, the rock mask raised terraces along its contours far inland.
		// The headland is a plateau: the cliff rises almost sheer to its top, which stays flat (scrub, agaves) until the
		// hills behind rise above it (a maximum, not a sum: added to the hills, the cliff made round domes).
		const float LocalCliff = CliffHeight * (0.6f + 0.8f * OceanGradientNoise(Seeded / 140.f + 6.1f));
		// Measured from a smoother outline (the coast's first octaves only): following every metre-scale zigzag of the
		// rocky shore, a sheer cliff this tall stood up as a field of needles.
		const float CliffInland = -(SmoothAcross + CoastlineDetail * FMath::Lerp(SandyCoastDetail, 1.f, Rocky)
			* OceanFractalNoise(Seeded, 120.f, FMath::Min(CoastlineOctaves, 3), CoastlinePersistence));
		const float CliffMask = Rock * FMath::SmoothStep(0.3f, 1.f + 0.15f * LocalCliff, CliffInland); // in front: a low rock shelf
		const float Cliff = LocalCliff * CliffMask;
		float Ground = Beach + FMath::Max(Hills, Cliff);
		if (bFineDetail)
		{
			// Bedding: the limestone lies in near-horizontal layers ~0.9 m thick, which the sea and the rain etch into
			// ledges on the cliff faces: flat benches, steep risers; the beds dip and wander a little.
			const float FaceTop = 3.f + 0.35f * LocalCliff;
			const float Face = Rock * FMath::SmoothStep(0.3f, 1.5f, Inland) * (1.f - FMath::SmoothStep(FaceTop, FaceTop + 3.f, Inland));
			const float Bed = 0.9f;
			const float Layer = (Beach + Cliff) / Bed + 0.3f * (OceanGradientNoise(Seeded / 9.f + 4.4f) - 0.5f);
			const float Benched = (FMath::FloorToFloat(Layer) + FMath::SmoothStep(0.6f, 1.f, FMath::Frac(Layer))) * Bed;
			// Coral limestone weathers into a jagged, pitted surface (ironshore): sharp ridges and hollows a few metres
			// across on the rocky points (ridged noise: creases where the noise crosses its middle), not smooth lumps.
			const float Ridges = 1.f - FMath::Abs(2.f * OceanGradientNoise(Seeded / 4.f + 7.7f) - 1.f);
			const float Pits = 1.f - FMath::Abs(2.f * OceanGradientNoise(Seeded / 1.6f + 3.3f) - 1.f);
			// Collapsed blocks and buttresses along the cliff foot and on the faces, metres wide.
			const float Blocks = 1.f - FMath::Abs(2.f * OceanGradientNoise(Seeded / 7.f + 12.9f) - 1.f);
			// (Stronger, the ridges and blocks stood up as spires on the faces: tsingy, not Antillean limestone.)
			Ground += Face * (Benched - Layer * Bed) + CliffMask * (0.7f * Ridges + 0.4f * Pits - 0.5f)
				+ Face * 0.6f * (Blocks - 0.5f);
		}
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
	Out.Rock = Rock * (1.f - FMath::SmoothStep(5.f, 25.f, Across));
	return Out;
}

float AOceanBathymetry::RockMask(const FVector2f& Seeded) const
{
	// Where the coast is rocky: a slow noise, its domain warped so the rocky stretches have ragged outlines rather than
	// the round blobs of a plain noise threshold.
	const FVector2f Warp = 30.f * FVector2f(OceanGradientNoise(Seeded / 70.f + 2.1f) - 0.5f, OceanGradientNoise(Seeded / 70.f + 8.4f) - 0.5f);
	return Rockiness * FMath::SmoothStep(0.45f, 0.68f, OceanGradientNoise((Seeded + Warp) / 160.f + 3.1f));
}

void AOceanBathymetry::Build()
{
	// First the eroded relief: everything below (shore data, terrain mesh, flora) samples it through SampleTerrain.
	BuildErosion();
	const int32 N = FMath::RoundUpToPowerOfTwo(FMath::Clamp(GridResolution, 64, 2048));
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

	// Terrain meshes (local to the actor; sea level is world Z = 0): the sea floor on a coarse grid, the land and the
	// coast in finer chunks grouped into tiles (BuildLandTiles).
	BuildLandTiles(Heights, N, Texel, 2);
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
		for (UOceanMeshComponent* Tile : LandTiles)
		{
			Tile->SetMaterial(0, TerrainMaterialInstance);
		}
		UpdateSandDeformationParameters();
	}
	BuildSprayCurtain(*Data);
	bBuilt = true;
}

void AOceanBathymetry::BuildLandTiles(const TArray<float>& Heights, int32 N, float Texel, int32 Stride)
{
	const double StartTime = FPlatformTime::Seconds();
	const FVector2f Center(float(GetActorLocation().X * 0.01), float(GetActorLocation().Y * 0.01));
	const FVector2f GridOrigin = Center - FVector2f(GridSize * 0.5f);
	const double ActorZ = GetActorLocation().Z;
	auto TexelCenter = [&](int32 X, int32 Y) { return GridOrigin + FVector2f((X + 0.5f) * Texel, (Y + 0.5f) * Texel); };

	// Coarse grid: one vertex every Stride texels (the deep sea floor needs no more).
	const int32 M = (N - 1) / Stride + 1;
	const float Step = Texel * Stride;
	auto CoarseIndex = [&](int32 I, int32 J) { return (J * Stride) * N + I * Stride; };

	// Chunks of 16 coarse cells, each at its own resolution: 0.5 m where the coast runs (the swash sheet and the
	// waterline cut coarse triangles along long straight edges; the cliffs need their ledges), 1 m over the land (the
	// erosion's gullies), coarse under the sea.
	constexpr int32 ChunkCells = 16;
	constexpr float CoastBand = 3.f;
	constexpr float CoastTop = 10.f;
	const int32 CoastSubdivision = FMath::Max(1, FMath::RoundToInt(Step / 0.5f));
	const int32 LandSubdivision = FMath::Max(1, FMath::RoundToInt(Step / 1.f));
	const int32 Chunks = FMath::DivideAndRoundUp(M - 1, ChunkCells);
	TArray<int32> Level;
	Level.Init(1, Chunks * Chunks);
	ParallelFor(Chunks, [&](int32 CY)
	{
		for (int32 CX = 0; CX < Chunks; ++CX)
		{
			float Low = MAX_flt, High = -MAX_flt;
			const int32 X0 = CX * ChunkCells * Stride, X1 = FMath::Min((CX + 1) * ChunkCells, M - 1) * Stride;
			const int32 Y0 = CY * ChunkCells * Stride, Y1 = FMath::Min((CY + 1) * ChunkCells, M - 1) * Stride;
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					Low = FMath::Min(Low, Heights[Y * N + X]);
					High = FMath::Max(High, Heights[Y * N + X]);
				}
			}
			Level[CY * Chunks + CX] = High > -CoastBand ? (Low < CoastTop ? CoastSubdivision : LandSubdivision) : 1;
		}
	});
	auto LevelAt = [&](int32 CX, int32 CY) { return (CX >= 0 && CY >= 0 && CX < Chunks && CY < Chunks) ? Level[CY * Chunks + CX] : 1; };

	// The coarse sea floor.
	{
		TArray<FVector> Vertices;
		TArray<FVector> Normals;
		TArray<FProcMeshTangent> Tangents;
		TArray<int32> Triangles;
		Vertices.SetNumUninitialized(M * M);
		Normals.SetNumUninitialized(M * M);
		Tangents.Init(FProcMeshTangent(1.f, 0.f, 0.f), M * M);
		ParallelFor(M, [&](int32 J)
		{
			for (int32 I = 0; I < M; ++I)
			{
				const FVector2f Position = TexelCenter(I * Stride, J * Stride) - Center;
				Vertices[J * M + I] = FVector(Position.X * 100.0, Position.Y * 100.0, Heights[CoarseIndex(I, J)] * 100.0 - ActorZ);
				const float DHX = Heights[CoarseIndex(FMath::Min(I + 1, M - 1), J)] - Heights[CoarseIndex(FMath::Max(I - 1, 0), J)];
				const float DHY = Heights[CoarseIndex(I, FMath::Min(J + 1, M - 1))] - Heights[CoarseIndex(I, FMath::Max(J - 1, 0))];
				Normals[J * M + I] = FVector(-DHX, -DHY, 2.f * Step).GetSafeNormal();
			}
		});
		Triangles.Reserve((M - 1) * (M - 1) * 6);
		for (int32 J = 0; J + 1 < M; ++J)
		{
			for (int32 I = 0; I + 1 < M; ++I)
			{
				if (LevelAt(I / ChunkCells, J / ChunkCells) > 1)
				{
					continue;
				}
				const int32 V00 = J * M + I;
				Triangles.Append({ V00, V00 + M, V00 + 1, V00 + 1, V00 + M, V00 + M + 1 });
			}
		}
		Terrain->ClearAllMeshSections();
		Terrain->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, TArray<FVector2D>(), TArray<FLinearColor>(), Tangents, false);
	}

	// The tiles (4 x 4 chunks, 256 m at the default grid): rebuilt from scratch.
	for (UOceanMeshComponent* Tile : LandTiles)
	{
		if (Tile)
		{
			Tile->DestroyComponent();
		}
	}
	LandTiles.Reset();
	{
		TInlineComponentArray<UOceanMeshComponent*> Leftovers(this);
		for (UOceanMeshComponent* Component : Leftovers)
		{
			if (Component && Component->ComponentHasTag(TEXT("LandTile")))
			{
				Component->DestroyComponent();
			}
		}
	}
	constexpr int32 TileChunks = 4;
	const int32 Tiles = FMath::DivideAndRoundUp(Chunks, TileChunks);
	auto TileOf = [&](int32 CX, int32 CY) { return (CY / TileChunks) * Tiles + CX / TileChunks; };
	struct FTileMesh
	{
		TArray<FVector> Vertices;
		TArray<FVector> Normals;
		TArray<int32> Triangles;
	};

	// Three levels of detail per tile: full resolution, then half, then a quarter (never coarser than the sea floor).
	// Within a level, chunks stitch to coarser neighbours; tiles shown at different levels meet over skirts hanging
	// from their edges (both ways, so the crack is filled whichever side is lower).
	constexpr int32 NumLods = 3;
	constexpr int32 LodDivisors[NumLods] = { 1, 2, 4 };
	constexpr float SkirtDepth = 8.f;
	TArray<TArray<FTileMesh>> LodMeshes;
	LodMeshes.SetNum(NumLods);
	int64 VertexCounts[NumLods] = {};
	int32 CoastChunks = 0, LandChunks = 0;
	for (int32 Lod = 0; Lod < NumLods; ++Lod)
	{
		TArray<FTileMesh>& TileMeshes = LodMeshes[Lod];
		TileMeshes.SetNum(Tiles * Tiles);
		auto LodLevelAt = [&](int32 CX, int32 CY)
		{
			const int32 S = LevelAt(CX, CY);
			return S > 1 ? FMath::Max(1, S / LodDivisors[Lod]) : 1;
		};
		for (int32 CY = 0; CY < Chunks; ++CY)
		{
			for (int32 CX = 0; CX < Chunks; ++CX)
			{
				if (LevelAt(CX, CY) <= 1)
				{
					continue;
				}
				if (Lod == 0)
				{
					(LevelAt(CX, CY) == CoastSubdivision ? CoastChunks : LandChunks) += 1;
				}
				const int32 S = LodLevelAt(CX, CY);
				const int32 X0 = CX * ChunkCells;
				const int32 Y0 = CY * ChunkCells;
				const int32 X1 = FMath::Min(X0 + ChunkCells, M - 1);
				const int32 Y1 = FMath::Min(Y0 + ChunkCells, M - 1);
				const int32 W = (X1 - X0) * S + 1;
				const int32 H = (Y1 - Y0) * S + 1;
				const float FineStep = Step / S;
				const FVector2f Origin = TexelCenter(X0 * Stride, Y0 * Stride);

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
				// Edges shared with a coarser chunk follow its edge (linear between its vertices, which this chunk's grid
				// also holds), so the meshes meet without cracks.
				auto Stitch = [&](int32 NeighbourLevel, int32 Count, TFunctionRef<float&(int32)> At)
				{
					if (NeighbourLevel >= S)
					{
						return;
					}
					const int32 K = S / FMath::Max(NeighbourLevel, 1);
					for (int32 I = 0; I < Count; ++I)
					{
						const int32 I0 = (I / K) * K;
						if (I != I0 && I0 + K < Count)
						{
							At(I) = FMath::Lerp(At(I0), At(I0 + K), float(I - I0) / K);
						}
					}
				};
				Stitch(LodLevelAt(CX - 1, CY), H, [&](int32 J) -> float& { return Local[(J + 1) * RW + 1]; });
				Stitch(LodLevelAt(CX + 1, CY), H, [&](int32 J) -> float& { return Local[(J + 1) * RW + W]; });
				Stitch(LodLevelAt(CX, CY - 1), W, [&](int32 I) -> float& { return Local[RW + I + 1]; });
				Stitch(LodLevelAt(CX, CY + 1), W, [&](int32 I) -> float& { return Local[H * RW + I + 1]; });

				FTileMesh& Mesh = TileMeshes[TileOf(CX, CY)];
				const int32 Base = Mesh.Vertices.Num();
				for (int32 J = 0; J < H; ++J)
				{
					for (int32 I = 0; I < W; ++I)
					{
						const int32 L = (J + 1) * RW + I + 1;
						const FVector2f Position = Origin + FVector2f(I * FineStep, J * FineStep) - Center;
						Mesh.Vertices.Add(FVector(Position.X * 100.0, Position.Y * 100.0, Local[L] * 100.0 - ActorZ));
						Mesh.Normals.Add(FVector(-(Local[L + 1] - Local[L - 1]), -(Local[L + RW] - Local[L - RW]), 2.f * FineStep).GetSafeNormal());
					}
				}
				for (int32 J = 0; J + 1 < H; ++J)
				{
					for (int32 I = 0; I + 1 < W; ++I)
					{
						const int32 V00 = Base + J * W + I;
						Mesh.Triangles.Append({ V00, V00 + W, V00 + 1, V00 + 1, V00 + W, V00 + W + 1 });
					}
				}

				// Skirts on the edges shared with a chunk of another tile (which may be shown at another level).
				auto Skirt = [&](int32 NX, int32 NY, int32 Count, TFunctionRef<int32(int32)> EdgeVertex)
				{
					if (LevelAt(NX, NY) <= 1 || TileOf(NX, NY) == TileOf(CX, CY))
					{
						return;
					}
					const int32 SkirtBase = Mesh.Vertices.Num();
					for (int32 I = 0; I < Count; ++I)
					{
						const int32 V = Base + EdgeVertex(I);
						const FVector Vertex = Mesh.Vertices[V] - FVector(0.0, 0.0, SkirtDepth * 100.0);
						const FVector Normal = Mesh.Normals[V];
						Mesh.Vertices.Add(Vertex);
						Mesh.Normals.Add(Normal);
					}
					for (int32 I = 0; I + 1 < Count; ++I)
					{
						const int32 A = Base + EdgeVertex(I), B = Base + EdgeVertex(I + 1);
						const int32 C = SkirtBase + I, D = SkirtBase + I + 1;
						// Both windings: the gap may be seen from either side.
						Mesh.Triangles.Append({ A, B, C, B, D, C, A, C, B, B, C, D });
					}
				};
				Skirt(CX - 1, CY, H, [&](int32 J) { return J * W; });
				Skirt(CX + 1, CY, H, [&](int32 J) { return J * W + W - 1; });
				Skirt(CX, CY - 1, W, [&](int32 I) { return I; });
				Skirt(CX, CY + 1, W, [&](int32 I) { return (H - 1) * W + I; });
				VertexCounts[Lod] += W * H;
			}
		}
	}

	LandTileLods.Reset();
	for (int32 Tile = 0; Tile < Tiles * Tiles; ++Tile)
	{
		if (LodMeshes[0][Tile].Vertices.Num() == 0)
		{
			continue;
		}
		FLandTileLods& Entry = LandTileLods.AddDefaulted_GetRef();
		for (int32 Lod = 0; Lod < NumLods; ++Lod)
		{
			FTileMesh& Mesh = LodMeshes[Lod][Tile];
			UOceanMeshComponent* Component = NewObject<UOceanMeshComponent>(this, NAME_None, RF_Transient);
			Component->ComponentTags.Add(TEXT("LandTile"));
			Component->SetupAttachment(RootComponent);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetVisibility(Lod == 0);
			Component->RegisterComponent();
			TArray<FProcMeshTangent> Tangents;
			Tangents.Init(FProcMeshTangent(1.f, 0.f, 0.f), Mesh.Vertices.Num());
			Component->CreateMeshSection_LinearColor(0, Mesh.Vertices, Mesh.Triangles, Mesh.Normals, TArray<FVector2D>(), TArray<FLinearColor>(), Tangents, false);
			if (Lod == 0)
			{
				Entry.Bounds = Component->Bounds.GetBox();
			}
			Entry.Lods.Add(LandTiles.Add(Component));
			Mesh = FTileMesh();
		}
		Entry.Shown = 0;
	}
	UpdateLandTileLods();
	UE_LOG(LogTemp, Display, TEXT("OceanBathymetry: sea floor %d x %d at %.1f m; %d coast chunks at %.2f m, %d land chunks at %.2f m: %lld / %lld / %lld vertices per level of detail in %d tiles, %.1f s"),
		M, M, Step, CoastChunks, Step / CoastSubdivision, LandChunks, Step / LandSubdivision, VertexCounts[0], VertexCounts[1], VertexCounts[2],
		LandTileLods.Num(), FPlatformTime::Seconds() - StartTime);
}

void AOceanBathymetry::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateLandTileLods();
}

void AOceanBathymetry::UpdateLandTileLods()
{
	// The viewer: the player's camera in game, the level viewport's camera in the editor (as AOceanActor follows it).
	FVector Viewer;
	bool bHasViewer = false;
	if (const UWorld* World = GetWorld(); World && World->IsGameWorld())
	{
		if (const APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(this, 0))
		{
			Viewer = CameraManager->GetCameraLocation();
			bHasViewer = true;
		}
	}
#if WITH_EDITOR
	else if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->IsPerspective())
	{
		Viewer = GCurrentLevelEditingViewportClient->GetViewLocation();
		bHasViewer = true;
	}
#endif
	const float Lod1 = LandLod1Distance * 100.f;
	const float Lod2 = FMath::Max(LandLod2Distance, LandLod1Distance) * 100.f;
	bool bChanged = false;
	for (FLandTileLods& Entry : LandTileLods)
	{
		int32 Wanted = 0;
		if (bHasViewer)
		{
			const double Distance = FMath::Sqrt(Entry.Bounds.ComputeSquaredDistanceToPoint(Viewer));
			Wanted = Distance > Lod2 ? 2 : (Distance > Lod1 ? 1 : 0);
		}
		Wanted = FMath::Min(Wanted, Entry.Lods.Num() - 1);
		if (Wanted == Entry.Shown)
		{
			continue;
		}
		for (int32 Lod = 0; Lod < Entry.Lods.Num(); ++Lod)
		{
			if (UOceanMeshComponent* Component = LandTiles.IsValidIndex(Entry.Lods[Lod]) ? LandTiles[Entry.Lods[Lod]].Get() : nullptr)
			{
				Component->SetVisibility(Lod == Wanted);
			}
		}
		Entry.Shown = Wanted;
		bChanged = true;
	}
	if (bChanged && UE_LOG_ACTIVE(LogTemp, Verbose))
	{
		int32 Counts[3] = {};
		int64 Vertices = 0;
		for (const FLandTileLods& Entry : LandTileLods)
		{
			Counts[FMath::Clamp(Entry.Shown, 0, 2)] += 1;
			if (UOceanMeshComponent* Component = LandTiles.IsValidIndex(Entry.Lods[Entry.Shown]) ? LandTiles[Entry.Lods[Entry.Shown]].Get() : nullptr)
			{
				Vertices += Component->GetProcMeshSection(0) ? Component->GetProcMeshSection(0)->ProcVertexBuffer.Num() : 0;
			}
		}
		UE_LOG(LogTemp, Verbose, TEXT("OceanBathymetry: land tiles shown at full / half / quarter detail: %d / %d / %d, %lld vertices"),
			Counts[0], Counts[1], Counts[2], Vertices);
	}
}

void AOceanBathymetry::BuildSprayCurtain(const FOceanShoreData& Data)
{
	UMaterialInterface* Material = SprayMaterial.LoadSynchronous();
	if (!Material || !ShoreTexture)
	{
		Spray->ClearAllMeshSections();
		return;
	}
	const int32 N = Data.Resolution;
	const float Texel = Data.SizeM / N;
	const FVector2f Center(float(GetActorLocation().X * 0.01), float(GetActorLocation().Y * 0.01));
	const double ActorZ = GetActorLocation().Z;
	auto Depth = [&](int32 X, int32 Y) { return Data.Texels[Y * N + X].X; };
	auto Position = [&](float X, float Y) { return Data.OriginM + FVector2f((X + 0.5f) * Texel, (Y + 0.5f) * Texel); };

	TArray<FVector> Vertices;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> Colors;
	TArray<int32> Triangles;
	int32 Segments = 0;
	const float Break = SprayBreakDepth;
	for (int32 Y = 0; Y + 1 < N; ++Y)
	{
		for (int32 X = 0; X + 1 < N; ++X)
		{
			const float D[4] = { Depth(X, Y), Depth(X + 1, Y), Depth(X + 1, Y + 1), Depth(X, Y + 1) };
			const bool bAbove = D[0] > Break;
			if (bAbove == (D[1] > Break) && bAbove == (D[2] > Break) && bAbove == (D[3] > Break))
			{
				continue;
			}
			// Only near the island: the shore within ~80 m (signed distance, + at sea).
			if (Data.Texels[Y * N + X].Y > 80.f)
			{
				continue;
			}
			// Marching squares: the isobath crosses the cell's edges where the depth passes Break.
			const FVector2f Corner[4] = { Position(X, Y), Position(X + 1, Y), Position(X + 1, Y + 1), Position(X, Y + 1) };
			FVector2f Crossings[4];
			int32 Count = 0;
			for (int32 Edge = 0; Edge < 4; ++Edge)
			{
				const int32 A = Edge;
				const int32 B = (Edge + 1) % 4;
				if ((D[A] > Break) != (D[B] > Break))
				{
					const float T = (Break - D[A]) / (D[B] - D[A]);
					Crossings[Count++] = FMath::Lerp(Corner[A], Corner[B], T);
				}
			}
			// Shoreward: down the depth gradient.
			const FVector2f Gradient(((D[1] - D[0]) + (D[2] - D[3])) * 0.5f / Texel, ((D[3] - D[0]) + (D[2] - D[1])) * 0.5f / Texel);
			const FVector2f Shoreward = (-Gradient).GetSafeNormal();
			if (Shoreward.IsNearlyZero())
			{
				continue;
			}
			// The waves burst much higher against the rocky points: the rockiness a few metres shoreward.
			const FVector2f CellCenter = (Corner[0] + Corner[2]) * 0.5f;
			const float Rock = SampleTerrain(CellCenter + Shoreward * 6.f).Rock;
			for (int32 Pair = 0; Pair + 1 < Count; Pair += 2)
			{
				++Segments;
				// One strip, on the line itself: its ends are shared with the neighbouring cells, so it is continuous
				// (strips offset along each cell's own normal overlapped and showed as bands).
				for (const float Offset : { 0.f })
				{
					const FVector2f P0 = Crossings[Pair] + Shoreward * Offset - Center;
					const FVector2f P1 = Crossings[Pair + 1] + Shoreward * Offset - Center;
					const int32 First = Vertices.Num();
					const FVector Normal(Shoreward.X, Shoreward.Y, 0.f);
					const FLinearColor Color(Rock, 0.5f, 0.f, 1.f);
					for (const FVector3f& Corner3 : { FVector3f(P0.X, P0.Y, -0.5f), FVector3f(P1.X, P1.Y, -0.5f),
						FVector3f(P1.X, P1.Y, SprayCurtainHeight), FVector3f(P0.X, P0.Y, SprayCurtainHeight) })
					{
						Vertices.Add(FVector(Corner3.X * 100.0, Corner3.Y * 100.0, Corner3.Z * 100.0 - ActorZ));
						Normals.Add(Normal);
						Colors.Add(Color);
					}
					UVs.Append({ FVector2D(0.0, 0.0), FVector2D(1.0, 0.0), FVector2D(1.0, 1.0), FVector2D(0.0, 1.0) });
					Triangles.Append({ First, First + 1, First + 2, First, First + 2, First + 3 });
				}
			}
		}
	}
	TArray<FProcMeshTangent> Tangents;
	Tangents.Init(FProcMeshTangent(1.f, 0.f, 0.f), Vertices.Num());
	Spray->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, UVs, Colors, Tangents, false);
	if (!SprayMaterialInstance || SprayMaterialInstance->Parent != Material)
	{
		SprayMaterialInstance = UMaterialInstanceDynamic::Create(Material, this);
	}
	SprayMaterialInstance->SetTextureParameterValue(TEXT("ShoreMap"), ShoreTexture);
	SprayMaterialInstance->SetVectorParameterValue(TEXT("ShoreMapOrigin"), FLinearColor(Data.OriginM.X, Data.OriginM.Y, 0.f, 0.f));
	SprayMaterialInstance->SetScalarParameterValue(TEXT("ShoreMapSize"), Data.SizeM);
	Spray->SetMaterial(0, SprayMaterialInstance);
	UE_LOG(LogTemp, Display, TEXT("OceanBathymetry: spray curtain along %d segments of the %.1f m isobath"), Segments, Break);
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
