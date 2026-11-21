#include "Ocean/OceanActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "Rendering/Texture2DResource.h"
#include "TextureResource.h"

namespace OceanParams
{
	const FName Displacement[] = { TEXT("Disp0"), TEXT("Disp1"), TEXT("Disp2") };
	const FName Derivatives[] = { TEXT("Deriv0"), TEXT("Deriv1"), TEXT("Deriv2") };
	const FName PatchSize[] = { TEXT("PatchSize0"), TEXT("PatchSize1"), TEXT("PatchSize2") };
	const FName Resolution(TEXT("Resolution"));
	const FName GridSpacing(TEXT("GridSpacing"));
	const FName GridInnerHalfWidth(TEXT("GridInnerHalfWidth"));
	const FName GridGrowth(TEXT("GridGrowth"));
	const FName FadeStart(TEXT("FadeStart"));
	const FName FadeEnd(TEXT("FadeEnd"));
}

namespace
{
	UTexture2D* CreateOceanTexture(int32 Size, int32 NumMips)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(Size, Size, PF_FloatRGBA);
		if (!Texture)
		{
			return nullptr;
		}
		Texture->SRGB = false;
		Texture->AddressX = TA_Wrap;
		Texture->AddressY = TA_Wrap;
		Texture->Filter = TF_Trilinear;
		Texture->CompressionSettings = TC_HDR;

		// CreateTransient only allocates mip 0.
		FTexturePlatformData* PlatformData = Texture->GetPlatformData();
		for (int32 Mip = 1; Mip < NumMips; ++Mip)
		{
			const int32 MipSize = FMath::Max(Size >> Mip, 1);
			FTexture2DMipMap* MipMap = new FTexture2DMipMap(MipSize, MipSize, 1);
			MipMap->BulkData.Lock(LOCK_READ_WRITE);
			void* Data = MipMap->BulkData.Realloc(int64(MipSize) * MipSize * sizeof(FFloat16Color));
			FMemory::Memzero(Data, int64(MipSize) * MipSize * sizeof(FFloat16Color));
			MipMap->BulkData.Unlock();
			PlatformData->Mips.Add(MipMap);
		}

		Texture->UpdateResource();
		return Texture;
	}

	void UploadMips(UTexture2D* Texture, const TArray<TArray<FFloat16Color>>& Mips)
	{
		if (!Texture)
		{
			return;
		}
		for (int32 Mip = 0; Mip < Mips.Num(); ++Mip)
		{
			const int32 Size = int32(FMath::Sqrt(float(Mips[Mip].Num())));
			const int64 Bytes = int64(Mips[Mip].Num()) * int64(sizeof(FFloat16Color));

			// The render thread reads the data later: give it its own copy.
			uint8* Copy = static_cast<uint8*>(FMemory::Malloc(Bytes));
			FMemory::Memcpy(Copy, Mips[Mip].GetData(), Bytes);
			FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Size, Size);

			Texture->UpdateTextureRegions(Mip, 1, Region, uint32(Size * sizeof(FFloat16Color)), uint32(sizeof(FFloat16Color)), Copy,
				[](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
				{
					FMemory::Free(SrcData);
					delete Regions;
				});
		}
	}
}

AOceanActor::AOceanActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	OceanMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("OceanMesh"));
	OceanMesh->SetupAttachment(RootComponent);
	OceanMesh->SetUsingAbsoluteLocation(true); // follows the camera, not the actor
	OceanMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OceanMesh->SetCastShadow(false);
	OceanMesh->bAffectDistanceFieldLighting = false;

	OceanMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Ocean/M_Ocean.M_Ocean")));
}

void AOceanActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	if (!bMeshBuilt)
	{
		BuildMesh();
	}
	FollowCamera();
}

void AOceanActor::BeginPlay()
{
	Super::BeginPlay();
	bNeedsInitialization = true;
}

void AOceanActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	WaitForSimulation();
	Super::EndPlay(EndPlayReason);
}

void AOceanActor::BeginDestroy()
{
	WaitForSimulation();
	Super::BeginDestroy();
}

