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
	PalmFrondMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/Flora/M_PalmFrond.M_PalmFrond")));
	PalmBarkMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/Flora/M_PalmBark.M_PalmBark")));
	RockMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/Flora/M_Rock.M_Rock")));

	auto Infinigen = [](TArray<TSoftObjectPtr<UStaticMesh>>& Out, const TCHAR* Category, const TCHAR* Factory, int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FString Name = FString::Printf(TEXT("%s_%03d"), Factory, Index);
			Out.Add(TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(FString::Printf(TEXT("/Game/Ocean/Infinigen/%s/%s.%s"), Category, *Name, *Name))));
		}
	};
	Infinigen(RockMeshes, TEXT("rocks"), TEXT("BoulderFactory"), 4);
	Infinigen(CoralMeshes, TEXT("corals"), TEXT("ElkhornCoralFactory"), 2);
	Infinigen(CoralMeshes, TEXT("corals"), TEXT("StarCoralFactory"), 2);
	Infinigen(UrchinMeshes, TEXT("shells"), TEXT("UrchinFactory"), 2);
	Infinigen(ShellMeshes, TEXT("shells"), TEXT("ConchFactory"), 2);
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
	UMaterialInterface* PalmFrond = PalmFrondMaterial.LoadSynchronous();
	if (!PalmFrond)
	{
		PalmFrond = Foliage; // no frond textures yet (Scripts/create_flora_materials.py)
	}
	UMaterialInterface* PalmBark = PalmBarkMaterial.LoadSynchronous();
	if (!PalmBark)
	{
		PalmBark = Bark;
	}
	UMaterialInterface* Stone = RockMaterial.LoadSynchronous();

	// --- Meshes: a few variants of each, generated from the seed ---
	TArray<UHierarchicalInstancedStaticMeshComponent*> Palms, Trees, Shrubs, Rocks;
	for (int32 Variant = 0; Variant < NumPalmVariants; ++Variant)
	{
		UStaticMesh* Mesh = OceanFlora::BuildStaticMesh(this, OceanFlora::MakeCoconutPalm(Seed * 100 + Variant), { PalmBark, PalmFrond, Bark });
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
	// Infinigen assets that are present; each instancer remembers its mesh's size, to scale instances to metres.
	TArray<float> RockSizes, CoralSizes, UrchinSizes, ShellSizes;
	TArray<UHierarchicalInstancedStaticMeshComponent*> Corals, Urchins, Shells;
	auto LoadSet = [&](const TArray<TSoftObjectPtr<UStaticMesh>>& Soft, float CullDistanceM,
		TArray<UHierarchicalInstancedStaticMeshComponent*>& Instancers, TArray<float>& SizesM)
	{
		for (const TSoftObjectPtr<UStaticMesh>& Entry : Soft)
		{
			if (UStaticMesh* Mesh = Entry.LoadSynchronous())
			{
				Instancers.Add(AddInstancer(Mesh, CullDistanceM));
				SizesM.Add(FMath::Max(float(Mesh->GetBounds().BoxExtent.GetMax()) * 2.f * 0.01f, 0.01f));
			}
		}
	};
	LoadSet(RockMeshes, 2000.f, Rocks, RockSizes);
	LoadSet(CoralMeshes, 400.f, Corals, CoralSizes);
	LoadSet(UrchinMeshes, 80.f, Urchins, UrchinSizes);
	LoadSet(ShellMeshes, 60.f, Shells, ShellSizes);
	if (Rocks.Num() == 0)
	{
		for (int32 Variant = 0; Variant < NumRockVariants; ++Variant)
		{
			UStaticMesh* Mesh = OceanFlora::BuildStaticMesh(this, OceanFlora::MakeRock(Seed * 400 + Variant), { Stone });
			Meshes.Add(Mesh);
			Rocks.Add(AddInstancer(Mesh, 2000.f));
			RockSizes.Add(2.f); // MakeRock: unit radius
		}
	}

	// --- Scatter on the island, on jittered grids, with rules per species ---
	FRandomStream Random(Seed);
	const FVector2f IslandCenter(float(Island->GetActorLocation().X * 0.01), float(Island->GetActorLocation().Y * 0.01));
	TArray<TArray<FTransform>> PalmTransforms, TreeTransforms, ShrubTransforms, RockTransforms, CoralTransforms, UrchinTransforms, ShellTransforms;
	PalmTransforms.SetNum(NumPalmVariants);
	TreeTransforms.SetNum(NumTreeVariants);
	ShrubTransforms.SetNum(NumShrubVariants);
	RockTransforms.SetNum(Rocks.Num());
	CoralTransforms.SetNum(Corals.Num());
	UrchinTransforms.SetNum(Urchins.Num());
	ShellTransforms.SetNum(Shells.Num());

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

	// Coconut palms grow along the vegetation line, where the beach meets the coastal forest (Understory: the
	// forest's bush mass, 0 on open sand): a band from the forest edge a few metres into the forest, plus a few
	// on the last metres of sand in front of it, leaning out over the beach towards the light. None on open sand.
	Scatter(5.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
	{
		if (Terrain.Ground < 0.5f || Terrain.Rock > 0.3f)
		{
			return;
		}
		const FVector2f Seaward = (Position - IslandCenter).GetSafeNormal();
		float Edge = 0.f;
		if (Terrain.Understory > 0.02f)
		{
			// Inside the forest: only its outer fringe (thin understory), most at the very edge.
			Edge = 1.f - Smooth(0.4f, 1.6f, Terrain.Understory);
		}
		else
		{
			// On the sand: only right in front of the forest (forest 5 m inland).
			const FOceanTerrainSample Behind = Island->SampleTerrain(Position - Seaward * 5.f);
			Edge = 0.45f * Smooth(0.1f, 0.6f, Behind.Understory);
		}
		const float Grove = Smooth(0.25f, 0.6f, OceanValueNoise(Position / 45.f + 13.f));
		if (Random.FRand() >= 0.8f * PalmDensity * Edge * Grove)
		{
			return;
		}
		const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Seaward.Y, Seaward.X)) + Random.FRandRange(-35.f, 35.f);
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
		const float SizeM = FMath::Lerp(1.2f, 5.6f, FMath::Square(Random.FRand())) * (0.7f + Terrain.Rock);
		const int32 Variant = Random.RandRange(0, Rocks.Num() - 1);
		const float Scale = SizeM / RockSizes[Variant];
		RockTransforms[Variant].Add(FTransform(
			FRotator(Random.FRandRange(-12.f, 12.f), Random.FRandRange(0.f, 360.f), Random.FRandRange(-12.f, 12.f)),
			Ground(Position, Terrain.Ground, 10.f * SizeM), FVector(Scale)));
	});

	// Patch reefs, exactly where the sea floor material draws them (OceanTerrainSurface in Shaders/Ocean.ush:
	// same warped fractal gradient noise, evaluated on the CPU with the identical functions).
	auto ReefCover = [](const FVector2f& PosM, float GroundM)
	{
		const FVector2f Warp = 8.f * FVector2f(OceanGradientNoise(PosM / 31.f + FVector2f(5.f)), OceanGradientNoise(PosM / 31.f + FVector2f(9.f))) - FVector2f(4.f);
		const float Reef = OceanFractalNoise(PosM + Warp, 22.f, 5, 0.55f) + 0.5f;
		const float Deep = Smooth(-0.8f, -2.2f, GroundM);
		return Smooth(0.66f, 0.72f, Reef) * Deep;
	};
	if (Corals.Num() > 0)
	{
		// Coral heads on the reefs, ~2 m apart, from 2 to 15 m deep.
		Scatter(2.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
		{
			if (Terrain.Ground > -1.5f || Terrain.Ground < -15.f)
			{
				return;
			}
			if (Random.FRand() >= 0.75f * CoralDensity * ReefCover(Position, Terrain.Ground))
			{
				return;
			}
			const int32 Variant = Random.RandRange(0, Corals.Num() - 1);
			const float SizeM = FMath::Lerp(0.6f, 2.f, Random.FRand());
			CoralTransforms[Variant].Add(FTransform(FRotator(Random.FRandRange(-8.f, 8.f), Random.FRandRange(0.f, 360.f), Random.FRandRange(-8.f, 8.f)),
				Ground(Position, Terrain.Ground, 10.f), FVector(SizeM / CoralSizes[Variant])));
		});
	}
	if (Urchins.Num() > 0)
	{
		// Long-spined urchins: in the shelter of the reef edges and of submerged rocks.
		Scatter(1.5f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
		{
			if (Terrain.Ground > -0.8f || Terrain.Ground < -8.f)
			{
				return;
			}
			const float Cover = ReefCover(Position, Terrain.Ground);
			const float Edge = Cover * (1.f - Cover) * 4.f; // most where the reef meets the sand
			if (Random.FRand() >= 0.12f * CoralDensity * FMath::Max(Edge, Terrain.Rock))
			{
				return;
			}
			const int32 Variant = Random.RandRange(0, Urchins.Num() - 1);
			const float SizeM = FMath::Lerp(0.15f, 0.3f, Random.FRand());
			UrchinTransforms[Variant].Add(FTransform(FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f),
				Ground(Position, Terrain.Ground, 3.f), FVector(SizeM / UrchinSizes[Variant])));
		});
	}
	if (Shells.Num() > 0)
	{
		// Shells washed up on the beach, most of them near the wrack line.
		Scatter(2.5f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
		{
			if (Terrain.Ground < 0.15f || Terrain.Ground > 1.5f || Terrain.Rock > 0.3f)
			{
				return;
			}
			if (Random.FRand() >= 0.04f)
			{
				return;
			}
			const int32 Variant = Random.RandRange(0, Shells.Num() - 1);
			const float SizeM = FMath::Lerp(0.08f, 0.2f, Random.FRand());
			// Lying on their side, half sunk in the sand.
			ShellTransforms[Variant].Add(FTransform(FRotator(Random.FRandRange(60.f, 100.f), Random.FRandRange(0.f, 360.f), 0.f),
				Ground(Position, Terrain.Ground, 30.f * SizeM), FVector(SizeM / ShellSizes[Variant])));
		});
	}

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
	AddAll(Corals, CoralTransforms);
	AddAll(Urchins, UrchinTransforms);
	AddAll(Shells, ShellTransforms);
	auto Count = [](const TArray<TArray<FTransform>>& Sets)
	{
		int32 Total = 0;
		for (const TArray<FTransform>& Set : Sets)
		{
			Total += Set.Num();
		}
		return Total;
	};
	UE_LOG(LogTemp, Display, TEXT("OceanIslandFlora: %d rock, %d coral, %d urchin, %d shell instances"),
		Count(RockTransforms), Count(CoralTransforms), Count(UrchinTransforms), Count(ShellTransforms));
	bBuilt = true;
}
