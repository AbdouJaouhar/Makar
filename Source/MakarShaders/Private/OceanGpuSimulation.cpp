#include "OceanGpuSimulation.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GenerateMips.h"
#include "GlobalShader.h"
#include "PooledRenderTarget.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"
#include "ShaderCompilerCore.h"
#include "ShaderParameterStruct.h"

namespace
{
	bool ShouldCompileOceanShader(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	/** The FFT works in place and the foam persists in the displacement texture: both read back their UAV
	 *  (RGBA32F / RGBA16F typed loads, supported by D3D12 and all current desktop GPUs). */
	void AllowTypedUavLoads(FShaderCompilerEnvironment& Environment)
	{
		Environment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}
}

class FOceanSpectrumCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FOceanSpectrumCS);
	SHADER_USE_PARAMETER_STRUCT(FOceanSpectrumCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Spectrum)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutFields0)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutFields1)
		SHADER_PARAMETER(uint32, Resolution)
		SHADER_PARAMETER(float, Time)
		SHADER_PARAMETER(float, PatchSize)
		SHADER_PARAMETER(float, Depth)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return ShouldCompileOceanShader(Parameters); }
};
IMPLEMENT_GLOBAL_SHADER(FOceanSpectrumCS, "/Makar/OceanFFT.usf", "SpectrumCS", SF_Compute);

class FOceanFftCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FOceanFftCS);
	SHADER_USE_PARAMETER_STRUCT(FOceanFftCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, Fields)
		SHADER_PARAMETER(uint32, Resolution)
		SHADER_PARAMETER(uint32, LogResolution)
		SHADER_PARAMETER(uint32, Columns)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return ShouldCompileOceanShader(Parameters); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& Environment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
		AllowTypedUavLoads(Environment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FOceanFftCS, "/Makar/OceanFFT.usf", "FftCS", SF_Compute);

class FOceanAssembleCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FOceanAssembleCS);
	SHADER_USE_PARAMETER_STRUCT(FOceanAssembleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Fields0)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Fields1)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutDisplacement)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutDerivatives)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutMoments)
		SHADER_PARAMETER(uint32, Resolution)
		SHADER_PARAMETER(float, Choppiness)
		SHADER_PARAMETER(float, FoamThreshold)
		SHADER_PARAMETER(float, FoamIntensity)
		SHADER_PARAMETER(float, FoamFade)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return ShouldCompileOceanShader(Parameters); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& Environment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
		AllowTypedUavLoads(Environment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FOceanAssembleCS, "/Makar/OceanFFT.usf", "AssembleCS", SF_Compute);

namespace
{
	void AddOceanCascadePasses(FRDGBuilder& GraphBuilder, const FOceanGpuCascadeStep& Step);
}

void OceanGpuSimulateCascade(FRHICommandListImmediate& RHICmdList, const FOceanGpuCascadeStep& Step)
{
	const int32 N = Step.Resolution;
	if (!Step.Spectrum || !Step.Displacement || !Step.Derivatives || !Step.Moments || N < 16 || N > 1024 || !FMath::IsPowerOfTwo(N))
	{
		return;
	}

	// GPU time of the whole step, from two timestamps, averaged and logged every few seconds (render thread only).
	static FRenderQueryPoolRHIRef QueryPool;
	static TArray<TPair<FRHIPooledRenderQuery, FRHIPooledRenderQuery>> PendingQueries;
	static double TotalMs = 0.0;
	static int32 TimedSteps = 0;
	if (!QueryPool.IsValid())
	{
		QueryPool = RHICreateRenderQueryPool(RQT_AbsoluteTime);
	}
	FRHIPooledRenderQuery Begin = QueryPool->AllocateQuery();
	RHICmdList.EndRenderQuery(Begin.GetQuery());

	FRDGBuilder GraphBuilder(RHICmdList);
	AddOceanCascadePasses(GraphBuilder, Step);
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
			++TimedSteps;
			PendingQueries.RemoveAt(Index);
		}
		else
		{
			++Index;
		}
	}
	if (PendingQueries.Num() > 16)
	{
		PendingQueries.RemoveAt(0); // never resolved: drop rather than grow
	}
	if (TimedSteps >= 120)
	{
		UE_LOG(LogTemp, Display, TEXT("OceanGpuCascade: %dx%d, GPU %.3f ms per step (%d steps)"), N, N, TotalMs / TimedSteps, TimedSteps);
		TotalMs = 0.0;
		TimedSteps = 0;
	}
}