#if WITH_EDITOR
void AOceanActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	bNeedsInitialization = true;
	bMeshBuilt = false;
	BuildMesh();
}
#endif

void AOceanActor::SetSettings(const FOceanSettings& NewSettings)
{
	Settings = NewSettings;
	bNeedsInitialization = true;
}

void AOceanActor::WaitForSimulation()
{
	if (PendingTask.IsValid())
	{
		PendingTask.Wait();
		PendingTask = UE::Tasks::FTask();
	}
}

void AOceanActor::InitializeSimulation()
{
	WaitForSimulation();

	if (!Simulation)
	{
		Simulation = MakeUnique<FOceanSimulation>();
	}
	Simulation->Initialize(Settings);
	FrontFrame = MakeUnique<FOceanFrame>();
	BackFrame = MakeUnique<FOceanFrame>();

	const int32 Size = Simulation->GetResolution();
	const int32 NumMips = Simulation->GetNumMips();
	if (DisplacementTextures.Num() != FOceanSimulation::NumCascades || !DisplacementTextures[0] || DisplacementTextures[0]->GetSizeX() != Size)
	{
		DisplacementTextures.Reset();
		DerivativeTextures.Reset();
		for (int32 Index = 0; Index < FOceanSimulation::NumCascades; ++Index)
		{
			DisplacementTextures.Add(CreateOceanTexture(Size, NumMips));
			DerivativeTextures.Add(CreateOceanTexture(Size, NumMips));
		}
	}

	UpdateMaterialParameters();
	bNeedsInitialization = false;
}

void AOceanActor::UpdateMaterialParameters()
{
	UMaterialInterface* BaseMaterial = OceanMaterial.LoadSynchronous();
	if (!BaseMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("OceanActor: material %s not found. Run Scripts/create_ocean_material.py in the editor."),
			*OceanMaterial.ToString());
		return;
	}

	if (!MaterialInstance || MaterialInstance->Parent != BaseMaterial)
	{
		MaterialInstance = UMaterialInstanceDynamic::Create(BaseMaterial, this);
		OceanMesh->SetMaterial(0, MaterialInstance);
	}

	const float Sizes[] = { Settings.Cascade0Size, Settings.Cascade1Size, Settings.Cascade2Size };
	const bool bHasTextures = DisplacementTextures.Num() == FOceanSimulation::NumCascades;
	for (int32 Index = 0; Index < FOceanSimulation::NumCascades; ++Index)
	{
		if (bHasTextures)
		{
			MaterialInstance->SetTextureParameterValue(OceanParams::Displacement[Index], DisplacementTextures[Index]);
			MaterialInstance->SetTextureParameterValue(OceanParams::Derivatives[Index], DerivativeTextures[Index]);
		}
		MaterialInstance->SetScalarParameterValue(OceanParams::PatchSize[Index], Sizes[Index]);
	}
	MaterialInstance->SetScalarParameterValue(OceanParams::Resolution, float(Simulation ? Simulation->GetResolution() : Settings.Resolution));
	MaterialInstance->SetScalarParameterValue(OceanParams::GridSpacing, GridSpacing);
	MaterialInstance->SetScalarParameterValue(OceanParams::GridInnerHalfWidth, GridSpacing * GridInnerQuads);
	MaterialInstance->SetScalarParameterValue(OceanParams::GridGrowth, GridGrowth);
	MaterialInstance->SetScalarParameterValue(OceanParams::FadeStart, DisplacementFadeStart);
	MaterialInstance->SetScalarParameterValue(OceanParams::FadeEnd, DisplacementFadeEnd);
}

