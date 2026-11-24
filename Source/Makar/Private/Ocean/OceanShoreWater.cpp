#include "Ocean/OceanShoreWater.h"

#include "Async/ParallelFor.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Ocean/OceanBathymetry.h"
#include "Ocean/OceanShoreData.h"
#include "OceanShoreSimulation.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "TextureResource.h"

namespace
{
	const FName ShoreWaterParam(TEXT("ShoreWater"));
	const FName ShoreWaterOriginParam(TEXT("ShoreWaterOrigin"));
	const FName ShoreWaterSizeParam(TEXT("ShoreWaterSize"));

	constexpr float StepSeconds = 1.f / 60.f;
	constexpr int32 MaxSubsteps = 4;
	// The square moves in jumps of this many cells (its terrain is rebuilt each time).
	constexpr int32 RecentreCells = 32;

	// Debugging switches: the decomposition's iterations (0 = pure shallow water, -1 = the component's), the swell's
	// height at the edges (-1 = the ocean's), and a reset of the water.
	TAutoConsoleVariable<int32> CVarShoreIterations(TEXT("r.Makar.ShoreWater.Iterations"), -1, TEXT("Diffusion iterations override (-1: default)."));
	TAutoConsoleVariable<float> CVarShoreSwell(TEXT("r.Makar.ShoreWater.Swell"), -1.f, TEXT("Swell height override at the edges (m, -1: default)."));
	TAutoConsoleVariable<int32> CVarShoreReset(TEXT("r.Makar.ShoreWater.Reset"), 0, TEXT("Set to 1 to reset the water to rest (clears itself)."));
}

UOceanShoreWaterComponent::UOceanShoreWaterComponent()
{
	PrimaryComponentTick.bCanEverTick = false; // stepped by AOceanActor, after the waves
}

void UOceanShoreWaterComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	FlushRenderingCommands(); // the readback and the targets are used by queued render commands
	Heights.Reset();
	Super::EndPlay(Reason);
}

void UOceanShoreWaterComponent::CreateTargets()
{
	auto Make = [this](const TCHAR* Name)
	{
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(this, Name, RF_Transient);
		Target->bCanCreateUAV = true;
		Target->bAutoGenerateMips = false;
		Target->AddressX = TA_Clamp;
		Target->AddressY = TA_Clamp;
		Target->Filter = TF_Bilinear;
		Target->ClearColor = FLinearColor::Transparent;
		Target->InitCustomFormat(Resolution, Resolution, PF_A32B32G32R32F, true);
		Target->UpdateResourceImmediate(true);
		return Target;
	};
	StateTarget = Make(TEXT("ShoreWaterState"));
	AuxTarget = Make(TEXT("ShoreWaterAux"));
	WaterTarget = Make(TEXT("ShoreWater"));
	Heights = MakeShared<FHeights, ESPMode::ThreadSafe>();
}

void UOceanShoreWaterComponent::BuildTerrain(const FOceanShoreData* Shore, const AOceanBathymetry* Bathymetry, float SeaLevelCm)
{
	const int32 N = Resolution;
	TArray<FLinearColor> Texels;
	Texels.SetNumUninitialized(N * N);
	const FVector2f Origin = FVector2f(OriginCell) * CellSizeM;
	ParallelFor(N, [&](int32 Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			const FVector2f PositionM = Origin + FVector2f(X + 0.5f, Y + 0.5f) * CellSizeM;
			// The bed: the island's own terrain where there is one (the same ground the beach is drawn with), else the
			// shore map's depth.
			float Bed = Shore ? -Shore->DepthAt(PositionM) : -FOceanShoreData::DeepWaterM;
			if (Bathymetry)
			{
				Bed = (Bathymetry->GetGroundHeightAt(FVector(PositionM.X * 100.0, PositionM.Y * 100.0, 0.0)) - SeaLevelCm) * 0.01f;
			}
			// The analytic shore swell's phase (travel time to the shore) and its direction (towards the shore).
			FVector2f Direction(0.f, 0.f);
			float Travel = 0.f;
			if (Shore)
			{
				const FVector4f Here = Shore->Sample(PositionM);
				Travel = Here.Z;
				const float Step = Shore->TexelSizeM();
				const FVector2f Gradient(Shore->Sample(PositionM + FVector2f(Step, 0.f)).Z - Here.Z, Shore->Sample(PositionM + FVector2f(0.f, Step)).Z - Here.Z);
				if (Gradient.SizeSquared() > 1e-10f)
				{
					Direction = -Gradient.GetSafeNormal();
				}
			}
			Texels[Y * N + X] = FLinearColor(FMath::Max(Bed, -200.f), Travel, Direction.X, Direction.Y);
		}
	});

	if (!TerrainTexture || TerrainTexture->GetSizeX() != N)
	{
		TerrainTexture = UTexture2D::CreateTransient(N, N, PF_A32B32G32R32F, TEXT("ShoreWaterTerrain"));
		TerrainTexture->SRGB = false;
		TerrainTexture->Filter = TF_Nearest;
		TerrainTexture->CompressionSettings = TC_HDR;
		TerrainTexture->NeverStream = true;
	}
	FByteBulkData& Mip = TerrainTexture->GetPlatformData()->Mips[0].BulkData;
	FMemory::Memcpy(Mip.Lock(LOCK_READ_WRITE), Texels.GetData(), int64(Texels.Num()) * sizeof(FLinearColor));
	Mip.Unlock();
	TerrainTexture->UpdateResource();
}

