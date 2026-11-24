#include "Ship/ProceduralShip.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Ocean/OceanFlora.h"
#include "ShipFittings.h"
#include "ShipHull.h"
#include "ShipMeshBuilder.h"
#include "ShipRig.h"

DEFINE_LOG_CATEGORY_STATIC(LogShip, Log, All);

AProceduralShip::AProceduralShip()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Ship"));
	Mesh->SetupAttachment(RootComponent);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetMobility(EComponentMobility::Movable);
}

uint32 AProceduralShip::Key() const
{
	uint32 Hash = GetTypeHash(int32(Type));
	Hash = HashCombine(Hash, GetTypeHash(Seed));
	Hash = HashCombine(Hash, GetTypeHash(bSailsSet));
	Hash = HashCombine(Hash, GetTypeHash(bOnCradle));
	Hash = HashCombine(Hash, GetTypeHash(CradleHeight));
	return Hash;
}

void AProceduralShip::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	// Moving the actor calls this too: build only when the ship itself changed.
	if (!Generated || BuiltKey != Key())
	{
		Rebuild();
	}
}

void AProceduralShip::BeginPlay()
{
	Super::BeginPlay();
	// Play in editor keeps no reference to the editor's transient mesh.
	if (!Mesh->GetStaticMesh())
	{
		Rebuild();
	}
}

void AProceduralShip::Rebuild()
{
	FShipDesign Made = FShipDesign::Make(Type, Seed);
	Made.bSailsSet = bSailsSet;
	BuildFromDesign(Made);
}

FBox AProceduralShip::GetShipBounds() const
{
	return Mesh && Mesh->GetStaticMesh() ? Mesh->Bounds.GetBox() : FBox(GetActorLocation(), GetActorLocation());
}

void AProceduralShip::BuildFromDesign(const FShipDesign& InDesign)
{
	const double Start = FPlatformTime::Seconds();
	Design = InDesign;
	Type = Design.Type;
	Seed = Design.Seed;
	bSailsSet = Design.bSailsSet;

	FOceanMeshData Data(ShipMesh::NumSections);
	ShipMesh::FBuilder Builder(Data);
	const FHullLines Lines(Design);
	TArray<FShipGunPort> Ports;
	TArray<FShipMastInfo> Masts;
	float Ground = Lines.Z(-Design.KeelDepth);
	// Each stage's vertices checked: a NaN anywhere makes the whole mesh's bounds NaN (and the ship invisible).
	auto Stage = [&](const TCHAR* Name, TFunctionRef<void()> Run)
	{
		const int32 First = Data.Vertices.Num();
		Run();
		int32 Bad = 0;
		for (int32 I = First; I < Data.Vertices.Num(); ++I)
		{
			FOceanMeshData::FVertex& V = Data.Vertices[I];
			if (V.Position.ContainsNaN() || V.Normal.ContainsNaN())
			{
				++Bad;
				V.Position = V.Position.ContainsNaN() ? FVector3f::ZeroVector : V.Position;
				V.Normal = V.Normal.ContainsNaN() ? FVector3f::UpVector : V.Normal;
			}
		}
		if (Bad > 0)
		{
			UE_LOG(LogShip, Warning, TEXT("ProceduralShip: %d NaN vertices in %s (zeroed)"), Bad, Name);
		}
	};
	Stage(TEXT("hull"), [&]() { ShipHull::Build(Builder, Lines, Ports); });
	Stage(TEXT("rig"), [&]() { ShipRig::Build(Builder, Lines, Masts); });
	Stage(TEXT("fittings"), [&]() { ShipFittings::Build(Builder, Lines, Ports, Masts); });
	if (bOnCradle)
	{
		Stage(TEXT("cradle"), [&]() { Ground = ShipFittings::Cradle(Builder, Lines, CradleHeight); });
	}

	static const TCHAR* const Paths[ShipMesh::NumSections] = {
		TEXT("/Game/Ship/M_ShipWood.M_ShipWood"), TEXT("/Game/Ship/M_ShipSail.M_ShipSail"), TEXT("/Game/Ship/M_ShipRope.M_ShipRope"),
		TEXT("/Game/Ship/M_ShipMetal.M_ShipMetal"), TEXT("/Game/Ship/M_ShipGloss.M_ShipGloss") };
	TArray<UMaterialInterface*> Materials;
	for (const TCHAR* Path : Paths)
	{
		Materials.Add(LoadObject<UMaterialInterface>(nullptr, Path));
	}
	Triangles = 0;
	for (const TArray<int32>& Section : Data.Sections)
	{
		Triangles += Section.Num() / 3;
	}
	Generated = OceanFlora::BuildStaticMesh(this, Data, Materials, 0);
	Mesh->SetStaticMesh(Generated);
	// The ground (or the keel) at the actor's origin.
	Mesh->SetRelativeLocation(FVector(0.0, 0.0, bOnCradle ? -Ground * 100.0 : 0.0));
	Mesh->UpdateBounds();
	BuiltKey = Key();
	UE_LOG(LogShip, Display, TEXT("ProceduralShip: %s seed %d, %.1f x %.1f m, %d guns, %d triangles, %.2f s"),
		*UEnum::GetValueAsString(Type), Seed, Design.Length, Design.Beam, 2 * Ports.Num(), Triangles, FPlatformTime::Seconds() - Start);
}
