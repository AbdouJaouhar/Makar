#pragma once

#include "CoreMinimal.h"

class FRHICommandListImmediate;
class FRHITexture;

/**
 * Steps of the shore water simulation on the GPU (Shaders/OceanShoreSim.usf): Jeschke & Wojtan 2023, "Generalizing
 * Shallow Water Simulations with Dispersive Surface Waves". A square of Resolution^2 cells of CellSize metres
 * around the camera, over the island's shores: a bulk flow by the shallow water equations (the swash running up and
 * draining off the beach, refraction, bores, eddies) and surface waves by Airy theory in Fourier space (their
 * dispersion exact), the swell entering at the edges with the phase of the analytic shore swell outside.
 * All textures RGBA32F, Resolution^2, UAV.
 */
struct FOceanShoreSimStep
{
	/** x = water depth h (m), y = qx, z = qy (m^2/s on the cells' +x / +y faces), w = spare. Persistent. */
	FRHITexture* State = nullptr;
	/** x = last step's surface wave height, y/z = bulk velocity on the faces, w = spare. Persistent. */
	FRHITexture* Aux = nullptr;
	/** x = bed height relative to sea level (m), y = swell travel time (s), z/w = swell direction. From the CPU. */
	FRHITexture* Terrain = nullptr;
	/** Output: x = water surface height (m, below the bed where dry), y/z = its slope, w = depth. */
	FRHITexture* Water = nullptr;

	int32 Resolution = 512;
	float CellSize = 0.5f;
	/** Fixed simulation step (s) and how many to run this frame. */
	float DeltaTime = 1.f / 60.f;
	int32 Substeps = 1;
	/** Simulation time at the start of this frame's steps (s): the forcing swell's phase. */
	float Time = 0.f;
	/** Explicit diffusion iterations of the decomposition (the paper uses 128). */
	int32 DiffusionIterations = 48;
	float SwellHeight = 1.f;
	float SwellPeriod = 7.f;
	float SwellReferenceDepth = 6.f;
	int32 EdgeCells = 24;
	/** Fill the state with water at rest (first step, or after the terrain changed). */
	bool bReset = false;
	/** The domain moved by this many whole cells since the last step: the state is shifted with it. */
	FIntPoint Shift = FIntPoint::ZeroValue;
};

/** Records and runs this frame's steps. Render thread. */
MAKARSHADERS_API void OceanShoreSimulate(FRHICommandListImmediate& RHICmdList, const FOceanShoreSimStep& Step);
