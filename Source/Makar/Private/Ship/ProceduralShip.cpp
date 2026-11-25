#include "Ship/ProceduralShip.h"

#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "ShipFittings.h"
#include "ShipHull.h"
#include "ShipMeshBuilder.h"
#include "ShipRig.h"

DEFINE_LOG_CATEGORY_STATIC(LogShip, Log, All);

/** A generated ship, ready for the mesh component: one section per material. */
struct FShipBuildResult
{
	FShipDesign Design;
	TArray<FProcMeshSection> Sections;
	float Ground = 0.f;
	int32 Triangles = 0;
	int32 Guns = 0;
	double GeometryMs = 0.0;
	FString Times;
};

namespace
{
	/** Pure geometry, no engine objects: runs on any thread. */
	FShipBuildResult Generate(const FShipDesign& Design, bool bCradle, float CradleHeight)
	{
		const double Start = FPlatformTime::Seconds();
		FShipBuildResult Result;
		Result.Design = Design;
		FOceanMeshData Data(ShipMesh::NumSections);
		ShipMesh::FBuilder Builder(Data);
		const FHullLines Lines(Design);
		TArray<FShipGunPort> Ports;
		TArray<FShipMastInfo> Masts;
		Result.Ground = Lines.Z(-Design.KeelDepth);
		Result.Times = FString::Printf(TEXT("lines %.0f ms"), (FPlatformTime::Seconds() - Start) * 1000.0);
		// Each stage's vertices checked: a NaN anywhere makes the whole mesh's bounds NaN (and the ship invisible).
		auto Stage = [&](const TCHAR* Name, TFunctionRef<void()> Run)
		{
			const int32 First = Data.Vertices.Num();
			const double T0 = FPlatformTime::Seconds();
			Run();
			Result.Times += FString::Printf(TEXT(", %s %.0f ms"), Name, (FPlatformTime::Seconds() - T0) * 1000.0);
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
		if (bCradle)
		{
			Stage(TEXT("cradle"), [&]() { Result.Ground = ShipFittings::Cradle(Builder, Lines, CradleHeight); });
		}
		Result.Guns = 2 * Ports.Num();

		// Mesh sections: each material's triangles with their own vertices. The vertex colour is stored as it is (the
		// vertex factory reads the bytes without decoding): the materials get the linear albedo they were given.
		const double T0 = FPlatformTime::Seconds();
		Result.Sections.SetNum(Data.Sections.Num());
		ParallelFor(Data.Sections.Num(), [&](int32 SectionIndex)
		{
			const TArray<int32>& Indices = Data.Sections[SectionIndex];
			FProcMeshSection& Section = Result.Sections[SectionIndex];
			TArray<int32> Remap;
			Remap.Init(INDEX_NONE, Data.Vertices.Num());
			Section.ProcIndexBuffer.Reserve(Indices.Num());
			for (int32 Index : Indices)
			{
				int32& Mapped = Remap[Index];
				if (Mapped == INDEX_NONE)
				{
					const FOceanMeshData::FVertex& V = Data.Vertices[Index];
					FProcMeshVertex& Out = Section.ProcVertexBuffer.AddDefaulted_GetRef();
					Out.Position = FVector(V.Position);
					Out.Normal = FVector(V.Normal);
					const FVector3f Helper = FMath::Abs(V.Normal.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector;
					Out.Tangent = FProcMeshTangent(FVector(FVector3f::CrossProduct(Helper, V.Normal).GetSafeNormal()), false);
					auto Byte = [](float C) { return uint8(FMath::Clamp(FMath::RoundToInt(C * 255.f), 0, 255)); };
					Out.Color = FColor(Byte(V.Color.R), Byte(V.Color.G), Byte(V.Color.B), Byte(V.Color.A));
					Out.UV0 = FVector2D(V.UV);
					Section.SectionLocalBox += Out.Position;
					Mapped = Section.ProcVertexBuffer.Num() - 1;
				}
				Section.ProcIndexBuffer.Add(uint32(Mapped));
			}
			Section.bEnableCollision = false;
			Section.bSectionVisible = true;
		});
		for (const TArray<int32>& Indices : Data.Sections)
		{
			Result.Triangles += Indices.Num() / 3;
		}
		Result.Times += FString::Printf(TEXT(", sections %.0f ms"), (FPlatformTime::Seconds() - T0) * 1000.0);
		Result.GeometryMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		return Result;
	}
}

AProceduralShip::AProceduralShip()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

UProceduralMeshComponent* AProceduralShip::GetOrCreateMesh()
{
	if (!Mesh)
	{
		Mesh = NewObject<UProceduralMeshComponent>(this, TEXT("Ship"), RF_Transient);
		Mesh->SetupAttachment(RootComponent);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->RegisterComponent();
	}
	return Mesh;
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
	if (!bBuilt || BuiltKey != Key())
	{
		Rebuild();
	}
}

void AProceduralShip::BeginPlay()
{
	Super::BeginPlay();
	if (!Mesh || Mesh->GetNumSections() == 0)
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
	return Mesh && Mesh->GetNumSections() > 0 ? Mesh->Bounds.GetBox() : FBox(GetActorLocation(), GetActorLocation());
}

void AProceduralShip::BuildFromDesign(const FShipDesign& InDesign)
{
	++Ticket; // a pending asynchronous build is now stale
	FShipBuildResult Result = Generate(InDesign, bOnCradle, CradleHeight);
	ApplyResult(Result);
}

void AProceduralShip::BuildFromDesignAsync(const FShipDesign& InDesign, TFunction<void()> OnDone)
{
	const int32 MyTicket = ++Ticket;
	const bool bCradle = bOnCradle;
	const float Height = CradleHeight;
	TWeakObjectPtr<AProceduralShip> Self(this);
	Async(EAsyncExecution::ThreadPool, [Self, MyTicket, InDesign, bCradle, Height, OnDone = MoveTemp(OnDone)]() mutable
	{
		TSharedRef<FShipBuildResult> Result = MakeShared<FShipBuildResult>(Generate(InDesign, bCradle, Height));
		AsyncTask(ENamedThreads::GameThread, [Self, MyTicket, Result, OnDone = MoveTemp(OnDone)]()
		{
			AProceduralShip* Ship = Self.Get();
			if (!Ship || Ship->Ticket != MyTicket)
			{
				return; // destroyed, or a newer request is under way
			}
			Ship->ApplyResult(*Result);
			if (OnDone)
			{
				OnDone();
			}
		});
	});
}

void AProceduralShip::ApplyResult(FShipBuildResult& Result)
{
	const double Start = FPlatformTime::Seconds();
	Design = Result.Design;
	Type = Design.Type;
	Seed = Design.Seed;
	bSailsSet = Design.bSailsSet;
	static const TCHAR* const Paths[ShipMesh::NumSections] = {
		TEXT("/Game/Ship/M_ShipWood.M_ShipWood"), TEXT("/Game/Ship/M_ShipSail.M_ShipSail"), TEXT("/Game/Ship/M_ShipRope.M_ShipRope"),
		TEXT("/Game/Ship/M_ShipMetal.M_ShipMetal"), TEXT("/Game/Ship/M_ShipGloss.M_ShipGloss") };
	GetOrCreateMesh();
	Mesh->ClearAllMeshSections();
	for (int32 Index = 0; Index < Result.Sections.Num(); ++Index)
	{
		Mesh->SetProcMeshSection(Index, Result.Sections[Index]);
		Mesh->SetMaterial(Index, LoadObject<UMaterialInterface>(nullptr, Paths[FMath::Min(Index, ShipMesh::NumSections - 1)]));
	}
	// The ground (or the keel) at the actor's origin.
	Mesh->SetRelativeLocation(FVector(0.0, 0.0, bOnCradle ? -Result.Ground * 100.0 : 0.0));
	Mesh->UpdateBounds();
	Triangles = Result.Triangles;
	bBuilt = true;
	BuiltKey = Key();
	const double MeshMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	LastBuildMs = float(Result.GeometryMs + MeshMs);
	UE_LOG(LogShip, Display, TEXT("ProceduralShip: %s seed %d, %.1f x %.1f m, %d guns, %d triangles, %.0f ms (%s, mesh %.0f ms)"),
		*UEnum::GetValueAsString(Type), Seed, Design.Length, Design.Beam, Result.Guns, Triangles, LastBuildMs, *Result.Times, MeshMs);
}
