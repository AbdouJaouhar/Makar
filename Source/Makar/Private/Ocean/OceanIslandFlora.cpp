#include "Ocean/OceanIslandFlora.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "Ocean/OceanBathymetry.h"
#include "Ocean/OceanFlora.h"
#include "Ocean/OceanShoreData.h"

namespace
{
	constexpr int32 NumPalmVariants = 4;
	constexpr int32 NumTreeVariants = 3;
	constexpr int32 NumShrubVariants = 3;
	constexpr int32 NumRockVariants = 5;

	float Smooth(float Edge0, float Edge1, float X)
	{
		const float T = FMath::Clamp((X - Edge0) / (Edge1 - Edge0), 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}
}

AOceanIslandFlora::AOceanIslandFlora()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	BarkMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/Flora/M_Bark.M_Bark")));
	FoliageMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/Flora/M_Foliage.M_Foliage")));
	RockMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/Flora/M_Rock.M_Rock")));
}

void AOceanIslandFlora::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Build();
}

void AOceanIslandFlora::BeginPlay()
{
	Super::BeginPlay();
	// OnConstruction does not run again in a cooked game, and nothing generated is saved.
	if (!bBuilt)
	{
		Build();
	}
}

UHierarchicalInstancedStaticMeshComponent* AOceanIslandFlora::AddInstancer(UStaticMesh* Mesh, float CullDistanceM)
{
	UHierarchicalInstancedStaticMeshComponent* Instancer = NewObject<UHierarchicalInstancedStaticMeshComponent>(this, NAME_None, RF_Transient);
	Instancer->SetupAttachment(RootComponent);
	Instancer->SetStaticMesh(Mesh);
	Instancer->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Instancer->SetCullDistances(0, int32(CullDistanceM * 100.f));
	Instancer->RegisterComponent();
	Instancers.Add(Instancer);
	return Instancer;
}

