#include "Ship/ProceduralShip.h"

#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "Ship/ShipCollision.h"
#include "ShipFittings.h"
#include "ShipHull.h"
#include "ShipMeshBuilder.h"
#include "SailCloth.h"
#include "ShipRig.h"

DEFINE_LOG_CATEGORY_STATIC(LogShip, Log, All);

/** A generated ship, ready for the mesh component: one section per material. */
struct FShipBuildResult
{
	FShipDesign Design;
	TArray<FProcMeshSection> Sections;
	TArray<FShipSail> Sails;
	TArray<FShipCapsule> Colliders;
	/** What the sails rest on: the hull's walking solids and the rig's blocks. */
	TArray<FShipBlock> Solids;
	FShipHullShape HullShape;
	/** How the sails were fitted (the success criteria). */
	int32 FitFails = 0;
	uint32 FitMasts = 0;
	float FitReduced = 0.f;
	FString FitText;
	/** The solids the sails were fitted against (diagnostics). */
	TArray<FShipBlock> FitObstacles;
	TSharedPtr<FShipVoxels> Voxels;
	FShipInterior Interior;
	float Ground = 0.f;
	int32 Triangles = 0;
	int32 Guns = 0;
	double GeometryMs = 0.0;
	FString Times;
};

namespace
{
	/** Pure geometry, no engine objects: runs on any thread. */
	FShipBuildResult GenerateOnce(const FShipDesign& Design, bool bCradle, float CradleHeight)
	{
		const double Start = FPlatformTime::Seconds();
		FShipBuildResult Result;
		Result.Design = Design;
		FOceanMeshData Data(ShipMesh::NumSections);
		ShipMesh::FBuilder Builder(Data);
		// The design as trimmed: the crew may ease a mast's trim below (the lines refer to it).
		FShipDesign Trial = Design;
		const FHullLines Lines(Trial);
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
		Stage(TEXT("hull"), [&]() { ShipHull::Build(Builder, Lines, Ports, Result.Interior); });
		TArray<FShipRope> Ropes;
		TArray<FShipBlock> Blocks;
		TArray<FShipBlock> Pieces;
		// The hull stays as built; if the sails do not fit as trimmed, the crew eases the masts whose sails fail (a
		// quarter less each time, three times) and only the rig, fittings, ropes and sails are made again.
		const int32 HullVertices = Data.Vertices.Num();
		TArray<int32> HullIndices;
		for (const TArray<int32>& Section : Data.Sections)
		{
			HullIndices.Add(Section.Num());
		}
		FString Eased;
		for (int32 Try = 0; ; ++Try)
		{
		if (Try > 0)
		{
			Data.Vertices.SetNum(HullVertices);
			for (int32 Section = 0; Section < Data.Sections.Num(); ++Section)
			{
				Data.Sections[Section].SetNum(HullIndices[Section]);
			}
			Result.Colliders.Reset();
			Result.Sails.Reset();
			Ropes.Reset();
			Blocks.Reset();
			Pieces.Reset();
			Masts.Reset();
		}
		Stage(TEXT("rig"), [&]()
		{
			// The spars and blocks recorded as obstacles, the ropes kept to be laid; the sails set come out as cloth.
			Builder.Colliders = &Result.Colliders;
			Builder.Ropes = &Ropes;
			Builder.Blocks = &Blocks;
			Builder.Pieces = &Pieces;
			ShipRig::Build(Builder, Lines, Masts, &Result.Sails);
			Builder.Colliders = nullptr;
			Builder.Blocks = nullptr;
		});
		Stage(TEXT("fittings"), [&]()
		{
			// The fittings' solid pieces (bitts, belfry, carriages...) recorded too: the sails are fitted clear of them.
			Builder.Blocks = &Blocks;
			Builder.bLatheBlocks = true;
			ShipFittings::Build(Builder, Lines, Ports, Masts, Result.Interior);
			Builder.bLatheBlocks = false;
			Builder.Blocks = nullptr;
			Builder.Pieces = nullptr;
		});
		int32 Crossing = 0;
		Stage(TEXT("ropes"), [&]()
		{
			// Every rope laid round the spars and blocks; laid, they are obstacles for the sails too.
			Builder.Ropes = nullptr;
			TArray<FShipCapsule> Spars;
			for (const FShipCapsule& C : Result.Colliders)
			{
				if (C.bSpar && C.Radius >= 0.04f)
				{
					Spars.Add(C);
				}
			}
			Builder.Colliders = &Result.Colliders;
			Crossing = ShipRopes::Lay(Builder, Ropes, Spars, Blocks);
			Builder.Colliders = nullptr;
		});
		Result.Times += FString::Printf(TEXT(", %d ropes, %d rope points still inside a solid"), Ropes.Num(), Crossing);
		if (bCradle)
		{
			Stage(TEXT("cradle"), [&]() { Result.Ground = ShipFittings::Cradle(Builder, Lines, CradleHeight); });
		}
		Result.Guns = 2 * Ports.Num();
		// The sails rest on what is drawn (the voxels below) and on the rig's blocks (tops, caps: above the voxels).
		Result.Solids = Blocks;
		// The hull's shape for the sails, and its rails as lines they rest against (a rail would slip between two
		// points of the canvas).
		{
			FShipHullShape& Shape = Result.HullShape;
			Shape.DX = 0.25f;
			Shape.DH = 0.1f;
			Shape.X0 = -Lines.HalfLength - 0.5f;
			Shape.NX = FMath::CeilToInt((2.f * Lines.HalfLength + 1.f) / Shape.DX) + 1;
			float Top = 0.f;
			Shape.Rail.SetNum(Shape.NX);
			for (int32 I = 0; I < Shape.NX; ++I)
			{
				const float X = FMath::Clamp(Shape.X0 + I * Shape.DX, -Lines.HalfLength, Lines.HalfLength);
				Shape.Rail[I] = Lines.RailAt(X);
				Top = FMath::Max(Top, Shape.Rail[I]);
			}
			Shape.H0 = 0.f;
			Shape.NH = FMath::CeilToInt((Top + 0.2f) / Shape.DH) + 1;
			Shape.ZOffset = Lines.Z(0.f);
			Shape.Wall = Lines.FrameThickness + Design.PlankThickness;
			Shape.Half.SetNum(Shape.NX * Shape.NH);
			for (int32 J = 0; J < Shape.NH; ++J)
			{
				for (int32 I = 0; I < Shape.NX; ++I)
				{
					const float X = Shape.X0 + I * Shape.DX;
					const float H = FMath::Min(Shape.H0 + J * Shape.DH, Shape.Rail[I]);
					const float B = Lines.HalfBreadth(X, H);
					Shape.Half[J * Shape.NX + I] = B > 0.f ? B + Design.PlankThickness : 0.f;
				}
			}
			for (float Side : { -1.f, 1.f })
			{
				FVector3f Last = FVector3f::ZeroVector;
				bool bLast = false;
				for (float X = -Lines.HalfLength; X <= Lines.HalfLength + 0.01f; X += 0.5f)
				{
					const float Rail = Lines.RailAt(X);
					const float B = Lines.HalfBreadth(X, Rail);
					const FVector3f P(X, Side * (B + Design.PlankThickness * 0.5f), Lines.Z(Rail));
					if (B > 0.05f && bLast)
					{
						FShipCapsule Capsule;
						Capsule.A = Last;
						Capsule.B = P;
						Capsule.Radius = 0.08f;
						Result.Colliders.Add(Capsule);
					}
					Last = P;
					bLast = B > 0.05f;
				}
			}
		}

		// The sails fitted into the ship once, as an artist would: clear of everything, each point given its room.
		if (Result.Sails.Num() > 0)
		{
			// Obstacles: the rig's and fittings' blocks, and the decks, rails, balustrades, bulkheads one walks among.
			TArray<FShipBlock> Obstacles = Blocks;
			Obstacles.Append(Pieces);
			Result.FitObstacles = Obstacles;
			Obstacles.Append(Result.Interior.Collision);
			const ShipSails::FFitReport Fit = ShipSails::Fit(Result.Sails, Result.Colliders, Obstacles, Result.HullShape);
			Result.FitFails = Fit.ShapeFails;
			Result.FitMasts = Fit.FailMasts;
			Result.FitReduced = Fit.Reduced;
			Result.FitText = Fit.Text;
			Result.Times += TEXT(", ") + Fit.Text;
		}
		if (Result.FitFails == 0 || Try >= 3)
		{
			break;
		}
		if (Try == 0)
		{
			const int32 At = Result.FitText.Find(TEXT("; fails: "));
			Eased = FString::Printf(TEXT(" [at full trim: %d fails%s]"), Result.FitFails, At != INDEX_NONE ? *Result.FitText.Mid(At, 400) : TEXT(""));
		}
		for (int32 Mast = 0; Mast < 4; ++Mast)
		{
			if (Result.FitMasts & (1u << Mast))
			{
				Trial.MastEase[Mast] *= 0.75f;
			}
		}
		Result.Times += FString::Printf(TEXT(", trim eased to %.0f/%.0f/%.0f/%.0f%%"), Trial.MastEase[0] * 100.f, Trial.MastEase[1] * 100.f, Trial.MastEase[2] * 100.f, Trial.MastEase[3] * 100.f);
		}
		Result.Design = Trial;
		Result.FitText += Eased;

		// Verification only (MAKAR_VERIFY_SAILS=1): everything drawn, in 7 cm cubes (sparse), over the whole ship from a
		// metre under the waterline to the trucks, to measure that the canvas crosses nothing (wood, iron, ropes...).
		if (Result.Sails.Num() > 0 && !FPlatformMisc::GetEnvironmentVariable(TEXT("MAKAR_VERIFY_SAILS")).IsEmpty())
		{
			const double TV = FPlatformTime::Seconds();
			const int32 Drawn[] = { int32(ShipMesh::Wood), 1, 2, int32(ShipMesh::Metal), 4 };
			FBox3f Bounds(ForceInit);
			int32 Wild = 0;
			for (const FOceanMeshData::FVertex& V : Data.Vertices)
			{
				// Only sane points (a stray infinite one would make the grid absurd).
				if (V.Position.GetAbsMax() < 30000.f)
				{
					Bounds += V.Position * 0.01f; // the mesh is in centimetres, the sails in metres
				}
				else
				{
					++Wild;
				}
			}
			if (Wild > 0)
			{
				UE_LOG(LogShip, Warning, TEXT("ProceduralShip: %d vertices out of all reason (beyond 300 m)"), Wild);
			}
			Bounds.Min.Z = FMath::Max(Bounds.Min.Z, -1.f);
			Bounds = Bounds.ExpandBy(0.2f);
			Result.Voxels = MakeShared<FShipVoxels>();
			FShipVoxels& G = *Result.Voxels;
			G.Reset(Bounds, 0.07f);
			auto EachTriangle = [&](TFunctionRef<void(const FVector3f&, const FVector3f&, const FVector3f&, int32)> Visit)
			{
				for (int32 Section : Drawn)
				{
					if (Section >= Data.Sections.Num())
					{
						continue;
					}
					const TArray<int32>& Indices = Data.Sections[Section];
					ParallelFor((Indices.Num() / 3 + 255) / 256, [&](int32 Chunk)
					{
						const int32 End = FMath::Min((Chunk + 1) * 256, Indices.Num() / 3);
						for (int32 T = Chunk * 256; T < End; ++T)
						{
							Visit(Data.Vertices[Indices[3 * T]].Position * 0.01f, Data.Vertices[Indices[3 * T + 1]].Position * 0.01f, Data.Vertices[Indices[3 * T + 2]].Position * 0.01f, Section);
						}
					});
				}
			};
			EachTriangle([&](const FVector3f& A, const FVector3f& B, const FVector3f& C, int32) { G.Touch(A, B, C); });
			G.Allocate();
			EachTriangle([&](const FVector3f& A, const FVector3f& B, const FVector3f& C, int32 Section) { G.Fill(A, B, C, uint8(4 << Section)); });
			G.Grow();
			Result.Times += FString::Printf(TEXT(", voxels %.0f ms (%d bricks, %.0f MB)"), (FPlatformTime::Seconds() - TV) * 1000.0, G.NumBricks(), G.Cells.Num() / 1048576.0);
		}

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

	FShipBuildResult Generate(const FShipDesign& Design, bool bCradle, float CradleHeight)
	{
		return GenerateOnce(Design, bCradle, CradleHeight);
	}
}

AProceduralShip::AProceduralShip()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

UProceduralMeshComponent* AProceduralShip::GetOrCreateMesh()
{
	if (!Mesh)
	{
		Mesh = NewObject<UProceduralMeshComponent>(this, TEXT("Ship"), RF_Transient);
		Mesh->SetupAttachment(RootComponent);
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
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

FShipDesign AProceduralShip::Trimmed(const FShipDesign& InDesign) const
{
	FShipDesign Out = InDesign;
	Out.WindFrom = WindFrom;
	Out.TrimEase = 1.f;
	for (float& Ease : Out.MastEase)
	{
		Ease = 1.f;
	}
	return Out;
}

void AProceduralShip::BuildFromDesign(const FShipDesign& InDesign)
{
	++Ticket; // a pending asynchronous build is now stale
	bBuildPending = false;
	FShipBuildResult Result = Generate(Trimmed(InDesign), bOnCradle, CradleHeight);
	ApplyResult(Result);
}

void AProceduralShip::BuildFromDesignAsync(const FShipDesign& InDesign_, TFunction<void()> OnDone)
{
	const FShipDesign InDesign = Trimmed(InDesign_);
	const int32 MyTicket = ++Ticket;
	bBuildPending = true;
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
			Ship->bBuildPending = false;
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
	// The sails: their fitted shapes, moving each frame within their room, in their own section.
	Sails = MakeShared<FSailAnimation>();
	Sails->Init(MoveTemp(Result.Sails), Result.Voxels);
	SimulationTime = 0.f;
	if (Sails->CanVerify())
	{
		// Verification (MAKAR_VERIFY_SAILS=1): the whole movement played against everything drawn.
		int32 Inside = 0, Crossings = 0;
		FVector3f First = FVector3f(1e9f);
		FString Cycle = Sails->VerifyCycle(48, 12.f, Inside, Crossings, &First);
		if (Inside > 0)
		{
			// The solid of the fitting nearest the first sample found inside something drawn.
			float Best = 1e9f;
			const FShipBlock* Near = nullptr;
			for (const FShipBlock& K : Result.FitObstacles)
			{
				const float D = (K.O - First).Size() - K.Half.GetMax();
				if (D < Best)
				{
					Best = D;
					Near = &K;
				}
			}
			if (Near)
			{
				Cycle += FString::Printf(TEXT(" nearest fitted solid: block (%.2f %.2f %.2f) half (%.2f %.2f %.2f), %.2f m"), Near->O.X, Near->O.Y, Near->O.Z, Near->Half.X, Near->Half.Y, Near->Half.Z, Best);
			}
		}
		UE_LOG(LogShip, Display, TEXT("SAILCHECK %s seed %d wind %.0f trim %.0f%%: %s fit fails %d, cut %.1f%%, inside %d, crossings %d | %s | %s"),
			*UEnum::GetValueAsString(Result.Design.Type), Result.Design.Seed, Result.Design.WindFrom, FMath::Min(FMath::Min(Result.Design.MastEase[0], Result.Design.MastEase[1]), FMath::Min(Result.Design.MastEase[2], Result.Design.MastEase[3])) * Result.Design.TrimEase * 100.f,
			(Result.FitFails == 0 && Inside == 0 && Crossings == 0 && Result.FitReduced < 0.05f) ? TEXT("PASS") : TEXT("FAIL"),
			Result.FitFails, Result.FitReduced * 100.f, Inside, Crossings, *Cycle, *Result.FitText);
	}
	Sails->Evaluate(0.f, SailStrength(), WindTowards(), -1.f, SailPace(), FMath::SmoothStep(7.f, 22.f, WindSpeed));
	if (!Sails->IsEmpty())
	{
		TArray<int32> SailTriangles;
		TArray<FVector2D> UV;
		TArray<FColor> Colors;
		Sails->GetTopology(SailTriangles, UV, Colors);
		TArray<FVector> Positions, Normals;
		Sails->GetSurface(Positions, Normals);
		Mesh->CreateMeshSection(ShipMesh::NumSections, Positions, SailTriangles, Normals, UV, Colors, {}, false);
		Mesh->SetMaterial(ShipMesh::NumSections, LoadObject<UMaterialInterface>(nullptr, Paths[ShipMesh::Sail]));
		Result.Triangles += SailTriangles.Num() / 3;
		UpdateSails();
	}
	// Collision for walking aboard: the interior's boxes as physics box primitives (no cooking).
	{
		if (!Collision)
		{
			Collision = NewObject<UShipCollisionComponent>(this, TEXT("Collision"), RF_Transient);
			Collision->SetupAttachment(Mesh);
			Collision->RegisterComponent();
		}
		TArray<UShipCollisionComponent::FBox3> Boxes;
		Boxes.Reserve(Result.Interior.Collision.Num());
		for (const FShipBlock& K : Result.Interior.Collision)
		{
			const FMatrix Axes(FVector(K.X), FVector(K.Y), FVector(K.Z), FVector::ZeroVector);
			Boxes.Add({ FVector(K.O * 100.f), Axes.Rotator(), FVector(K.Half * 100.f) });
		}
		Collision->SetBoxes(Boxes);
		// Diagnostics: the largest boxes and any not finite.
		{
			int32 Bad = 0;
			TArray<TPair<float, int32>> Sizes;
			for (int32 I = 0; I < Result.Interior.Collision.Num(); ++I)
			{
				const FShipBlock& K = Result.Interior.Collision[I];
				if (K.O.ContainsNaN() || K.Half.ContainsNaN() || K.X.ContainsNaN() || K.Z.ContainsNaN())
				{
					++Bad;
				}
				Sizes.Add({ K.Half.GetMax(), I });
			}
			Sizes.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B) { return A.Key > B.Key; });
			for (int32 I = 0; I < FMath::Min(3, Sizes.Num()); ++I)
			{
				const FShipBlock& K = Result.Interior.Collision[Sizes[I].Value];
				UE_LOG(LogShip, Display, TEXT("ProceduralShip: collision box at %s half %s axes %s / %s"), *K.O.ToString(), *K.Half.ToString(), *K.X.ToString(), *K.Z.ToString());
			}
			if (Bad > 0)
			{
				UE_LOG(LogShip, Warning, TEXT("ProceduralShip: %d collision boxes not finite"), Bad);
			}
		}
		Boarding = FVector(Result.Interior.Boarding * 100.f);
		CollisionBoxes = Boxes.Num();
	}
	Triangles = Result.Triangles;
	bBuilt = true;
	BuiltKey = Key();
	const double MeshMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	LastBuildMs = float(Result.GeometryMs + MeshMs);
	UE_LOG(LogShip, Display, TEXT("ProceduralShip: %s seed %d, %.1f x %.1f m, %d guns, %d triangles, %d collision boxes, %.0f ms (%s, mesh %.0f ms)"),
		*UEnum::GetValueAsString(Type), Seed, Design.Length, Design.Beam, Result.Guns, Triangles, CollisionBoxes, LastBuildMs, *Result.Times, MeshMs);
}

void AProceduralShip::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Sails.IsValid() || Sails->IsEmpty() || !Mesh)
	{
		return;
	}
	SimulationTime += DeltaSeconds;
	// The crew trims the rig to the wind: when it has shifted enough to brace the yards round, the rig is laid again.
	if (!bBuildPending && FPlatformTime::Seconds() - LastTrimTime > 0.4)
	{
		float Brace0, Lee0, Swing0, Brace1, Lee1, Swing1;
		bool bAback0, bAback1;
		FShipDesign::Trim(Design.WindFrom, Brace0, Lee0, Swing0, bAback0);
		FShipDesign::Trim(WindFrom, Brace1, Lee1, Swing1, bAback1);
		if (Lee0 != Lee1 || bAback0 != bAback1 || FMath::Abs(Brace0 - Brace1) > 3.f || FMath::Abs(Swing0 - Swing1) > 5.f)
		{
			LastTrimTime = FPlatformTime::Seconds();
			BuildFromDesignAsync(Design, nullptr);
		}
	}
	Sails->Evaluate(SimulationTime, SailStrength(), WindTowards(), -1.f, SailPace(), FMath::SmoothStep(7.f, 22.f, WindSpeed));
	UpdateSails();
}

FString AProceduralShip::DescribeSails() const
{
	return Sails.IsValid() ? Sails->Describe() : FString();
}

void AProceduralShip::UpdateSails()
{
	TArray<FVector> Positions, Normals;
	Sails->GetSurface(Positions, Normals);
	Mesh->UpdateMeshSection(ShipMesh::NumSections, Positions, Normals, {}, {}, {});
}
