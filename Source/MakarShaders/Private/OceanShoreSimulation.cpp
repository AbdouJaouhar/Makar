#include "OceanShoreSimulation.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "PooledRenderTarget.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderCompilerCore.h"
#include "ShaderParameterStruct.h"

namespace
{
	bool ShouldCompileShoreShader(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
}

// Shared constants of every pass (each shader binds what it uses).
BEGIN_SHADER_PARAMETER_STRUCT(FShoreCommon, )
	SHADER_PARAMETER(uint32, Resolution)
	SHADER_PARAMETER(uint32, LogResolution)
	SHADER_PARAMETER(float, CellSize)
	SHADER_PARAMETER(float, DeltaTime)
	SHADER_PARAMETER(float, Time)
END_SHADER_PARAMETER_STRUCT()

#define SHORE_SHADER(Name, Entry, TypedLoads, ...) \
	class Name : public FGlobalShader \
	{ \
	public: \
		DECLARE_GLOBAL_SHADER(Name); \
		SHADER_USE_PARAMETER_STRUCT(Name, FGlobalShader); \
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, ) \
			SHADER_PARAMETER_STRUCT_INCLUDE(FShoreCommon, Common) \
			__VA_ARGS__ \
		END_SHADER_PARAMETER_STRUCT() \
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return ShouldCompileShoreShader(Parameters); } \
		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& Environment) \
		{ \
			FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment); \
			if (TypedLoads) { Environment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads); } \
		} \
	}; \
	IMPLEMENT_GLOBAL_SHADER(Name, "/Makar/OceanShoreSim.usf", Entry, SF_Compute);

SHORE_SHADER(FShoreRestartCS, "RestartCS", false,
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, OldState)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, OldAux)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Terrain)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutState)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutAux)
	SHADER_PARAMETER(FIntPoint, ShiftCells)
	SHADER_PARAMETER(uint32, bReset))

SHORE_SHADER(FShoreDiffuseInitCS, "DiffuseInitCS", false,
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, State)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Terrain)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DiffuseOut))

SHORE_SHADER(FShoreDiffuseCS, "DiffuseCS", false,
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DiffuseIn)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DiffuseOut))

SHORE_SHADER(FShoreDecomposeCS, "DecomposeCS", false,
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, State)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Terrain)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Diffused)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutBulk)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutSurface))

SHORE_SHADER(FShoreBulkCS, "BulkCS", false,
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Bulk)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Terrain)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutBulkFlow))

SHORE_SHADER(FShoreAiryPackCS, "AiryPackCS", false,
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Surface)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Aux)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutSpectrum))

SHORE_SHADER(FShoreFftCS, "FftCS", true,
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, Fields)
	SHADER_PARAMETER(uint32, Columns))

SHORE_SHADER(FShoreAirySpectrumCS, "AirySpectrumCS", false,
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Spectrum)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutDepths01)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutDepths23))

SHORE_SHADER(FShoreAiryGatherCS, "AiryGatherCS", false,
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Depths01)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Depths23)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Bulk)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutSurfaceFlow))

SHORE_SHADER(FShoreTransportCS, "TransportCS", false,
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Aux)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, BulkFlow)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Surface)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SurfaceFlow)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutTransported))

SHORE_SHADER(FShoreIntegrateCS, "IntegrateCS", false,
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, State)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Terrain)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, BulkFlow)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Transported)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Surface)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutState)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutAux))

SHORE_SHADER(FShoreForceCS, "ForceCS", true,
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Terrain)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, InOutState)
	SHADER_PARAMETER(uint32, EdgeCells)
	SHADER_PARAMETER(float, SwellHeight)
	SHADER_PARAMETER(float, SwellPeriod)
	SHADER_PARAMETER(float, SwellReferenceDepth))

SHORE_SHADER(FShoreSurfaceCS, "SurfaceCS", false,
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, State)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Terrain)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutWater))

namespace
{
	FShoreCommon MakeCommon(const FOceanShoreSimStep& Step, float Time)
	{
		FShoreCommon Common;
		Common.Resolution = uint32(Step.Resolution);
		Common.LogResolution = uint32(FMath::FloorLog2(uint32(Step.Resolution)));
		Common.CellSize = Step.CellSize;
		Common.DeltaTime = Step.DeltaTime;
		Common.Time = Time;
		return Common;
	}