void AOceanActor::BuildMesh()
{
	// One continuous grid: uniform spacing around the centre, then spacing growing geometrically
	// to the horizon. No LOD seams, and detail where the camera is.
	const float S0 = FMath::Max(GridSpacing, 0.05f);
	const int32 Inner = FMath::Max(GridInnerQuads, 1);
	const int32 Outer = FMath::Max(GridOuterQuads, 1);
	const float InnerHalfWidth = S0 * Inner;
	const float OuterLength = FMath::Max(GridFarDistance - InnerHalfWidth, S0 * Outer);

	// Growth factor g such that S0 * (g^Outer - 1) / (g - 1) == OuterLength.
	auto OuterSpan = [&](double Growth) { return S0 * (FMath::Pow(Growth, double(Outer)) - 1.0) / (Growth - 1.0); };
	double Low = 1.0 + 1e-6;
	double High = 4.0;
	for (int32 Iteration = 0; Iteration < 100; ++Iteration)
	{
		const double Mid = 0.5 * (Low + High);
		(OuterSpan(Mid) < OuterLength ? Low : High) = Mid;
	}
	const double G = 0.5 * (Low + High);
	GridGrowth = float(G - 1.0);

	// Distance (m) from the edge of the inner area to the J-th outer vertex.
	auto OuterOffset = [&](int32 J) { return float(OuterSpan(G) * (FMath::Pow(G, double(J)) - 1.0) / (FMath::Pow(G, double(Outer)) - 1.0)); };

	TArray<float> Axis; // metres
	for (int32 J = Outer; J >= 1; --J)
	{
		Axis.Add(-(InnerHalfWidth + OuterOffset(J)));
	}
	for (int32 I = -Inner; I <= Inner; ++I)
	{
		Axis.Add(float(I) * S0);
	}
	for (int32 J = 1; J <= Outer; ++J)
	{
		Axis.Add(InnerHalfWidth + OuterOffset(J));
	}

	const int32 Count = Axis.Num();
	TArray<FVector> Vertices;
	TArray<FVector> Normals;
	TArray<FProcMeshTangent> Tangents;
	TArray<int32> Triangles;
	Vertices.Reserve(Count * Count + 2);
	Normals.Reserve(Count * Count + 2);
	Tangents.Reserve(Count * Count + 2);
	Triangles.Reserve((Count - 1) * (Count - 1) * 6);

	for (int32 Y = 0; Y < Count; ++Y)
	{
		for (int32 X = 0; X < Count; ++X)
		{
			Vertices.Add(FVector(double(Axis[X]), double(Axis[Y]), 0.0) * 100.0);
			Normals.Add(FVector::UpVector);
			Tangents.Add(FProcMeshTangent(1.f, 0.f, 0.f));
		}
	}
	for (int32 Y = 0; Y + 1 < Count; ++Y)
	{
		for (int32 X = 0; X + 1 < Count; ++X)
		{
			const int32 V00 = Y * Count + X;
			const int32 V10 = V00 + 1;
			const int32 V01 = V00 + Count;
			const int32 V11 = V01 + 1;
			Triangles.Append({ V00, V01, V10, V10, V01, V11 });
		}
	}

	// Unreferenced vertices that only widen the bounds, so displaced waves are never culled.
	Vertices.Add(FVector(0.0, 0.0, 5000.0));
	Vertices.Add(FVector(0.0, 0.0, -5000.0));
	Normals.Add(FVector::UpVector);
	Normals.Add(FVector::UpVector);
	Tangents.Add(FProcMeshTangent(1.f, 0.f, 0.f));
	Tangents.Add(FProcMeshTangent(1.f, 0.f, 0.f));

	OceanMesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, TArray<FVector2D>(),
		TArray<FLinearColor>(), Tangents, /*bCreateCollision=*/ false);
	if (MaterialInstance)
	{
		OceanMesh->SetMaterial(0, MaterialInstance);
	}
	bMeshBuilt = true;
	UpdateMaterialParameters();
}

void AOceanActor::FollowCamera()
{
	FVector Center = GetActorLocation();
	if (const UWorld* World = GetWorld(); World && World->IsGameWorld())
	{
		if (const APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(this, 0))
		{
			// Snap to the vertex spacing so the inner vertices never slide over the waves.
			const double Step = FMath::Max(GridSpacing, 0.05f) * 100.0;
			const FVector CameraLocation = CameraManager->GetCameraLocation();
			Center.X = FMath::GridSnap(CameraLocation.X, Step);
			Center.Y = FMath::GridSnap(CameraLocation.Y, Step);
		}
	}
	OceanMesh->SetWorldLocation(Center);
}

void AOceanActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bNeedsInitialization || !Simulation)
	{
		InitializeSimulation();
	}

	SimulationTime += DeltaSeconds;

	// Publish the frame computed since last tick.
	if (PendingTask.IsValid() && PendingTask.IsCompleted())
	{
		PendingTask = UE::Tasks::FTask();
		Swap(FrontFrame, BackFrame);
		UploadFrame(*FrontFrame);
	}

	// Start the next one.
	if (!PendingTask.IsValid())
	{
		const float Time = SimulationTime;
		const float Delta = Time - LastLaunchTime;
		LastLaunchTime = Time;
		FOceanSimulation* Sim = Simulation.Get();
		FOceanFrame* Target = BackFrame.Get();
		PendingTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Sim, Target, Time, Delta]()
		{
			Sim->Simulate(Time, Delta, *Target);
		});
	}

	FollowCamera();
}

void AOceanActor::UploadFrame(const FOceanFrame& Frame)
{
	if (!Frame.bValid)
	{
		return;
	}
	for (int32 Index = 0; Index < Frame.Cascades.Num() && Index < DisplacementTextures.Num(); ++Index)
	{
		UploadMips(DisplacementTextures[Index], Frame.Cascades[Index].DisplacementMips);
		UploadMips(DerivativeTextures[Index], Frame.Cascades[Index].DerivativeMips);
	}
}

FVector3f AOceanActor::SampleDisplacement(const FVector2f& PositionMeters) const
{
	FVector3f Result = FVector3f::ZeroVector;
	const int32 NumCascades = FMath::Min(NumGeometryCascades, FrontFrame->Cascades.Num());
	for (int32 Index = 0; Index < NumCascades; ++Index)
	{
		const FOceanCascadeFrame& Cascade = FrontFrame->Cascades[Index];
		const int32 N = Cascade.Resolution;

		// Same convention as GPU bilinear filtering: texel i is centred on uv = (i + 0.5) / N.
		const float U = PositionMeters.X / Cascade.PatchSize * N - 0.5f;
		const float V = PositionMeters.Y / Cascade.PatchSize * N - 0.5f;
		const float FloorU = FMath::FloorToFloat(U);
		const float FloorV = FMath::FloorToFloat(V);
		const float FracU = U - FloorU;
		const float FracV = V - FloorV;
		const int32 X0 = ((int32(FloorU) % N) + N) % N;
		const int32 Y0 = ((int32(FloorV) % N) + N) % N;
		const int32 X1 = (X0 + 1) % N;
		const int32 Y1 = (Y0 + 1) % N;

		const FVector3f& A = Cascade.Displacement[Y0 * N + X0];
		const FVector3f& B = Cascade.Displacement[Y0 * N + X1];
		const FVector3f& C = Cascade.Displacement[Y1 * N + X0];
		const FVector3f& D = Cascade.Displacement[Y1 * N + X1];
		Result += FMath::Lerp(FMath::Lerp(A, B, FracU), FMath::Lerp(C, D, FracU), FracV);
	}
	return Result;
}

float AOceanActor::GetWaterHeightAt(const FVector& WorldLocation) const
{
	const float SeaLevel = GetSeaLevel();
	if (!FrontFrame || !FrontFrame->bValid)
	{
		return SeaLevel;
	}

	// The surface point above P comes from the grid point X0 with X0 + D(X0) = P (horizontal displacement).
	const FVector2f Target(float(WorldLocation.X * 0.01), float(WorldLocation.Y * 0.01));
	FVector2f Source = Target;
	for (int32 Iteration = 0; Iteration < 4; ++Iteration)
	{
		const FVector3f D = SampleDisplacement(Source);
		Source = Target - FVector2f(D.X, D.Y);
	}
	return SeaLevel + SampleDisplacement(Source).Z * 100.f;
}