namespace
{
void AddOceanCascadePasses(FRDGBuilder& GraphBuilder, const FOceanGpuCascadeStep& Step)
{
	// Named in ProfileGPU. The scope must close before GraphBuilder.Execute(), hence this separate function.
	const int32 N = Step.Resolution;
	RDG_EVENT_SCOPE(GraphBuilder, "OceanGpuCascade %dx%d", N, N);
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	FRDGTextureRef Spectrum = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Step.Spectrum, TEXT("OceanGpu.Spectrum")));
	FRDGTextureRef Displacement = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Step.Displacement, TEXT("OceanGpu.Displacement")));
	FRDGTextureRef Derivatives = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Step.Derivatives, TEXT("OceanGpu.Derivatives")));
	FRDGTextureRef Moments = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Step.Moments, TEXT("OceanGpu.Moments")));

	const FRDGTextureDesc FieldDesc = FRDGTextureDesc::Create2D(FIntPoint(N, N), PF_A32B32G32R32F, FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef Fields[2] = {
		GraphBuilder.CreateTexture(FieldDesc, TEXT("OceanGpu.Fields0")),
		GraphBuilder.CreateTexture(FieldDesc, TEXT("OceanGpu.Fields1")) };

	// 1. Spectrum at time t.
	{
		FOceanSpectrumCS::FParameters* Parameters = GraphBuilder.AllocParameters<FOceanSpectrumCS::FParameters>();
		Parameters->Spectrum = Spectrum;
		Parameters->OutFields0 = GraphBuilder.CreateUAV(Fields[0]);
		Parameters->OutFields1 = GraphBuilder.CreateUAV(Fields[1]);
		Parameters->Resolution = uint32(N);
		Parameters->Time = Step.Time;
		Parameters->PatchSize = Step.PatchSize;
		Parameters->Depth = Step.Depth;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanGpu.Spectrum"), ShaderMap->GetShader<FOceanSpectrumCS>(),
			Parameters, FComputeShaderUtils::GetGroupCount(FIntPoint(N, N), 8));
	}

	// 2. Inverse FFT: rows, then columns, of both field textures.
	const uint32 LogN = uint32(FMath::FloorLog2(uint32(N)));
	for (FRDGTextureRef FieldTexture : Fields)
	{
		for (uint32 Columns = 0; Columns < 2; ++Columns)
		{
			FOceanFftCS::FParameters* Parameters = GraphBuilder.AllocParameters<FOceanFftCS::FParameters>();
			Parameters->Fields = GraphBuilder.CreateUAV(FieldTexture);
			Parameters->Resolution = uint32(N);
			Parameters->LogResolution = LogN;
			Parameters->Columns = Columns;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanGpu.Fft%s", Columns ? TEXT("Columns") : TEXT("Rows")),
				ShaderMap->GetShader<FOceanFftCS>(), Parameters, FIntVector(N, 1, 1));
		}
	}

	// 3. Output textures (mip 0), then their mips.
	{
		FOceanAssembleCS::FParameters* Parameters = GraphBuilder.AllocParameters<FOceanAssembleCS::FParameters>();
		Parameters->Fields0 = Fields[0];
		Parameters->Fields1 = Fields[1];
		Parameters->OutDisplacement = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Displacement, 0));
		Parameters->OutDerivatives = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Derivatives, 0));
		Parameters->OutMoments = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Moments, 0));
		Parameters->Resolution = uint32(N);
		Parameters->Choppiness = Step.Choppiness;
		Parameters->FoamThreshold = Step.FoamThreshold;
		Parameters->FoamIntensity = Step.FoamIntensity;
		Parameters->FoamFade = Step.FoamFade;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanGpu.Assemble"), ShaderMap->GetShader<FOceanAssembleCS>(),
			Parameters, FComputeShaderUtils::GetGroupCount(FIntPoint(N, N), 8));
	}
	for (FRDGTextureRef Output : { Displacement, Derivatives, Moments })
	{
		if (Output->Desc.NumMips > 1)
		{
			FGenerateMips::ExecuteCompute(GraphBuilder, GMaxRHIFeatureLevel, Output,
				TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI());
		}
		GraphBuilder.SetTextureAccessFinal(Output, ERHIAccess::SRVMask);
	}
}
}