	void AddShorePasses(FRDGBuilder& GraphBuilder, const FOceanShoreSimStep& Step)
	{
		const int32 N = Step.Resolution;
		RDG_EVENT_SCOPE(GraphBuilder, "OceanShoreSim %dx%d x%d", N, N, Step.Substeps);
		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		const FIntVector Groups = FComputeShaderUtils::GetGroupCount(FIntPoint(N, N), 8);

		FRDGTextureRef State = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Step.State, TEXT("OceanShore.State")));
		FRDGTextureRef Aux = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Step.Aux, TEXT("OceanShore.Aux")));
		FRDGTextureRef Terrain = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Step.Terrain, TEXT("OceanShore.Terrain")));
		FRDGTextureRef Water = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Step.Water, TEXT("OceanShore.Water")));

		const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(FIntPoint(N, N), PF_A32B32G32R32F, FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV);
		auto NewTexture = [&](const TCHAR* Name) { return GraphBuilder.CreateTexture(Desc, Name); };
		auto Copy = [&](FRDGTextureRef From, FRDGTextureRef To) { AddCopyTexturePass(GraphBuilder, From, To); };

		// Restart: water at rest, or the state shifted with the domain.
		if (Step.bReset || Step.Shift != FIntPoint::ZeroValue)
		{
			FRDGTextureRef OldState = NewTexture(TEXT("OceanShore.OldState"));
			FRDGTextureRef OldAux = NewTexture(TEXT("OceanShore.OldAux"));
			Copy(State, OldState);
			Copy(Aux, OldAux);
			FShoreRestartCS::FParameters* P = GraphBuilder.AllocParameters<FShoreRestartCS::FParameters>();
			P->Common = MakeCommon(Step, Step.Time);
			P->OldState = OldState;
			P->OldAux = OldAux;
			P->Terrain = Terrain;
			P->OutState = GraphBuilder.CreateUAV(State);
			P->OutAux = GraphBuilder.CreateUAV(Aux);
			P->ShiftCells = Step.Shift;
			P->bReset = Step.bReset ? 1u : 0u;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanShore.Restart"), ShaderMap->GetShader<FShoreRestartCS>(), P, Groups);
		}

		for (int32 Substep = 0; Substep < Step.Substeps; ++Substep)
		{
			const FShoreCommon Common = MakeCommon(Step, Step.Time + Substep * Step.DeltaTime);

			// 1. Decomposition by diffusion.
			FRDGTextureRef Diffuse[2] = { NewTexture(TEXT("OceanShore.Diffuse0")), NewTexture(TEXT("OceanShore.Diffuse1")) };
			{
				FShoreDiffuseInitCS::FParameters* P = GraphBuilder.AllocParameters<FShoreDiffuseInitCS::FParameters>();
				P->Common = Common;
				P->State = State;
				P->Terrain = Terrain;
				P->DiffuseOut = GraphBuilder.CreateUAV(Diffuse[0]);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanShore.DiffuseInit"), ShaderMap->GetShader<FShoreDiffuseInitCS>(), P, Groups);
			}
			int32 Current = 0;
			for (int32 Iteration = 0; Iteration < Step.DiffusionIterations; ++Iteration)
			{
				FShoreDiffuseCS::FParameters* P = GraphBuilder.AllocParameters<FShoreDiffuseCS::FParameters>();
				P->Common = Common;
				P->DiffuseIn = Diffuse[Current];
				P->DiffuseOut = GraphBuilder.CreateUAV(Diffuse[1 - Current]);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanShore.Diffuse"), ShaderMap->GetShader<FShoreDiffuseCS>(), P, Groups);
				Current = 1 - Current;
			}

			// 2. Bulk and surface parts.
			FRDGTextureRef Bulk = NewTexture(TEXT("OceanShore.Bulk"));
			FRDGTextureRef Surface = NewTexture(TEXT("OceanShore.Surface"));
			{
				FShoreDecomposeCS::FParameters* P = GraphBuilder.AllocParameters<FShoreDecomposeCS::FParameters>();
				P->Common = Common;
				P->State = State;
				P->Terrain = Terrain;
				P->Diffused = Diffuse[Current];
				P->OutBulk = GraphBuilder.CreateUAV(Bulk);
				P->OutSurface = GraphBuilder.CreateUAV(Surface);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanShore.Decompose"), ShaderMap->GetShader<FShoreDecomposeCS>(), P, Groups);
			}

			// 3. Bulk flow.
			FRDGTextureRef BulkFlow = NewTexture(TEXT("OceanShore.BulkFlow"));
			{
				FShoreBulkCS::FParameters* P = GraphBuilder.AllocParameters<FShoreBulkCS::FParameters>();
				P->Common = Common;
				P->Bulk = Bulk;
				P->Terrain = Terrain;
				P->OutBulkFlow = GraphBuilder.CreateUAV(BulkFlow);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanShore.Bulk"), ShaderMap->GetShader<FShoreBulkCS>(), P, Groups);
			}

			// 4. Airy surface waves: forward transform, exact update per depth, inverse transforms, interpolation.
			auto AddFft = [&](FRDGTextureRef Texture)
			{
				for (uint32 Columns = 0; Columns < 2; ++Columns)
				{
					FShoreFftCS::FParameters* P = GraphBuilder.AllocParameters<FShoreFftCS::FParameters>();
					P->Common = Common;
					P->Fields = GraphBuilder.CreateUAV(Texture);
					P->Columns = Columns;
					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanShore.Fft%s", Columns ? TEXT("Columns") : TEXT("Rows")),
						ShaderMap->GetShader<FShoreFftCS>(), P, FIntVector(N, 1, 1));
				}
			};
			FRDGTextureRef Spectrum = NewTexture(TEXT("OceanShore.Spectrum"));
			{
				FShoreAiryPackCS::FParameters* P = GraphBuilder.AllocParameters<FShoreAiryPackCS::FParameters>();
				P->Common = Common;
				P->Surface = Surface;
				P->Aux = Aux;
				P->OutSpectrum = GraphBuilder.CreateUAV(Spectrum);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanShore.AiryPack"), ShaderMap->GetShader<FShoreAiryPackCS>(), P, Groups);
			}
			AddFft(Spectrum);
			FRDGTextureRef Depths[2] = { NewTexture(TEXT("OceanShore.Depths01")), NewTexture(TEXT("OceanShore.Depths23")) };
			{
				FShoreAirySpectrumCS::FParameters* P = GraphBuilder.AllocParameters<FShoreAirySpectrumCS::FParameters>();
				P->Common = Common;
				P->Spectrum = Spectrum;
				P->OutDepths01 = GraphBuilder.CreateUAV(Depths[0]);
				P->OutDepths23 = GraphBuilder.CreateUAV(Depths[1]);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanShore.AirySpectrum"), ShaderMap->GetShader<FShoreAirySpectrumCS>(), P, Groups);
			}
			AddFft(Depths[0]);
			AddFft(Depths[1]);
			FRDGTextureRef SurfaceFlow = NewTexture(TEXT("OceanShore.SurfaceFlow"));
			{
				FShoreAiryGatherCS::FParameters* P = GraphBuilder.AllocParameters<FShoreAiryGatherCS::FParameters>();
				P->Common = Common;
				P->Depths01 = Depths[0];
				P->Depths23 = Depths[1];
				P->Bulk = Bulk;
				P->OutSurfaceFlow = GraphBuilder.CreateUAV(SurfaceFlow);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanShore.AiryGather"), ShaderMap->GetShader<FShoreAiryGatherCS>(), P, Groups);
			}

			// 5. Transport through the bulk flow.
			FRDGTextureRef Transported = NewTexture(TEXT("OceanShore.Transported"));
			{
				FShoreTransportCS::FParameters* P = GraphBuilder.AllocParameters<FShoreTransportCS::FParameters>();
				P->Common = Common;
				P->Aux = Aux;
				P->BulkFlow = BulkFlow;
				P->Surface = Surface;
				P->SurfaceFlow = SurfaceFlow;
				P->OutTransported = GraphBuilder.CreateUAV(Transported);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanShore.Transport"), ShaderMap->GetShader<FShoreTransportCS>(), P, Groups);
			}

			// 6. Integrate into a new state.
			FRDGTextureRef NewState = NewTexture(TEXT("OceanShore.NewState"));
			FRDGTextureRef NewAux = NewTexture(TEXT("OceanShore.NewAux"));
			{
				FShoreIntegrateCS::FParameters* P = GraphBuilder.AllocParameters<FShoreIntegrateCS::FParameters>();
				P->Common = Common;
				P->State = State;
				P->Terrain = Terrain;
				P->BulkFlow = BulkFlow;
				P->Transported = Transported;
				P->Surface = Surface;
				P->OutState = GraphBuilder.CreateUAV(NewState);
				P->OutAux = GraphBuilder.CreateUAV(NewAux);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanShore.Integrate"), ShaderMap->GetShader<FShoreIntegrateCS>(), P, Groups);
			}

			// 7. Swell at the edges.
			{
				FShoreForceCS::FParameters* P = GraphBuilder.AllocParameters<FShoreForceCS::FParameters>();
				P->Common = Common;
				P->Terrain = Terrain;
				P->InOutState = GraphBuilder.CreateUAV(NewState);
				P->EdgeCells = uint32(Step.EdgeCells);
				P->SwellHeight = Step.SwellHeight;
				P->SwellPeriod = Step.SwellPeriod;
				P->SwellReferenceDepth = Step.SwellReferenceDepth;
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanShore.Force"), ShaderMap->GetShader<FShoreForceCS>(), P, Groups);
			}
			Copy(NewState, State);
			Copy(NewAux, Aux);
		}

		// 8. The surface for rendering.
		{
			FShoreSurfaceCS::FParameters* P = GraphBuilder.AllocParameters<FShoreSurfaceCS::FParameters>();
			P->Common = MakeCommon(Step, Step.Time);
			P->State = State;
			P->Terrain = Terrain;
			P->OutWater = GraphBuilder.CreateUAV(Water);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanShore.Surface"), ShaderMap->GetShader<FShoreSurfaceCS>(), P, Groups);
		}
		GraphBuilder.SetTextureAccessFinal(Water, ERHIAccess::SRVMask);
	}
}

