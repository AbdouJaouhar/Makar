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
	constexpr int32 NumTreeVariants = 5;
	constexpr int32 NumAlmondVariants = 3;
	constexpr int32 NumSmallPlantVariants = 3; // of each of the 3D plants (agave, cacti, bromeliad, fern, heliconia, creeper)
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

	BarkMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/Flora/M_StemF.M_StemF")));
	FoliageMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/Flora/M_Foliage.M_Foliage")));
	PalmFrondMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/Flora/M_PalmFrond.M_PalmFrond")));
	PalmLeafletMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/Flora/M_PalmLeaflet.M_PalmLeaflet")));
	LeafBladeMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/Flora/M_LeafBlade.M_LeafBlade")));
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

UHierarchicalInstancedStaticMeshComponent* AOceanIslandFlora::AddInstancer(UStaticMesh* Mesh, float CullDistanceM, float WindDistanceM)
{
	UHierarchicalInstancedStaticMeshComponent* Instancer = NewObject<UHierarchicalInstancedStaticMeshComponent>(this, NAME_None, RF_Transient);
	Instancer->SetupAttachment(RootComponent);
	Instancer->SetStaticMesh(Mesh);
	Instancer->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Instancer->SetCullDistances(0, int32(CullDistanceM * 100.f));
	// The wind (Shaders/Flora.ush) only where it is seen (a frond swaying at 70 m, a grass tuft at 20): every plant moving by
	// world position offset is drawn in Nanite's slower programmable path, needs velocities and redraws its shadows each
	// frame. Objects lying on the ground never evaluate it.
	Instancer->SetEvaluateWorldPositionOffset(WindDistanceM > 0.f);
	Instancer->SetWorldPositionOffsetDisableDistance(int32(WindDistanceM * 100.f));
	Instancer->RegisterComponent();
	Instancers.Add(Instancer);
	return Instancer;
}

