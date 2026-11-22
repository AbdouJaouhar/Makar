#pragma once

#include "CoreMinimal.h"

/**
 * A regular grid of heights (m) over the island, sampled at its nodes: node (X, Y) sits at OriginM + (X, Y) * CellM.
 * Holds the eroded ground (OceanErosion) and the water that ran over it.
 */
struct MAKAR_API FOceanHeightfield
{
	FVector2f OriginM = FVector2f::ZeroVector;
	float CellM = 1.f;
	int32 Width = 0;
	int32 Height = 0;
	TArray<float> Heights;
	/** Runoff that went through each node during the hydraulic erosion, normalised: ~1 in the gullies, 0 on ridges. */
	TArray<float> Flow;

	bool IsValid() const { return Width > 1 && Height > 1 && Heights.Num() == Width * Height; }

	/** True if PositionM is inside the grid (with one cell of margin for bilinear sampling). */
	bool Contains(const FVector2f& PositionM) const;

	/** Bilinear height at PositionM (clamped to the grid). */
	float Sample(const FVector2f& PositionM) const { return SampleArray(Heights, PositionM); }

	/** Bilinear runoff at PositionM, 0 if there is none. */
	float SampleFlow(const FVector2f& PositionM) const { return Flow.Num() == Heights.Num() ? SampleArray(Flow, PositionM) : 0.f; }

private:
	float SampleArray(const TArray<float>& Values, const FVector2f& PositionM) const;
};

/**
 * Erosion of a heightfield, so that the land has the shapes of water and gravity instead of the smooth lumps of noise:
 * gullies and valleys that join downhill, ridges and spurs between them, fans of sediment at their mouths, screes at
 * the foot of steep slopes.
 *  - Hydraulic: droplets of rain run downhill (particle method of H. T. Beyer, "Implementation of a method for
 *    hydraulic erosion", 2015). A droplet's capacity to carry sediment grows with its speed, its water and the slope;
 *    under capacity it erodes the ground around it (a small brush), over capacity or uphill it deposits. Droplets that
 *    reach the sea take their sediment with them.
 *  - Thermal: wherever the slope to a neighbour exceeds the material's angle of repose (Talus, as a tangent), part of
 *    the difference slides down, until the slope rests at that angle (sand ~33 degrees, soil ~37, limestone cliffs
 *    stay steep).
 */
namespace OceanErosion
{
	/**
	 * Fluvial incision at the scale of the drainage basins (stream power law, E = K A^m S, solved implicitly along the
	 * drainage network: J. Braun and S. Willett, "A very efficient O(n), implicit and parallel method to solve the
	 * stream power equation governing fluvial incision and landscape evolution", Geomorphology 2013). Each iteration
	 * fills the depressions (priority flood with an epsilon gradient, R. Barnes et al. 2014) so that every cell drains
	 * to the sea, routes each cell to its steepest neighbour, accumulates the drained area A, and lowers each cell
	 * towards its receiver in proportion to K A^m: valleys that branch upstream and deepen towards the coast, ridges
	 * between them. Fills Field.Flow with the drainage area (log, normalised).
	 */
	MAKAR_API void StreamPower(FOceanHeightfield& Field, const TArray<float>& Erodibility, int32 Iterations, float K, float M = 0.5f,
		float SeaLevel = 0.f);

	/**
	 * Relief grown from uplift against fluvial incision (G. Cordonnier et al., "Large Scale Terrain Generation from
	 * Tectonic Uplift and Fluvial Erosion", Eurographics 2016): each iteration raises the land by Uplift (per node, m
	 * per iteration; 0 = base level, which stays at height 0), then the stream power law cuts it down along the drainage
	 * network (K and M as in StreamPower, per iteration), then soil creep rounds it (Diffusion = D dt / dx2, < 0.25).
	 * Towards the steady state the drainage network organises itself: branching valleys whose slope decreases
	 * downstream, sharp ridges between them. Fills Field.Flow.
	 */
	MAKAR_API void LandscapeEvolution(FOceanHeightfield& Field, const TArray<float>& Uplift, int32 Iterations, float K, float M = 0.5f,
		float Diffusion = 0.05f);

	struct FHydraulicSettings
	{
		int32 Droplets = 400000;
		int32 Lifetime = 64;          // steps of one cell
		float Inertia = 0.05f;        // 0 = follows the slope exactly, 1 = keeps its direction
		float CapacityFactor = 0.05f; // sediment (m) carried per unit of drop x speed x water
		float MinCapacity = 0.0005f;
		float ErodeSpeed = 0.3f;
		float DepositSpeed = 0.3f;
		float Evaporation = 0.015f;
		float Gravity = 4.f;
		int32 Radius = 2;             // erosion brush, cells
		float SeaLevel = 0.f;         // droplets stop (and lose their sediment) below it
		float RainAbove = 0.3f;       // droplets only start on ground above this height
	};

	/** Erodibility: per node, 0 = hard (bare rock), 1 = loose soil. Fills Field.Flow. Deterministic for a seed. */
	MAKAR_API void Hydraulic(FOceanHeightfield& Field, const FHydraulicSettings& Settings, const TArray<float>& Erodibility, int32 Seed);

	/** Talus: per node, the tangent of the angle of repose. Rate: share of the excess moved per iteration. */
	MAKAR_API void Thermal(FOceanHeightfield& Field, const TArray<float>& Talus, int32 Iterations, float Rate = 0.5f);
}