void AOceanIslandFlora::Build()
{
	for (UHierarchicalInstancedStaticMeshComponent* Instancer : Instancers)
	{
		if (Instancer)
		{
			Instancer->DestroyComponent();
		}
	}
	Instancers.Reset();
	Meshes.Reset();
	bBuilt = false;

	UWorld* World = GetWorld();
	AOceanBathymetry* Island = nullptr;
	if (World)
	{
		for (TActorIterator<AOceanBathymetry> It(World); It; ++It)
		{
			Island = *It;
			break;
		}
	}
	if (!Island)
	{
		return;
	}
	Island->EnsureBuilt();
	const TSharedPtr<const FOceanShoreData> Shore = Island->GetShoreData();
	if (!Shore)
	{
		return;
	}

	UMaterialInterface* Bark = BarkMaterial.LoadSynchronous();
	UMaterialInterface* Foliage = FoliageMaterial.LoadSynchronous();
	UMaterialInterface* Stone = RockMaterial.LoadSynchronous();

	// --- Meshes: a few variants of each, generated from the seed ---
	TArray<UHierarchicalInstancedStaticMeshComponent*> Palms, Trees, Shrubs, Rocks;
	for (int32 Variant = 0; Variant < NumPalmVariants; ++Variant)
	{
		UStaticMesh* Mesh = OceanFlora::BuildStaticMesh(this, OceanFlora::MakeCoconutPalm(Seed * 100 + Variant), { Bark, Foliage });
		Meshes.Add(Mesh);
		Palms.Add(AddInstancer(Mesh, 3000.f));
	}
	for (int32 Variant = 0; Variant < NumTreeVariants; ++Variant)
	{
		UStaticMesh* Mesh = OceanFlora::BuildStaticMesh(this, OceanFlora::MakeBroadleafTree(Seed * 200 + Variant), { Bark, Foliage });
		Meshes.Add(Mesh);
		Trees.Add(AddInstancer(Mesh, 3000.f));
	}
	for (int32 Variant = 0; Variant < NumShrubVariants; ++Variant)
	{
		UStaticMesh* Mesh = OceanFlora::BuildStaticMesh(this, OceanFlora::MakeSeaGrape(Seed * 300 + Variant), { Bark, Foliage });
		Meshes.Add(Mesh);
		Shrubs.Add(AddInstancer(Mesh, 1500.f));
	}
	for (int32 Variant = 0; Variant < NumRockVariants; ++Variant)
	{
		UStaticMesh* Mesh = OceanFlora::BuildStaticMesh(this, OceanFlora::MakeRock(Seed * 400 + Variant), { Stone });
		Meshes.Add(Mesh);
		Rocks.Add(AddInstancer(Mesh, 2000.f));
	}

	// --- Scatter on the island, on jittered grids, with rules per species ---
	FRandomStream Random(Seed);
	const FVector2f IslandCenter(float(Island->GetActorLocation().X * 0.01), float(Island->GetActorLocation().Y * 0.01));
	TArray<TArray<FTransform>> PalmTransforms, TreeTransforms, ShrubTransforms, RockTransforms;
	PalmTransforms.SetNum(NumPalmVariants);
	TreeTransforms.SetNum(NumTreeVariants);
	ShrubTransforms.SetNum(NumShrubVariants);
	RockTransforms.SetNum(NumRockVariants);

	auto Scatter = [&](float CellM, TFunctionRef<void(const FVector2f&, const FOceanTerrainSample&)> Place)
	{
		const int32 Cells = FMath::CeilToInt(Shore->SizeM / CellM);
		for (int32 Y = 0; Y < Cells; ++Y)
		{
			for (int32 X = 0; X < Cells; ++X)
			{
				const FVector2f Position = Shore->OriginM
					+ FVector2f((X + 0.5f + Random.FRandRange(-0.4f, 0.4f)) * CellM, (Y + 0.5f + Random.FRandRange(-0.4f, 0.4f)) * CellM);
				// Far from the coast: nothing to place, skip the costly terrain evaluation.
				if (Shore->Sample(Position).Y > 30.f)
				{
					continue;
				}
				Place(Position, Island->SampleTerrain(Position));
			}
		}
	};
	auto Ground = [](const FVector2f& Position, float HeightM, float SinkCm)
	{
		return FVector(Position.X * 100.0, Position.Y * 100.0, HeightM * 100.0 - SinkCm);
	};

	// Coconut palms: the back of the beaches and the forest edge, in loose groves, leaning seawards.
	Scatter(8.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
	{
		if (Terrain.Inland < 4.f || Terrain.Inland > 45.f || Terrain.Ground < 0.5f || Terrain.Rock > 0.3f)
		{
			return;
		}
		const float Grove = Smooth(0.3f, 0.7f, OceanValueNoise(Position / 60.f + 13.f));
		const float Probability = 0.55f * PalmDensity * Grove * (1.f - Smooth(30.f, 45.f, Terrain.Inland));
		if (Random.FRand() >= Probability)
		{
			return;
		}
		const FVector2f Seaward = (Position - IslandCenter).GetSafeNormal();
		const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Seaward.Y, Seaward.X)) + Random.FRandRange(-40.f, 40.f);
		PalmTransforms[Random.RandRange(0, NumPalmVariants - 1)].Add(
			FTransform(FRotator(0.f, Yaw, 0.f), Ground(Position, Terrain.Ground, 0.f), FVector(Random.FRandRange(0.85f, 1.15f))));
	});

	// Broadleaf trees inland, close enough for the crowns to merge into a canopy, with a few clearings.
	Scatter(7.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
	{
		if (Terrain.Inland < 25.f || Terrain.Ground < 2.5f)
		{
			return;
		}
		const float Clearing = Smooth(0.65f, 0.8f, OceanValueNoise(Position / 80.f + 3.f));
		if (Random.FRand() >= 0.85f * TreeDensity * (1.f - Clearing))
		{
			return;
		}
		TreeTransforms[Random.RandRange(0, NumTreeVariants - 1)].Add(
			FTransform(FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f), Ground(Position, Terrain.Ground, 30.f), FVector(Random.FRandRange(0.8f, 1.3f))));
	});

	// Sea grape along the upper beach and the forest edge.
	Scatter(5.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
	{
		if (Terrain.Inland < 5.f || Terrain.Inland > 40.f || Terrain.Ground < 0.8f || Terrain.Rock > 0.5f)
		{
			return;
		}
		if (Random.FRand() >= 0.45f * ShrubDensity * Smooth(0.25f, 0.6f, OceanValueNoise(Position / 25.f + 7.f)))
		{
			return;
		}
		ShrubTransforms[Random.RandRange(0, NumShrubVariants - 1)].Add(
			FTransform(FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f), Ground(Position, Terrain.Ground, 10.f), FVector(Random.FRandRange(0.7f, 1.4f))));
	});

	// Limestone boulders: piled on the rocky points (also under water), a few at the waterline elsewhere.
	Scatter(3.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
	{
		if (Terrain.Ground < -3.f || Terrain.Ground > 6.f)
		{
			return;
		}
		const float Probability = RockDensity * (0.45f * Terrain.Rock + (FMath::Abs(Terrain.Ground) < 0.6f ? 0.015f : 0.f));
		if (Random.FRand() >= Probability)
		{
			return;
		}
		const float Size = FMath::Lerp(0.6f, 2.8f, FMath::Square(Random.FRand())) * (0.7f + Terrain.Rock);
		RockTransforms[Random.RandRange(0, NumRockVariants - 1)].Add(FTransform(
			FRotator(Random.FRandRange(-12.f, 12.f), Random.FRandRange(0.f, 360.f), Random.FRandRange(-12.f, 12.f)),
			Ground(Position, Terrain.Ground, 20.f * Size), FVector(Size)));
	});

	auto AddAll = [](TArray<UHierarchicalInstancedStaticMeshComponent*>& Targets, const TArray<TArray<FTransform>>& Transforms)
	{
		for (int32 Index = 0; Index < Targets.Num(); ++Index)
		{
			Targets[Index]->AddInstances(Transforms[Index], false, true);
		}
	};
	AddAll(Palms, PalmTransforms);
	AddAll(Trees, TreeTransforms);
	AddAll(Shrubs, ShrubTransforms);
	AddAll(Rocks, RockTransforms);
	bBuilt = true;
}