void UOceanShoreWaterComponent::Step(float SimulationTime, const FVector2f& CenterM, const FOceanShoreData* Shore,
	const AOceanBathymetry* Bathymetry, float SeaLevelCm, const FOceanShoreSwell& Swell, UMaterialInstanceDynamic* Material)
{
	if (!bEnabled || !Shore || Swell.Height <= 0.f || Swell.Period <= 0.f || !FMath::IsPowerOfTwo(Resolution))
	{
		if (Material)
		{
			Material->SetScalarParameterValue(ShoreWaterSizeParam, 0.f);
		}
		return;
	}
	// The square follows the camera in jumps, the state shifted with it.
	const FIntPoint Wanted(
		(FMath::FloorToInt(CenterM.X / CellSizeM) - Resolution / 2) / RecentreCells * RecentreCells,
		(FMath::FloorToInt(CenterM.Y / CellSizeM) - Resolution / 2) / RecentreCells * RecentreCells);
	if (!bInitialized)
	{
		CreateTargets();
		OriginCell = Wanted;
		BuildTerrain(Shore, Bathymetry, SeaLevelCm);
		bInitialized = true;
		bNeedsReset = true;
	}
	else if (Wanted != OriginCell)
	{
		PendingShift += Wanted - OriginCell;
		OriginCell = Wanted;
		BuildTerrain(Shore, Bathymetry, SeaLevelCm);
	}

	// Fixed steps of 1/60 s on the ocean's clock.
	const float Delta = LastTime >= 0.f ? FMath::Clamp(SimulationTime - LastTime, 0.f, 0.1f) : 0.f;
	LastTime = SimulationTime;
	Accumulator += Delta;
	const int32 Substeps = FMath::Min(FMath::FloorToInt(Accumulator / StepSeconds), MaxSubsteps);
	Accumulator = FMath::Max(Accumulator - Substeps * StepSeconds, 0.f);
	if (Accumulator > StepSeconds)
	{
		Accumulator = 0.f; // fell behind: drop the backlog rather than spiral
	}

	FOceanShoreSimStep Sim;
	Sim.Resolution = Resolution;
	Sim.CellSize = CellSizeM;
	Sim.DeltaTime = StepSeconds;
	Sim.Substeps = Substeps;
	Sim.Time = SimulationTime - FMath::Max(Substeps - 1, 0) * StepSeconds;
	Sim.DiffusionIterations = CVarShoreIterations.GetValueOnGameThread() >= 0 ? CVarShoreIterations.GetValueOnGameThread() : DiffusionIterations;
	Sim.SwellHeight = CVarShoreSwell.GetValueOnGameThread() >= 0.f ? CVarShoreSwell.GetValueOnGameThread() : Swell.Height;
	if (CVarShoreReset.GetValueOnGameThread() != 0)
	{
		CVarShoreReset->Set(0);
		bNeedsReset = true;
	}
	Sim.SwellPeriod = Swell.Period;
	Sim.SwellReferenceDepth = 0.5f * Swell.MaxDepth;
	Sim.bReset = bNeedsReset;
	Sim.Shift = PendingShift;
	bNeedsReset = false;
	PendingShift = FIntPoint::ZeroValue;

	FTextureRenderTargetResource* State = StateTarget->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* Aux = AuxTarget->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* Water = WaterTarget->GameThread_GetRenderTargetResource();
	FTextureResource* Terrain = TerrainTexture ? TerrainTexture->GetResource() : nullptr;
	TSharedPtr<FHeights, ESPMode::ThreadSafe> HeightsRef = Heights;
	const FIntPoint Origin = OriginCell;
	const int32 N = Resolution;
	ENQUEUE_RENDER_COMMAND(OceanShoreWater)([Sim, State, Aux, Water, Terrain, HeightsRef, Origin, N](FRHICommandListImmediate& RHICmdList) mutable
	{
		if (!State || !Aux || !Water || !Terrain || !Terrain->GetTexture2DRHI())
		{
			return;
		}
		Sim.State = State->GetRenderTargetTexture();
		Sim.Aux = Aux->GetRenderTargetTexture();
		Sim.Water = Water->GetRenderTargetTexture();
		Sim.Terrain = Terrain->GetTexture2DRHI();
		if (Sim.Substeps > 0 || Sim.bReset || Sim.Shift != FIntPoint::ZeroValue)
		{
			Sim.Substeps = FMath::Max(Sim.Substeps, 1);
			OceanShoreSimulate(RHICmdList, Sim);
		}
		// The CPU copy for the water queries: collect the last readback once the GPU is done with it, start another.
		if (!HeightsRef.IsValid())
		{
			return;
		}
		FHeights& H = *HeightsRef;
		if (!H.Readback)
		{
			H.Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("ShoreWaterReadback"));
		}
		if (H.bPending && H.Readback->IsReady())
		{
			int32 RowPitch = 0;
			if (const FLinearColor* Data = static_cast<const FLinearColor*>(H.Readback->Lock(RowPitch)))
			{
				FScopeLock ScopeLock(&H.Lock);
				H.Data.SetNumUninitialized(N * N);
				for (int32 Y = 0; Y < N; ++Y)
				{
					for (int32 X = 0; X < N; ++X)
					{
						H.Data[Y * N + X] = Data[Y * RowPitch + X].R;
					}
				}
				H.Origin = H.PendingOrigin;
				// Diagnostics every ~5 s: the water surface's range over the wet cells, and how much is wet.
				static int32 Counter = 0;
				if (++Counter % 60 == 0)
				{
					float MinEta = 1e9f, MaxEta = -1e9f;
					int32 Wet = 0;
					for (int32 Y = 0; Y < N; ++Y)
					{
						for (int32 X = 0; X < N; ++X)
						{
							const FLinearColor& T = Data[Y * RowPitch + X];
							if (T.A > 0.01f)
							{
								++Wet;
								MinEta = FMath::Min(MinEta, T.R);
								MaxEta = FMath::Max(MaxEta, T.R);
							}
						}
					}
					UE_LOG(LogTemp, Display, TEXT("OceanShoreWater: surface %.2f .. %.2f m over %.0f%% wet cells"), MinEta, MaxEta, 100.f * Wet / float(N * N));
				}
			}
			H.Readback->Unlock();
			H.bPending = false;
		}
		if (!H.bPending)
		{
			H.Readback->EnqueueCopy(RHICmdList, Sim.Water);
			H.PendingOrigin = Origin;
			H.bPending = true;
		}
	});

	if (Material)
	{
		const FVector2f OriginM = GetOriginM();
		Material->SetTextureParameterValue(ShoreWaterParam, WaterTarget);
		Material->SetVectorParameterValue(ShoreWaterOriginParam, FLinearColor(OriginM.X, OriginM.Y, 0.f, 0.f));
		Material->SetScalarParameterValue(ShoreWaterSizeParam, GetSizeM());
	}
}

