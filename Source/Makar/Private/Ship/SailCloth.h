#pragma once

#include "CoreMinimal.h"

/** A rope or a spar the sails rest against (ship space, m). */
struct FShipCapsule
{
	FVector3f A = FVector3f::ZeroVector;
	FVector3f B = FVector3f::ZeroVector;
	float Radius = 0.f;
	/** A spar (mast, yard, boom...), not a rope. */
	bool bSpar = false;
};

/** A solid of the ship (deck, side, bulwark, rail, top, cap...): an oriented box. */
struct FShipBlock
{
	FVector3f O = FVector3f::ZeroVector;
	FVector3f X = FVector3f::ForwardVector;
	FVector3f Y = FVector3f::RightVector;
	FVector3f Z = FVector3f::UpVector;
	FVector3f Half = FVector3f::ZeroVector;
};

/**
 * The hull's outer shape for the sails: its half breadth outside the planking, tabulated by length X and height H above
 * the keel (ship space z = H + ZOffset), the height of its rail along it, and the thickness of its side.
 */
struct FShipHullShape
{
	float X0 = 0.f, DX = 0.25f;
	float H0 = 0.f, DH = 0.1f;
	int32 NX = 0, NH = 0;
	float ZOffset = 0.f;
	float Wall = 0.3f;
	TArray<float> Half;
	TArray<float> Rail;

	bool IsValid() const { return NX > 1 && NH > 1; }
	/** Half breadth at X, H (0 off the hull). */
	float HalfAt(float X, float H) const
	{
		const float FX = FMath::Clamp((X - X0) / DX, 0.f, NX - 1.001f), FH = FMath::Clamp((H - H0) / DH, 0.f, NH - 1.001f);
		const int32 IX = FMath::FloorToInt(FX), IH = FMath::FloorToInt(FH);
		const float AX = FX - IX, AH = FH - IH;
		auto V = [&](int32 I, int32 J) { return Half[J * NX + I]; };
		return FMath::Lerp(FMath::Lerp(V(IX, IH), V(IX + 1, IH), AX), FMath::Lerp(V(IX, IH + 1), V(IX + 1, IH + 1), AX), AH);
	}
	float RailAt(float X) const
	{
		const float FX = FMath::Clamp((X - X0) / DX, 0.f, NX - 1.001f);
		const int32 IX = FMath::FloorToInt(FX);
		return FMath::Lerp(Rail[IX], Rail[IX + 1], FX - IX);
	}
};

/**
 * Everything of the ship that is drawn (wood, iron, ropes, glass, furled canvas), in 7 cm cubes over the whole ship from
 * a metre under the waterline to the trucks: what the canvas never passes through. Sparse: bricks of 8 x 8 x 8 cells
 * where something is. Each cell: bit 1, a drawn surface goes through it; bit 2, next to one (the solid grown by a
 * cell: no gap between two cells touching only at a corner, and a little air kept round everything).
 */
struct FShipVoxels
{
	FVector3f Origin = FVector3f::ZeroVector;
	float Cell = 0.07f;
	FIntVector Bricks = FIntVector::ZeroValue;
	TArray<int32> Top;          // brick index, or -1 where nothing is
	TArray<FIntVector> Where;   // each brick's place in Top
	TArray<uint8> Cells;        // 512 a brick
	TArray<uint8> Touched;      // while building

	bool IsValid() const { return Cells.Num() > 0; }
	uint8 GetCell(int32 X, int32 Y, int32 Z) const
	{
		if (X < 0 || Y < 0 || Z < 0)
		{
			return 0;
		}
		const int32 BX = X >> 3, BY = Y >> 3, BZ = Z >> 3;
		if (BX >= Bricks.X || BY >= Bricks.Y || BZ >= Bricks.Z)
		{
			return 0;
		}
		const int32 Brick = Top[(BZ * Bricks.Y + BY) * Bricks.X + BX];
		return Brick < 0 ? 0 : Cells[Brick * 512 + ((Z & 7) * 8 + (Y & 7)) * 8 + (X & 7)];
	}
	uint8 Get(const FVector3f& P) const
	{
		const FVector3f Q = (P - Origin) / Cell;
		return GetCell(FMath::FloorToInt(Q.X), FMath::FloorToInt(Q.Y), FMath::FloorToInt(Q.Z));
	}
	/** Inside the grown solid: the canvas stops here. */
	bool Blocked(const FVector3f& P) const { return Get(P) != 0; }
	/** Inside something drawn itself: a true crossing. */
	bool InWood(const FVector3f& P) const { return (Get(P) & 1) != 0; }
	/** The free cell nearest P within Radius cells (its centre); false if none. */
	bool NearestFree(const FVector3f& P, int32 Radius, FVector3f& Out) const;

