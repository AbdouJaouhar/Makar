#include "Ocean/OceanBathymetry.h"

#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Ocean/OceanMeshComponent.h"
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
	const float Across = Delta.Size() - (IslandRadius + CoastlineVariation * Coast);
	if (Across < 0.f)
	{
		// Rises with LandSlope at the waterline, levels off at MaxLandHeight.
		const float Height = FMath::Max(MaxLandHeight, 0.01f);
		return Height * (1.f - FMath::Exp(Across * LandSlope / Height));
	}

	// Drops with BeachSlope at the waterline, levels off at ShelfDepth, then the reef drop-off.
	const float Shelf = FMath::Max(ShelfDepth, 0.01f);
	const float Depth = Shelf * (1.f - FMath::Exp(-Across * BeachSlope / Shelf))
		+ DropOffDepth * FMath::SmoothStep(ShelfWidth, ShelfWidth + DropOffWidth, Across);
	return -Depth;
}

void AOceanBathymetry::Build()
{
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

	// Seaward direction: gradient of the distance.
	ParallelFor(N, [&](int32 Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			const int32 Index = Y * N + X;
			const float GX = Distance[Y * N + FMath::Min(X + 1, N - 1)] - Distance[Y * N + FMath::Max(X - 1, 0)];
			const float GY = Distance[FMath::Min(Y + 1, N - 1) * N + X] - Distance[FMath::Max(Y - 1, 0) * N + X];
			const FVector2f Direction = FVector2f(GX, GY).GetSafeNormal();
			Data->Texels[Index] = FVector4f(-Heights[Index], Distance[Index], Direction.X, Direction.Y);
		}
	});

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
	Triangles.Reserve((N - 1) * (N - 1) * 6);
	for (int32 Y = 0; Y + 1 < N; ++Y)
	{
		for (int32 X = 0; X + 1 < N; ++X)
		{
			const int32 V00 = Y * N + X;
			const int32 V10 = V00 + 1;
			const int32 V01 = V00 + N;
			const int32 V11 = V01 + 1;
			Triangles.Append({ V00, V01, V10, V10, V01, V11 });
		}
	}
	Terrain->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, TArray<FVector2D>(), TArray<FLinearColor>(), Tangents, false);
	if (UMaterialInterface* Material = TerrainMaterial.LoadSynchronous())
	{
		Terrain->SetMaterial(0, Material);
	}

	BuildShoreTexture(*Data);
	ShoreData = Data;
	bBuilt = true;
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