UStaticMesh* AOceanIslandFlora::GetMesh(const FString& Name, TFunctionRef<TArray<FOceanMeshData>()> Generate,
	const TArray<UMaterialInterface*>& Materials, const TArray<float>& ScreenSizes, const TArray<int32>& ColorSections, bool bFoliage)
{
	const FString AssetName = FString::Printf(TEXT("SM_%s_S%d_V%d"), *Name, Seed, OceanFlora::GeneratorVersion);
	const FString PackagePath = TEXT("/Game/Ocean/Flora/Baked/") + AssetName;
	UStaticMesh* Mesh = nullptr;
#if WITH_EDITOR
	// A bake resumes where an interrupted one stopped: meshes of this generator version already saved are loaded below
	// (a generator change bumps OceanFlora::GeneratorVersion, which renames them all).
	if (bBaking && !FPackageName::DoesPackageExist(PackagePath))
	{
		// Nanite renders the full detail at every distance: only the nearest level is baked.
		const TArray<FOceanMeshData> Lods = Generate();
		Mesh = OceanFlora::BakeNaniteMesh(PackagePath, Lods[0], Materials, ColorSections.IsValidIndex(0) ? ColorSections[0] : -1, bFoliage);
	}
#endif
	if (!Mesh && bUseBakedMeshes)
	{
		Mesh = LoadObject<UStaticMesh>(nullptr, *(PackagePath + TEXT(".") + AssetName), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (!Mesh)
	{
		const TArray<FOceanMeshData> Lods = Generate();
		TArray<const FOceanMeshData*> Pointers;
		for (const FOceanMeshData& Lod : Lods)
		{
			Pointers.Add(&Lod);
		}
		Mesh = OceanFlora::BuildStaticMeshLods(this, Pointers, Materials, ScreenSizes, ColorSections);
	}
	Meshes.Add(Mesh);
	return Mesh;
}

void AOceanIslandFlora::BakeMeshes()
{
#if WITH_EDITOR
	bBaking = true;
	Build();
	bBaking = false;
	UE_LOG(LogTemp, Display, TEXT("OceanIslandFlora: baked %d Nanite meshes into /Game/Ocean/Flora/Baked"), Meshes.Num());
#endif
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
	UMaterialInterface* PalmLeaflet = LoadOr(PalmLeafletMaterial, Foliage);
	UMaterialInterface* LeafBlade = LoadOr(LeafBladeMaterial, Foliage);
	UE_LOG(LogTemp, Verbose, TEXT("OceanIslandFlora: palm leaflets %s, leaf blades %s (from %s)"), *GetNameSafe(PalmLeaflet), *GetNameSafe(LeafBlade),
		*LeafBladeMaterial.ToString());
	UMaterialInterface* PalmBark = LoadOr(PalmBarkMaterial, Bark);
	UMaterialInterface* BroadleafLeaves = LoadOr(BroadleafLeavesMaterial, Foliage);
	UMaterialInterface* SeaGrapeLeaves = LoadOr(SeaGrapeLeavesMaterial, Foliage);

	// --- Meshes: a few variants of each, generated from the seed ---
	TArray<UHierarchicalInstancedStaticMeshComponent*> Palms, Trees, Shrubs, Bushes, Rocks;
	for (int32 Variant = 0; Variant < NumPalmVariants; ++Variant)
	{
		// Every leaflet modelled up close, a third of them further, a dozen broad strips a side in the distance (the same
		// crown at every level).
		const int32 PalmSeed = Seed * 100 + Variant;
		UStaticMesh* Mesh = GetMesh(FString::Printf(TEXT("Palm%d"), Variant), [&]() {
			return TArray<FOceanMeshData>{ OceanFlora::MakeCoconutPalm(PalmSeed, 0), OceanFlora::MakeCoconutPalm(PalmSeed, 1), OceanFlora::MakeCoconutPalm(PalmSeed, 2) }; },
			{ PalmBark, PalmLeaflet, Bark }, { 1.f, 0.35f, 0.1f }, { 1, 1, 1 }, true);
		Palms.Add(AddInstancer(Mesh, 3000.f, 70.f));
	}
	// The L-system plants in two levels of detail: real 3D leaves up close (M_LeafBlade), leaf cards further away (the
	// cards show a rendered cluster of leaves, which up close read as flat rosettes).
	// Three levels: dense real leaves, then fewer and larger ones, then cards only where they are a few pixels wide.
	auto TwoLevels = [&](const FString& Name, TFunctionRef<FOceanMeshData(bool, float)> Make, UMaterialInterface* Cards, float NearScreenSize)
	{
		// Branches keep their own section (M_Bark, vertex colours): the leaves' material cuts its quads into leaf shapes.
		return GetMesh(Name, [&]() { return TArray<FOceanMeshData>{ Make(true, 1.f), Make(true, 2.4f), Make(false, 1.f) }; },
			{ Bark, Cards, LeafBlade }, { 1.f, NearScreenSize, 0.3f * NearScreenSize }, { 2, 2, -1 }, true);
	};
	for (int32 Variant = 0; Variant < NumTreeVariants; ++Variant)
	{
		Trees.Add(AddInstancer(TwoLevels(FString::Printf(TEXT("Tree%d"), Variant), [&](bool b3D, float LeafScale) { return OceanFlora::MakeBroadleafTree(Seed * 200 + Variant, b3D, LeafScale); }, BroadleafLeaves, 0.35f), 3000.f, 50.f));
	}
	TArray<UHierarchicalInstancedStaticMeshComponent*> Almonds;
	for (int32 Variant = 0; Variant < NumAlmondVariants; ++Variant)
	{
		Almonds.Add(AddInstancer(TwoLevels(FString::Printf(TEXT("Almond%d"), Variant), [&](bool b3D, float LeafScale) { return OceanFlora::MakeTropicalAlmond(Seed * 250 + Variant, b3D, LeafScale); }, BroadleafLeaves, 0.35f), 3000.f, 50.f));
	}
	for (int32 Variant = 0; Variant < NumShrubVariants; ++Variant)
	{
		Shrubs.Add(AddInstancer(TwoLevels(FString::Printf(TEXT("SeaGrape%d"), Variant), [&](bool b3D, float LeafScale) { return OceanFlora::MakeSeaGrape(Seed * 300 + Variant, b3D, LeafScale); }, SeaGrapeLeaves, 0.2f), 1500.f, 35.f));
	}
	for (int32 Variant = 0; Variant < NumBushVariants; ++Variant)
	{
		Bushes.Add(AddInstancer(TwoLevels(FString::Printf(TEXT("Bush%d"), Variant), [&](bool b3D, float LeafScale) { return OceanFlora::MakeCoastalBush(Seed * 400 + Variant, b3D, LeafScale); }, BroadleafLeaves, 0.2f), 1500.f, 35.f));
	}
	// Single-section meshes (stones, coconuts) moved to a second section, built like the small plants (two slots, colour
	// section 1): built with one section, their vertex colours drew white; with the duplicate triangle of
	// FillDescription in one slot, the default material.
	auto AsSecondSection = [](FOceanMeshData Data)
	{
		Data.Sections.Insert(TArray<int32>(), 0);
		return Data;
	};
	// Smaller plants, all 3D: stems in M_Bark, leaves in M_Foliage; culled with distance.
	auto SmallPlants = [&](TFunctionRef<FOceanMeshData(int32)> Make, int32 SeedBase, float CullM)
	{
		TArray<UHierarchicalInstancedStaticMeshComponent*> Set;
		for (int32 Variant = 0; Variant < NumSmallPlantVariants; ++Variant)
		{
			FOceanMeshData Data = Make(Seed * SeedBase + Variant);
			OceanFlora::MergeSection(Data, 0, 1); // stems with the leaves: one section, its colours show
			const int32 ColorSection = Data.Sections.Num() > 1 ? 1 : 0;
			UStaticMesh* Mesh = GetMesh(FString::Printf(TEXT("Plant%d_%d"), SeedBase, Variant), [&]() { return TArray<FOceanMeshData>{ Data }; },
				{ Foliage, Foliage }, { 1.f }, { ColorSection }, true);
			Set.Add(AddInstancer(Mesh, CullM, 25.f));
		}
		return Set;
	};
	TArray<UHierarchicalInstancedStaticMeshComponent*> Agaves = SmallPlants(OceanFlora::MakeAgave, 810, 600.f);
	TArray<UHierarchicalInstancedStaticMeshComponent*> TurksCaps = SmallPlants(OceanFlora::MakeTurksCap, 820, 120.f);
	TArray<UHierarchicalInstancedStaticMeshComponent*> Columns = SmallPlants(OceanFlora::MakeColumnCactus, 830, 600.f);
	TArray<UHierarchicalInstancedStaticMeshComponent*> Bromeliads = SmallPlants(OceanFlora::MakeBromeliad, 840, 150.f);
	TArray<UHierarchicalInstancedStaticMeshComponent*> Ferns = SmallPlants(OceanFlora::MakeFern, 850, 200.f);
	TArray<UHierarchicalInstancedStaticMeshComponent*> Heliconias = SmallPlants(OceanFlora::MakeHeliconia, 860, 400.f);
	TArray<UHierarchicalInstancedStaticMeshComponent*> Creepers = SmallPlants(OceanFlora::MakeBeachCreeper, 870, 150.f);
	// Ground cover: dead fronds and coconuts under the palms, grass tufts (yellower in the last variant, for the sunny
	// beach top). Small: culled sooner.
	UMaterialInterface* GrassTuft = GrassTuftMaterial.LoadSynchronous();
	TArray<UHierarchicalInstancedStaticMeshComponent*> FallenFronds, Coconuts, Tufts;
	for (int32 Variant = 0; Variant < NumGroundVariants; ++Variant)
	{
		// Modelled leaflets like the palms' (every one up close, a dozen broad strips a side further away), in the
		// windless stem material: a frond lying on the sand does not flap.
		UStaticMesh* Frond = GetMesh(FString::Printf(TEXT("FallenFrond%d"), Variant), [&]() {
			return TArray<FOceanMeshData>{ OceanFlora::MakeFallenFrond(Seed * 500 + Variant, 0), OceanFlora::MakeFallenFrond(Seed * 500 + Variant, 2) }; },
			{ Bark, Bark }, { 1.f, 0.08f }, { 1, 1 }, true);
		FallenFronds.Add(AddInstancer(Frond, 250.f, 0.f));
		UStaticMesh* Nut = GetMesh(FString::Printf(TEXT("Coconut%d"), Variant), [&]() {
			return TArray<FOceanMeshData>{ AsSecondSection(OceanFlora::MakeCoconut(Seed * 600 + Variant)) }; }, { Bark, Bark }, { 1.f }, { 1 }, false);
		Coconuts.Add(AddInstancer(Nut, 150.f, 0.f));
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
			UStaticMesh* Tuft = GetMesh(FString::Printf(TEXT("GrassTuft%d"), Variant), [&]() { return TArray<FOceanMeshData>{ TuftData }; },
				{ GrassTuft }, { 1.f }, { -1 }, true);
			Tufts.Add(AddInstancer(Tuft, 120.f, 20.f));
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
				Instancers.Add(AddInstancer(Mesh, CullDistanceM, 0.f)); // sea life: still
				SizesM.Add(FMath::Max(float(Mesh->GetBounds().BoxExtent.GetMax()) * 2.f * 0.01f, 0.01f));
			}
		}
	};
	LoadSet(CoralMeshes, 400.f, Corals, CoralSizes);
	LoadSet(UrchinMeshes, 80.f, Urchins, UrchinSizes);
	LoadSet(ShellMeshes, 60.f, Shells, ShellSizes);
	// Caribbean stones (OceanFlora::MakeCaribbeanRock): a few variants of each kind, one instancer per variant; the
	// small ones are culled sooner.
	using OceanFlora::ERockKind;
	constexpr int32 NumRockKinds = 6;
	const int32 VariantsPerKind[NumRockKinds] = { 7, 4, 4, 4, 3, 3 };
	const float RockCullM[NumRockKinds] = { 1500.f, 900.f, 700.f, 900.f, 120.f, 90.f };
	TArray<TArray<int32>> RockVariants;
	RockVariants.SetNum(NumRockKinds);
	for (int32 Kind = 0; Kind < NumRockKinds; ++Kind)
	{
		for (int32 Variant = 0; Variant < VariantsPerKind[Kind]; ++Variant)
		{
			UStaticMesh* Mesh = GetMesh(FString::Printf(TEXT("Rock%d_%d"), Kind, Variant), [&]() {
				return TArray<FOceanMeshData>{ AsSecondSection(OceanFlora::MakeCaribbeanRock(ERockKind(Kind), Seed * 400 + Variant)) }; },
				{ Stone, Stone }, { 1.f }, { 1 }, false);
			RockVariants[Kind].Add(Rocks.Num());
			Rocks.Add(AddInstancer(Mesh, RockCullM[Kind], 0.f));
			RockSizes.Add(FMath::Max(float(Mesh->GetBounds().BoxExtent.GetMax()) * 2.f * 0.01f, 0.01f));
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
		// Taller inland, out of the salt wind, with an emergent standing above the canopy here and there; and coconut
		// palms scattered through the forest, their crowns among the broadleaves.
		const float Shelter = Smooth(30.f, 220.f, Terrain.Inland);
		const float Emergent = Random.FRand() < 0.08f ? 1.4f : 1.f;
		if (Random.FRand() < 0.1f * PalmDensity && Terrain.Ground < 45.f)
		{
			PalmTransforms[Random.RandRange(0, NumPalmVariants - 1)].Add(FTransform(FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f),
				Ground(Position, Terrain.Ground, 0.f), FVector(Random.FRandRange(1.f, 1.35f))));
			return;
		}
		TreeTransforms[Random.RandRange(0, NumTreeVariants - 1)].Add(
			FTransform(FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f), Ground(Position, Terrain.Ground, 30.f),
				FVector(FMath::Lerp(0.85f, 1.6f, Shelter) * Emergent * Random.FRandRange(0.85f, 1.15f))));
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

	// Grass tufts wherever the terrain material paints grass (OceanGroundCover mirrors its masks): a sward as dense as the
	// painted grass, thinning out towards its ragged edges, so the green ground has blades on it instead of looking flat;
	// straw-tipped on the sunny coastal strip.
	if (Tufts.Num() == NumGroundVariants)
	{
		// Slope and distance inland from the cell's centre (they vary slowly), the masks' noises at each tussock.
		auto CellSlope = [&](const FVector2f& Position)
		{
			const float E = 0.5f;
			const float GX = (Surface(Position + FVector2f(E, 0.f)) - Surface(Position - FVector2f(E, 0.f))) / (2.f * E);
			const float GY = (Surface(Position + FVector2f(0.f, E)) - Surface(Position - FVector2f(0.f, E))) / (2.f * E);
			return 1.f - 1.f / FMath::Sqrt(1.f + GX * GX + GY * GY);
		};
		auto GrassAt = [&](const FVector2f& Position, float HeightM, float Slope, float FromShore)
		{
			const FOceanGroundCover Cover = OceanGroundCover(Position, HeightM, Slope, FromShore);
			return Cover.Vegetation * Cover.Grass;
		};
		Scatter(2.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
		{
			if (Terrain.Ground < 0.8f || Terrain.Rock > 0.4f)
			{
				return;
			}
			const float Slope = CellSlope(Position);
			const float FromShore = -Shore->Sample(Position).Y;
			if (GrassAt(Position, Surface(Position), Slope, FromShore) < 0.05f)
			{
				return;
			}
			const bool bCoast = FromShore < 20.f;
			const int32 Tries = FMath::RoundToInt(22.f * GroundCoverDensity); // up to ~5 tussocks per square metre: a sward, not dots
			for (int32 Index = 0; Index < Tries; ++Index)
			{
				const FVector2f Tussock = Position + FVector2f(Random.FRandRange(-1.f, 1.f), Random.FRandRange(-1.f, 1.f));
				const float Height = Surface(Tussock);
				if (Random.FRand() >= GrassAt(Tussock, Height, Slope, FromShore))
				{
					continue;
				}
				const int32 Variant = bCoast && Random.FRand() < 0.5f ? NumGroundVariants - 1 : Random.RandRange(0, NumGroundVariants - 2);
				TuftTransforms[Variant].Add(FTransform(FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f), Ground(Tussock, Height, 0.f),
					FVector(FMath::Lerp(1.1f, 3.f, FMath::Square(Random.FRand())))));
			}
		});
	}

	// --- The other plants of the island ---
	TArray<TArray<FTransform>> AlmondTransforms, AgaveTransforms, TurksCapTransforms, ColumnTransforms, BromeliadTransforms,
		FernTransforms, HeliconiaTransforms, CreeperTransforms;
	for (TArray<TArray<FTransform>>* Set : { &AlmondTransforms, &AgaveTransforms, &TurksCapTransforms, &ColumnTransforms,
		&BromeliadTransforms, &FernTransforms, &HeliconiaTransforms, &CreeperTransforms })
	{
		Set->SetNum(NumSmallPlantVariants);
	}
	auto Plant = [&](TArray<TArray<FTransform>>& Set, const FVector2f& Position, float HeightM, float MinScale, float MaxScale, float SinkCm)
	{
		Set[Random.RandRange(0, Set.Num() - 1)].Add(FTransform(FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f),
			Ground(Position, HeightM, SinkCm), FVector(Random.FRandRange(MinScale, MaxScale))));
	};
	// Tropical almonds: with the palms along the back of the beaches, and here and there in the coastal forest.
	Scatter(9.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
	{
		if (Terrain.Ground < 1.f || Terrain.Rock > 0.35f || Terrain.Inland > 120.f)
		{
			return;
		}
		const float Edge = Terrain.Understory > 0.02f ? 1.f - Smooth(0.6f, 2.f, Terrain.Understory) : 0.f;
		if (Random.FRand() < TreeDensity * (0.45f * Edge + 0.06f))
		{
			Plant(AlmondTransforms, Position, Terrain.Ground, 0.8f, 1.2f, 20.f);
		}
	});
	// Dry, rocky ground near the sea (the rocky points' tops, the scrub behind them): agaves, Turk's caps among the
	// stones, and clumps of columnar cactus.
	Scatter(4.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
	{
		if (Terrain.Ground < 1.f || Terrain.Ground > 40.f || Terrain.Inland > 150.f)
		{
			return;
		}
		const float Dry = FMath::Max(Smooth(0.15f, 0.4f, Terrain.Rock), 0.25f * (1.f - Smooth(0.3f, 1.2f, Terrain.Understory)));
		if (Dry <= 0.f)
		{
			return;
		}
		const float Patch = Smooth(0.35f, 0.65f, OceanGradientNoise(Position / 30.f + FVector2f(71.f, 13.f)));
		const float Roll = Random.FRand();
		if (Roll < 0.22f * Dry * Patch)
		{
			Plant(AgaveTransforms, Position, Surface(Position), 0.7f, 1.4f, 8.f);
		}
		else if (Roll < 0.3f * Dry * Patch)
		{
			Plant(ColumnTransforms, Position, Surface(Position), 0.75f, 1.25f, 5.f);
		}
		else if (Roll < 0.5f * Dry * Patch && Terrain.Rock > 0.2f)
		{
			// Turk's caps come in small groups.
			const int32 Count = Random.RandRange(2, 6);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FVector2f At = Position + FVector2f(Random.FRandRange(-1.5f, 1.5f), Random.FRandRange(-1.5f, 1.5f));
				Plant(TurksCapTransforms, At, Surface(At), 0.7f, 1.3f, 3.f);
			}
		}
	});
	// The forest floor: ferns everywhere under the canopy, thickest in the damp valleys (Flow); bromeliads in clumps;
	// heliconias along the valleys and the forest edges inland.
	Scatter(2.5f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
	{
		if (Terrain.Understory < 0.3f || Terrain.Rock > 0.5f)
		{
			return;
		}
		const float Damp = FMath::Max(Smooth(0.15f, 0.6f, Terrain.Flow), Smooth(15.f, 40.f, Terrain.Ground) * 0.5f);
		const float Roll = Random.FRand();
		const float Height = Surface(Position);
		if (Roll < GroundCoverDensity * (0.18f + 0.35f * Damp))
		{
			Plant(FernTransforms, Position, Height, 0.7f, 1.4f, 5.f);
		}
		else if (Roll < GroundCoverDensity * (0.24f + 0.35f * Damp) && OceanValueNoise(Position / 14.f + FVector2f(5.f, 77.f)) > 0.55f)
		{
			Plant(BromeliadTransforms, Position, Height, 0.8f, 1.3f, 3.f);
		}
		else if (Roll < GroundCoverDensity * (0.24f + 0.35f * Damp + 0.12f * Damp) && Terrain.Ground > 6.f)
		{
			Plant(HeliconiaTransforms, Position, Height, 0.8f, 1.25f, 5.f);
		}
	});
	// Beach morning glory over the upper beach, in patches between the berm and the vegetation line.
	Scatter(2.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
	{
		if (Terrain.Ground < 1.1f || Terrain.Rock > 0.2f || Terrain.Understory > 0.3f || Terrain.Inland > 80.f)
		{
			return;
		}
		const float Patch = Smooth(0.45f, 0.7f, OceanGradientNoise(Position / 18.f + FVector2f(23.f, 51.f)));
		if (Random.FRand() < 0.35f * GroundCoverDensity * Patch)
		{
			Plant(CreeperTransforms, Position, Surface(Position), 0.8f, 1.3f, 0.f);
		}
	});

	// --- Stones, by kind and setting ---
	auto PlaceRock = [&](ERockKind Kind, const FVector2f& Position, float HeightM, float SizeM, float Yaw, float Tilt, float SinkShare)
	{
		const TArray<int32>& Variants = RockVariants[int32(Kind)];
		const int32 Index = Variants[Random.RandRange(0, Variants.Num() - 1)];
		RockTransforms[Index].Add(FTransform(FRotator(Random.FRandRange(-Tilt, Tilt), Yaw, Random.FRandRange(-Tilt, Tilt)),
			Ground(Position, HeightM, SinkShare * 100.f * SizeM), FVector(SizeM / RockSizes[Index])));
	};
	// Seaward direction from the shore data's distance field (positive at sea).
	auto Seaward = [&](const FVector2f& Position)
	{
		const float DX = Shore->Sample(Position + FVector2f(2.f, 0.f)).Y - Shore->Sample(Position - FVector2f(2.f, 0.f)).Y;
		const float DY = Shore->Sample(Position + FVector2f(0.f, 2.f)).Y - Shore->Sample(Position - FVector2f(0.f, 2.f)).Y;
		return FVector2f(DX, DY).GetSafeNormal();
	};
	Scatter(3.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
	{
		const float H = Terrain.Ground;
		if (H < -3.f)
		{
			return;
		}
		// Blocks fallen from the limestone cliffs: piled at their foot and on the rocky points, also under water; along
		// the waterline the spray zone has weathered them into black, razor-jagged ironshore.
		if (Terrain.Rock > 0.15f && H < 16.f)
		{
			if (Random.FRand() < RockDensity * 0.55f * Terrain.Rock)
			{
				const bool bSpray = H > -0.8f && H < 2.5f && Random.FRand() < 0.6f;
				const float SizeM = bSpray ? Random.FRandRange(1.2f, 4.5f) : FMath::Lerp(0.5f, 4.f, FMath::Square(Random.FRand())) * (0.7f + Terrain.Rock);
				PlaceRock(bSpray ? ERockKind::Ironshore : ERockKind::Limestone, Position, H, SizeM, Random.FRandRange(0.f, 360.f), bSpray ? 6.f : 18.f, 0.15f);
			}
			return;
		}
		// Beachrock: along some stretches of the sandy beaches, slabs of cemented sand at the swash line, lying parallel
		// to the shore and tilted gently seaward.
		const float BeachrockStretch = Smooth(0.6f, 0.72f, OceanGradientNoise(Position / 160.f + FVector2f(17.f, 3.f)));
		if (H > -0.6f && H < 0.9f && BeachrockStretch > 0.f)
		{
			if (Random.FRand() < 0.55f * BeachrockStretch)
			{
				const FVector2f Out = Seaward(Position);
				const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Out.Y, Out.X)) + 90.f + Random.FRandRange(-12.f, 12.f);
				PlaceRock(ERockKind::Beachrock, Position, H, Random.FRandRange(1.5f, 4.f), Yaw, 4.f, 0.05f);
			}
			return;
		}
		// A boulder beach: one stretch of the coast strewn with dark rounded volcanic boulders across the upper beach.
		const float BoulderBeach = Smooth(0.7f, 0.8f, OceanGradientNoise(Position / 220.f + FVector2f(41.f, 9.f)));
		if (H > -1.2f && H < 2.5f && BoulderBeach > 0.f)
		{
			if (Random.FRand() < 0.35f * BoulderBeach)
			{
				PlaceRock(ERockKind::Volcanic, Position, H, FMath::Lerp(0.4f, 2.f, FMath::Square(Random.FRand())), Random.FRandRange(0.f, 360.f), 25.f, 0.2f);
			}
			return;
		}
		// Limestone outcrops on the steep slopes inland, half buried; a lone block at the waterline here and there.
		if (H > 5.f && Random.FRand() < 0.05f * RockDensity)
		{
			const float Slope = SlopeAt(Position, H);
			if (Random.FRand() < Smooth(0.25f, 0.8f, Slope))
			{
				PlaceRock(ERockKind::Limestone, Position, H, FMath::Lerp(0.8f, 3.5f, Random.FRand()), Random.FRandRange(0.f, 360.f), 20.f, 0.35f);
			}
			return;
		}
		if (FMath::Abs(H) < 0.6f && Random.FRand() < 0.01f * RockDensity)
		{
			PlaceRock(ERockKind::Limestone, Position, H, Random.FRandRange(0.6f, 2.f), Random.FRandRange(0.f, 360.f), 15.f, 0.2f);
		}
	});
	// Loose stones: limestone cobbles on the rocky points and at the foot of their cliffs; bleached coral rubble along
	// the beaches, at the swash line and on the upper beach (where storms threw it).
	Scatter(2.f, [&](const FVector2f& Position, const FOceanTerrainSample& Terrain)
	{
		const float H = Terrain.Ground;
		if (Terrain.Rock >= 0.25f && H > 0.3f && H < 14.f)
		{
			if (Random.FRand() < 0.3f * GroundCoverDensity * Terrain.Rock)
			{
				const float SizeM = FMath::Lerp(0.12f, 0.55f, FMath::Square(Random.FRand()));
				PlaceRock(Random.FRand() < 0.5f ? ERockKind::Cobble : ERockKind::Limestone, Position, Surface(Position), SizeM,
					Random.FRandRange(0.f, 360.f), 30.f, 0.3f);
			}
			return;
		}
		if (Terrain.Rock < 0.25f && H > -0.3f && H < 2.2f && Terrain.Understory < 0.05f)
		{
			const float Rubble = Smooth(0.45f, 0.7f, OceanGradientNoise(Position / 25.f + FVector2f(61.f, 7.f)));
			if (Random.FRand() < 0.06f * GroundCoverDensity * Rubble)
			{
				PlaceRock(ERockKind::CoralRubble, Position, Surface(Position), FMath::Lerp(0.08f, 0.35f, FMath::Square(Random.FRand())),
					Random.FRandRange(0.f, 360.f), 40.f, 0.3f);
			}
		}
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
	AddAll(Almonds, AlmondTransforms);
	AddAll(Agaves, AgaveTransforms);
	AddAll(TurksCaps, TurksCapTransforms);
	AddAll(Columns, ColumnTransforms);
	AddAll(Bromeliads, BromeliadTransforms);
	AddAll(Ferns, FernTransforms);
	AddAll(Heliconias, HeliconiaTransforms);
	AddAll(Creepers, CreeperTransforms);
	UE_LOG(LogTemp, Display, TEXT("OceanIslandFlora: %d almond, %d agave, %d turk's cap, %d column cactus, %d bromeliad, %d fern, %d heliconia, %d creeper instances"),
		Count(AlmondTransforms), Count(AgaveTransforms), Count(TurksCapTransforms), Count(ColumnTransforms), Count(BromeliadTransforms),
		Count(FernTransforms), Count(HeliconiaTransforms), Count(CreeperTransforms));
	bBuilt = true;
}
