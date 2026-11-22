#pragma once

#include "CoreMinimal.h"

class FRHICommandListImmediate;
class FRHITexture;

/**
 * One time step of a Tessendorf FFT ocean cascade on the GPU (compute shaders in Shaders/OceanFFT.usf): the same
 * maths as FOceanSimulation::SimulateCascade, at resolutions the CPU cannot afford every frame. Used for the finest
 * cascade, which only shades the surface (normals, glints, foam, caustics): geometry and water queries stay on the
 * CPU cascades, so rendering and physics remain consistent.
 */
struct FOceanGpuCascadeStep
{
	/** RGBA32F, Resolution^2: h0(k).re, h0(k).im, conj(h0(-k)).re, conj(h0(-k)).im (FOceanSimulation). */
	FRHITexture* Spectrum = nullptr;

	/** RGBA16F, Resolution^2 with a full mip chain, UAV: displacement X, Y, height (m), foam (persistent). */
	FRHITexture* Displacement = nullptr;

	/** Same format: slope X, slope Y, dDispX/dX, dDispY/dY. */
	FRHITexture* Derivatives = nullptr;

	/** Same format: slope second moments X^2, Y^2, X*Y (LEAN mapping). */
	FRHITexture* Moments = nullptr;

	int32 Resolution = 0; // power of two, up to 1024
	float Time = 0.f;
	float PatchSize = 0.f;
	float Depth = 0.f;
	float Choppiness = 0.f;
	float FoamThreshold = 0.f;
	float FoamIntensity = 0.f;
	/** Fraction of last step's foam that survives this step. */
	float FoamFade = 1.f;
};

/** Records and runs the passes of one step. Render thread. */
MAKARSHADERS_API void OceanGpuSimulateCascade(FRHICommandListImmediate& RHICmdList, const FOceanGpuCascadeStep& Step);