void OceanShoreSimulate(FRHICommandListImmediate& RHICmdList, const FOceanShoreSimStep& Step)
{
	const int32 N = Step.Resolution;
	if (!Step.State || !Step.Aux || !Step.Terrain || !Step.Water || N < 16 || N > 1024 || !FMath::IsPowerOfTwo(N) || Step.Substeps <= 0)
	{
		return;
	}
	// GPU time of the whole frame's steps, averaged and logged every few seconds (render thread only).
	static FRenderQueryPoolRHIRef QueryPool;
	static TArray<TPair<FRHIPooledRenderQuery, FRHIPooledRenderQuery>> PendingQueries;
	static double TotalMs = 0.0;
	static int32 Timed = 0;
	if (!QueryPool.IsValid())
	{
		QueryPool = RHICreateRenderQueryPool(RQT_AbsoluteTime);
	}
	FRHIPooledRenderQuery Begin = QueryPool->AllocateQuery();
	RHICmdList.EndRenderQuery(Begin.GetQuery());

	FRDGBuilder GraphBuilder(RHICmdList);
	AddShorePasses(GraphBuilder, Step);
	GraphBuilder.Execute();

	FRHIPooledRenderQuery End = QueryPool->AllocateQuery();
	RHICmdList.EndRenderQuery(End.GetQuery());
	PendingQueries.Emplace(MoveTemp(Begin), MoveTemp(End));
	for (int32 Index = 0; Index < PendingQueries.Num();)
	{
		uint64 BeginUs = 0, EndUs = 0;
		if (RHIGetRenderQueryResult(PendingQueries[Index].Key.GetQuery(), BeginUs, false)
			&& RHIGetRenderQueryResult(PendingQueries[Index].Value.GetQuery(), EndUs, false))
		{
			TotalMs += double(EndUs - BeginUs) / 1000.0;
			++Timed;
			PendingQueries.RemoveAt(Index);
		}
		else
		{
			++Index;
		}
	}
	if (PendingQueries.Num() > 16)
	{
		PendingQueries.RemoveAt(0);
	}
	if (Timed >= 120)
	{
		UE_LOG(LogTemp, Display, TEXT("OceanShoreSim: %dx%d, GPU %.3f ms per frame (%d frames)"), N, N, TotalMs / Timed, Timed);
		TotalMs = 0.0;
		Timed = 0;
	}
}