bool UOceanShoreWaterComponent::SampleHeight(const FVector2f& PositionM, float& OutHeightM) const
{
	if (!Heights.IsValid())
	{
		return false;
	}
	const int32 N = Resolution;
	FScopeLock ScopeLock(&Heights->Lock);
	const TArray<float>& Data = Heights->Data;
	if (Data.Num() != N * N)
	{
		return false;
	}
	// Bilinear, cell centres at (i + 0.5) * size, like the GPU.
	const FVector2f Local = PositionM / CellSizeM - FVector2f(Heights->Origin) - FVector2f(0.5f, 0.5f);
	const int32 X0 = FMath::FloorToInt(Local.X);
	const int32 Y0 = FMath::FloorToInt(Local.Y);
	// The outer band is the forcing zone: outside it the analytic shore wave answers.
	const int32 Margin = 24;
	if (X0 < Margin || Y0 < Margin || X0 >= N - 1 - Margin || Y0 >= N - 1 - Margin)
	{
		return false;
	}
	const float FX = Local.X - X0;
	const float FY = Local.Y - Y0;
	const float A = Data[Y0 * N + X0];
	const float B = Data[Y0 * N + X0 + 1];
	const float C = Data[(Y0 + 1) * N + X0];
	const float D = Data[(Y0 + 1) * N + X0 + 1];
	OutHeightM = FMath::Lerp(FMath::Lerp(A, B, FX), FMath::Lerp(C, D, FX), FY);
	return true;
}
