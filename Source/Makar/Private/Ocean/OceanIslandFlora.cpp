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
	constexpr int32 NumBushVariants = 4;
	constexpr int32 NumGroundVariants = 3; // fallen fronds, coconuts, grass tufts
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
	BroadleafLeavesMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/Flora/M_LeafCards_Broadleaf.M_LeafCards_Broadleaf")));
	SeaGrapeLeavesMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/Flora/M_LeafCards_SeaGrape.M_LeafCards_SeaGrape")));
	GrassTuftMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/Flora/M_GrassTuft.M_GrassTuft")));
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
	// Caribbean reef builders: elkhorn, star and brain corals, and sea fans (Gorgonia).
	Infinigen(CoralMeshes, TEXT("corals"), TEXT("ElkhornCoralFactory"), 2);
	Infinigen(CoralMeshes, TEXT("corals"), TEXT("StarCoralFactory"), 2);
	// Brain corals: the tan ones (Infinigen's _001 came out pink, not a Caribbean brain coral's colour).
	CoralMeshes.Add(TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/Ocean/Infinigen/corals/BrainCoralFactory_000.BrainCoralFactory_000"))));
	CoralMeshes.Add(TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/Ocean/Infinigen/corals/BrainCoralFactory_002.BrainCoralFactory_002"))));
	Infinigen(CoralMeshes, TEXT("corals"), TEXT("FanCoralFactory"), 3);
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
	UMaterialInterface* Stone = RockMaterial.LoadSynchronous();
	// Materials built from Infinigen renders (Scripts/create_flora_materials.py); until they exist, the plain ones.
	auto LoadOr = [](const TSoftObjectPtr<UMaterialInterface>& Material, UMaterialInterface* Fallback)
	{
		UMaterialInterface* Loaded = Material.LoadSynchronous();
		return Loaded ? Loaded : Fallback;
	};
	UMaterialInterface* PalmFrond = LoadOr(PalmFrondMaterial, Foliage);
	UMaterialInterface* PalmBark = LoadOr(PalmBarkMaterial, Bark);
	UMaterialInterface* BroadleafLeaves = LoadOr(BroadleafLeavesMaterial, Foliage);
	UMaterialInterface* SeaGrapeLeaves = LoadOr(SeaGrapeLeavesMaterial, Foliage);

	// --- Meshes: a few variants of each, generated from the seed ---
	TArray<UHierarchicalInstancedStaticMeshComponent*> Palms, Trees, Shrubs, Bushes, Rocks;
	for (int32 Variant = 0; Variant < NumPalmVariants; ++Variant)
	{
		UStaticMesh* Mesh = OceanFlora::BuildStaticMesh(this, OceanFlora::MakeCoconutPalm(Seed * 100 + Variant), { PalmBark, PalmFrond, Bark });
		Meshes.Add(Mesh);
		Palms.Add(AddInstancer(Mesh, 3000.f));
	}
	for (int32 Variant = 0; Variant < NumTreeVariants; ++Variant)
	{
		UStaticMesh* Mesh = OceanFlora::BuildStaticMesh(this, OceanFlora::MakeBroadleafTree(Seed * 200 + Variant), { Bark, BroadleafLeaves });
		Meshes.Add(Mesh);
		Trees.Add(AddInstancer(Mesh, 3000.f));
	}
	for (int32 Variant = 0; Variant < NumShrubVariants; ++Variant)
	{
		UStaticMesh* Mesh = OceanFlora::BuildStaticMesh(this, OceanFlora::MakeSeaGrape(Seed * 300 + Variant), { Bark, SeaGrapeLeaves });
		Meshes.Add(Mesh);
		Shrubs.Add(AddInstancer(Mesh, 1500.f));
	}
	for (int32 Variant = 0; Variant < NumBushVariants; ++Variant)
	{
		UStaticMesh* Mesh = OceanFlora::BuildStaticMesh(this, OceanFlora::MakeCoastalBush(Seed * 400 + Variant), { Bark, BroadleafLeaves });
		Meshes.Add(Mesh);
		Bushes.Add(AddInstancer(Mesh, 1500.f));
	}
	// Ground cover: dead fronds and coconuts under the palms, grass tufts (yellower in the last variant, for the sunny
	// beach top). Small: culled sooner.
	UMaterialInterface* GrassTuft = GrassTuftMaterial.LoadSynchronous();
	TArray<UHierarchicalInstancedStaticMeshComponent*> FallenFronds, Coconuts, Tufts;
	for (int32 Variant = 0; Variant < NumGroundVariants; ++Variant)
	{
		UStaticMesh* Frond = OceanFlora::BuildStaticMesh(this, OceanFlora::MakeFallenFrond(Seed * 500 + Variant), { PalmFrond });
		Meshes.Add(Frond);
		FallenFronds.Add(AddInstancer(Frond, 250.f));
		UStaticMesh* Nut = OceanFlora::BuildStaticMesh(this, OceanFlora::MakeCoconut(Seed * 600 + Variant), { Bark });
		Meshes.Add(Nut);
		Coconuts.Add(AddInstancer(Nut, 150.f));
		if (GrassTuft)
		{
			FOceanMeshData TuftData = OceanFlora::MakeGrassTuft(Seed * 700 + Variant, GrassTuftSpanCm, GrassTuftBaseV);
			if (Variant == NumGroundVariants - 1)
			{
				for (FOceanMeshData::FVertex& Vertex : TuftData.Vertices)
				{
					Vertex.Color *= FLinearColor(1.35f, 1.1f, 0.75f); // sun-dried, straw-tipped
				}
			}
			UStaticMesh* Tuft = OceanFlora::BuildStaticMesh(this, TuftData, { GrassTuft });
			Meshes.Add(Tuft);
			Tufts.Add(AddInstancer(Tuft, 120.f));
		}
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
	TArray<TArray<FTransform>> PalmTransforms, TreeTransforms, ShrubTransforms, BushTransforms, RockTransforms, CoralTransforms, UrchinTransforms, ShellTransforms;
	PalmTransforms.SetNum(NumPalmVariants);
	TreeTransforms.SetNum(NumTreeVariants);
	ShrubTransforms.SetNum(NumShrubVariants);
	BushTransforms.SetNum(NumBushVariants);
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

	// The understory: the forest's bush mass (Understory, 1.5-3 m above the ground) is real shrubs, coastal bushes and
	// sea grapes, standing on the terrain (which keeps only UnderstoryRelief of the mass) and as tall as the mass plus
	// about a metre. Densest along the forest edge (low ground, just behind the beach), the wall of scrub seen from
	// the beach; thinner under the canopy, where the trees hide it.
	const float Relief = Island->GetUnderstoryRelief();
	Scatter(3.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
	{
		if (Terrain.Understory < 0.1f || Terrain.Rock > 0.6f)
		{
			return;
		}
		const float Fringe = 1.f - Smooth(6.f, 10.f, Terrain.Ground);
		if (Random.FRand() >= ShrubDensity * Smooth(0.1f, 0.5f, Terrain.Understory) * FMath::Lerp(0.35f, 1.f, Fringe))
		{
			return;
		}
		// The bushes are ~2.2 m tall, the sea grapes ~1.6 m.
		const bool bSeaGrape = Random.FRand() < 0.35f;
		const float Height = (1.f - Relief) * Terrain.Understory + 1.f;
		const float Scale = FMath::Clamp(Height / (bSeaGrape ? 1.6f : 2.2f), 0.6f, 1.8f) * Random.FRandRange(0.85f, 1.15f);
		const FTransform Transform(FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f),
			Ground(Position, Terrain.Ground + Relief * Terrain.Understory, 15.f), FVector(Scale));
		if (bSeaGrape)
		{
			ShrubTransforms[Random.RandRange(0, NumShrubVariants - 1)].Add(Transform);
		}
		else
		{
			BushTransforms[Random.RandRange(0, NumBushVariants - 1)].Add(Transform);
		}
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

	// --- Ground cover ---
	// Props that lie on the ground sit on the rendered terrain (ground plus the understory's relief).
	auto Surface = [&](const FVector2f& Position) { return Island->GetTerrainHeight(Position); };
	auto SlopeAt = [&](const FVector2f& Position, float Height)
	{
		return FVector2f((Island->SampleTerrain(Position + FVector2f(0.7f, 0.f)).Ground - Height) / 0.7f,
			(Island->SampleTerrain(Position + FVector2f(0.f, 0.7f)).Ground - Height) / 0.7f).Size();
	};

	// Wind-pruned scrub on top of the rocky points: low sea grapes and bushes between the limestone, none on the cliff
	// faces themselves (the forest's understory stops short of the rock).
	Scatter(3.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
	{
		if (Terrain.Rock < 0.25f || Terrain.Ground < 1.2f
			|| Random.FRand() >= 0.6f * ShrubDensity * Smooth(0.25f, 0.45f, Terrain.Rock) || SlopeAt(Position, Terrain.Ground) > 0.6f)
		{
			return;
		}
		const FTransform Transform(FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f), Ground(Position, Terrain.Ground, 15.f),
			FVector(Random.FRandRange(0.4f, 0.8f)));
		if (Random.FRand() < 0.5f)
		{
			ShrubTransforms[Random.RandRange(0, NumShrubVariants - 1)].Add(Transform);
		}
		else
		{
			BushTransforms[Random.RandRange(0, NumBushVariants - 1)].Add(Transform);
		}
	});

	// Under each palm, dead fronds and fallen coconuts, mostly under the crown (the side the palm leans to, its +X).
	TArray<TArray<FTransform>> FrondTransforms, CoconutTransforms, TuftTransforms;
	FrondTransforms.SetNum(NumGroundVariants);
	CoconutTransforms.SetNum(NumGroundVariants);
	TuftTransforms.SetNum(NumGroundVariants);
	for (const TArray<FTransform>& Set : PalmTransforms)
	{
		for (const FTransform& Palm : Set)
		{
			const FVector2f Base(float(Palm.GetLocation().X * 0.01), float(Palm.GetLocation().Y * 0.01));
			const FVector Forward3 = Palm.GetRotation().GetForwardVector();
			const FVector2f Forward = FVector2f(float(Forward3.X), float(Forward3.Y)).GetSafeNormal();
			auto Around = [&](float MinM, float MaxM, float Spread)
			{
				const float Angle = Random.FRandRange(-Spread, Spread);
				const FVector2f Direction(Forward.X * FMath::Cos(Angle) - Forward.Y * FMath::Sin(Angle), Forward.X * FMath::Sin(Angle) + Forward.Y * FMath::Cos(Angle));
				return Base + Direction * Random.FRandRange(MinM, MaxM);
			};
			const int32 Fronds = FMath::RoundToInt(Random.FRandRange(0.f, 2.4f) * GroundCoverDensity);
			for (int32 Index = 0; Index < Fronds; ++Index)
			{
				const FVector2f Position = Around(0.3f, 3.5f, 2.2f);
				const float Height = Surface(Position);
				if (Height > 0.4f)
				{
					FrondTransforms[Random.RandRange(0, NumGroundVariants - 1)].Add(FTransform(FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f),
						Ground(Position, Height, 2.f), FVector(Random.FRandRange(0.85f, 1.1f))));
				}
			}
			const int32 Nuts = FMath::RoundToInt(Random.FRandRange(0.f, 3.4f) * GroundCoverDensity);
			for (int32 Index = 0; Index < Nuts; ++Index)
			{
				const FVector2f Position = Around(0.5f, 4.f, 1.3f);
				const float Height = Surface(Position);
				if (Height > 0.4f)
				{
					CoconutTransforms[Random.RandRange(0, NumGroundVariants - 1)].Add(FTransform(
						FRotator(Random.FRandRange(-15.f, 15.f), Random.FRandRange(0.f, 360.f), Random.FRandRange(-20.f, 20.f)),
						Ground(Position, Height, 3.f), FVector(Random.FRandRange(0.85f, 1.15f))));
				}
			}
		}
	}

	// Grass tufts, in colonies of a few tussocks: along the vegetation line (the forest's fringe and the last metres of
	// sand in front of it, straw-tipped on the sunny sand), and in the clearings (the same clearings as the trees' and
	// the ground texture's: OceanValueNoise(P / 80 + 3)).
	if (Tufts.Num() == NumGroundVariants)
	{
		Scatter(2.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
		{
			if (Terrain.Ground < 1.f || Terrain.Rock > 0.4f)
			{
				return;
			}
			const FVector2f Seaward = (Position - IslandCenter).GetSafeNormal();
			const float Fringe = Terrain.Understory > 0.02f ? 1.f - Smooth(0.5f, 1.4f, Terrain.Understory)
				: Smooth(0.1f, 0.5f, Island->SampleTerrain(Position - Seaward * 4.f).Understory);
			const float Clearing = Smooth(0.65f, 0.8f, OceanValueNoise(Position / 80.f + FVector2f(3.f))) * Smooth(0.2f, 0.6f, Terrain.Understory);
			const float Colonies = Smooth(0.45f, 0.7f, OceanValueNoise(Position / 9.f + FVector2f(31.f)));
			// Along the edge in separate colonies; in the clearings, a sward broken only here and there.
			if (Random.FRand() >= GroundCoverDensity * FMath::Max(0.9f * Fringe * Colonies, 0.85f * Clearing * FMath::Lerp(0.5f, 1.f, Colonies))
				|| SlopeAt(Position, Terrain.Ground) > 0.7f)
			{
				return;
			}
			const bool bBeach = Terrain.Understory < 0.05f;
			const int32 Count = Random.RandRange(3, 7);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FVector2f Tussock = Position + FVector2f(Random.FRandRange(-0.6f, 0.6f), Random.FRandRange(-0.6f, 0.6f));
				const int32 Variant = bBeach && Random.FRand() < 0.6f ? NumGroundVariants - 1 : Random.RandRange(0, NumGroundVariants - 2);
				TuftTransforms[Variant].Add(FTransform(FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f), Ground(Tussock, Surface(Tussock), 0.f),
					FVector(Random.FRandRange(1.6f, 2.6f))));
			}
		});
	}

	// Loose stones: limestone cobbles and pebbles on top of the rocky points and at the foot of their cliffs.
	Scatter(2.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
	{
		if (Terrain.Rock < 0.25f || Terrain.Ground < 0.3f || Terrain.Ground > 12.f || Random.FRand() >= 0.3f * GroundCoverDensity * Terrain.Rock)
		{
			return;
		}
		const float SizeM = FMath::Lerp(0.12f, 0.55f, FMath::Square(Random.FRand()));
		const int32 Variant = Random.RandRange(0, Rocks.Num() - 1);
		RockTransforms[Variant].Add(FTransform(
			FRotator(Random.FRandRange(-30.f, 30.f), Random.FRandRange(0.f, 360.f), Random.FRandRange(-30.f, 30.f)),
			Ground(Position, Surface(Position), 30.f * SizeM), FVector(SizeM / RockSizes[Variant])));
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
			const float Scale = SizeM / CoralSizes[Variant];
			// Brain corals grow as low domes, wider than tall (Infinigen's stand upright).
			const bool bBrain = Corals[Variant]->GetStaticMesh() && Corals[Variant]->GetStaticMesh()->GetName().Contains(TEXT("Brain"));
			CoralTransforms[Variant].Add(FTransform(FRotator(Random.FRandRange(-8.f, 8.f), Random.FRandRange(0.f, 360.f), Random.FRandRange(-8.f, 8.f)),
				Ground(Position, Terrain.Ground, 10.f), bBrain ? FVector(Scale, Scale, Scale * 0.5f) : FVector(Scale)));
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
	AddAll(Bushes, BushTransforms);
	AddAll(FallenFronds, FrondTransforms);
	AddAll(Coconuts, CoconutTransforms);
	AddAll(Tufts, TuftTransforms);
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
	UE_LOG(LogTemp, Display, TEXT("OceanIslandFlora: %d palm, %d tree, %d sea grape, %d bush, %d rock, %d coral, %d urchin, %d shell, ")
		TEXT("%d fallen frond, %d coconut, %d grass tuft instances"),
		Count(PalmTransforms), Count(TreeTransforms), Count(ShrubTransforms), Count(BushTransforms),
		Count(RockTransforms), Count(CoralTransforms), Count(UrchinTransforms), Count(ShellTransforms),
		Count(FrondTransforms), Count(CoconutTransforms), Count(TuftTransforms));
	bBuilt = true;
}