	/** Building: Reset over Bounds; Touch every triangle (in parallel); Allocate; Fill every triangle; Grow. */
	void Reset(const FBox3f& Bounds, float InCell);
	void Touch(const FVector3f& A, const FVector3f& B, const FVector3f& C);
	void Allocate();
	/** Kind: a bit telling what is drawn there (4 << mesh section), for the diagnostics. */
	void Fill(const FVector3f& A, const FVector3f& B, const FVector3f& C, uint8 Kind = 0);
	void Grow();
	int32 NumBricks() const { return Where.Num(); }
};

/**
 * A sail as the generator cuts it: a grid of (NU + 1) x (NV + 1) points in its cut shape, filled by the wind it is
 * trimmed to (the canvas' built-in camber: the rest lengths of the cloth, and the shape it keeps to), the points bent
 * to the spars and stays (pinned), the clews held by sheets. Fitted into the ship (ShipSails::Fit): nothing passes
 * through its shape, and each point has the room it may move in (Margin) without meeting anything.
 */
struct FShipSail
{
	int32 NU = 0;
	int32 NV = 0;
	TArray<FVector3f> Rest;
	TArray<bool> Pinned;
	TArray<FVector2f> UV;
	TArray<FLinearColor> Color;
	/** Metres each point may stray from Rest without touching a rope, spar, the hull or another sail. */
	TArray<float> Margin;

	/** A sheet: a rope from a clew to its block, which pulls but cannot push. */
	struct FSheet
	{
		int32 Particle = 0;
		FVector3f Anchor = FVector3f::ZeroVector;
		float Length = 0.f;
	};
	TArray<FSheet> Sheets;

	int32 Index(int32 I, int32 J) const { return J * (NU + 1) + I; }
};

namespace ShipSails
{
	/**
	 * Done once when the ship is generated, as an artist would for a hand-made ship: every sail's shape kept clear of
	 * every rope, spar, top, the hull and the other sails (a point too near is moved off; a facet a rope goes through is
	 * moved to the side the sail bellies to), then each point's room measured (Margin). Returns a report for the log.
	 */
	FString Fit(TArray<FShipSail>& Sails, const TArray<FShipCapsule>& Colliders, const TArray<FShipBlock>& Blocks, const FShipHullShape& Hull);
}

/**
 * The sails as cloth (position-based dynamics, small substeps): canvas that does not stretch but folds, weight,
 * aerodynamic pressure of the apparent wind on each facet, held by its spars and sheets, and each point kept within its
 * margin of the fitted shape: it ripples, fills and flogs, and never reaches anything (no collision to search for while
 * playing). Ship space.
 */
class FSailSimulation
{
public:
	/** Verify (optional): everything drawn, in cubes, to measure that nothing is crossed (diagnostics only). */
	void Init(TArray<FShipSail> InSails, TSharedPtr<const FShipVoxels> Verify);
	bool IsEmpty() const { return Sails.Num() == 0; }

	/** Advances by DeltaTime under Wind (m/s, ship space: where the air goes). Time: for the gusts. */
	void Step(float DeltaTime, const FVector3f& Wind, float Time);

	/** Every sail merged: the triangles, UVs and colours once; positions (cm) and normals each frame. */
	void GetTopology(TArray<int32>& OutTriangles, TArray<FVector2D>& OutUV, TArray<FColor>& OutColors) const;
	void GetSurface(TArray<FVector>& OutPositionsCm, TArray<FVector>& OutNormals) const;
	/** The sheets: clew, block. */
	void GetSheets(TArray<TPair<FVector3f, FVector3f>>& Out) const;

	/** Diagnostics. The canvas sampled 4 x 4 a facet: samples inside anything drawn (needs Verify), out of how many. */
	bool CanVerify() const { return Verify.IsValid() && Verify->IsValid(); }
	int32 CountInDrawn(int32& OutSamples, FString* OutDetail = nullptr) const;
	/** Edges of a sail's canvas through another sail's facets. */
	int32 CountSailCrossings() const;
	/** Milliseconds a frame spent stepping, substeps a frame, since the last call. */
	FString TakeProfile();

private:
	struct FConstraint
	{
		int32 A, B;
		float Rest, Stretch, Compress;
	};

	struct FSailState
	{
		FShipSail Spec;
		TArray<FVector3f> X, Prev, Normal, Force;
		TArray<float> InvMass;
		TArray<FConstraint> Constraints;
	};

	void Substep(FSailState& S, float H, const FVector3f& Wind, float Time) const;
	void UpdateNormals(FSailState& S) const;

	TArray<FSailState> Sails;
	TSharedPtr<const FShipVoxels> Verify;
	float Accumulator = 0.f;
	double ProfileSeconds = 0.0, ProfileSubsteps = 0.0, ProfileFrames = 0.0;
};
