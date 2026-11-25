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
 * A sail as the generator cuts it: a grid of (NU + 1) x (NV + 1) points in its shape, trimmed to the wind (the belly
 * its cut and the wind give it), the points bent to its spars and stays held. Fitted into the ship (ShipSails::Fit):
 * its shape clear of everything, and each point's movement (Amplitude) no more than the room it has.
 */
struct FShipSail
{
	int32 NU = 0;
	int32 NV = 0;
	TArray<FVector3f> Rest;
	TArray<bool> Pinned;
	TArray<FVector2f> UV;
	TArray<FLinearColor> Color;
	/** The mast it belongs to (headsails: the foremast). */
	int32 Mast = 0;
	/** The most this kind of sail moves, away from its held edges (m). */
	float MaxSwing = 0.3f;
	/** Each point's movement as designed (MaxSwing, fading to nothing at the held edges), and as fitted. */
	TArray<float> Swing;
	TArray<float> Amplitude;
	/** The same sail with its belly gone (the wind out of it), and the share of the way to it each point may go. */
	TArray<FVector3f> Flat;
	TArray<float> Slack;
	/** The way the sail bellies (its canvas' mean normal, towards the belly). */
	FVector3f BellyDir = FVector3f::ForwardVector;

	int32 Index(int32 I, int32 J) const { return J * (NU + 1) + I; }
};

namespace ShipSails
{
	/** Air kept between a sail at the far end of its movement and anything else (m). */
	constexpr float Keep = 0.12f;
	/** The band along a sail's held edges (yard, stay, mast, gaff, boom) where it is bent to them and may touch its
	 *  own fittings there (lacing, hoops, jaws, robands) (m). */
	constexpr float Bent = 0.3f;

	/** Distance from P to the sail's held edges (segments between neighbouring held points). */
	float ToHeld(const struct FShipSail& Sail, const FVector3f& P);

	/** How the fitting went: what the success criteria measure. */
	struct FFitReport
	{
		/** Points or facet samples of a shape nearer than Keep to something (must be 0). */
		int32 ShapeFails = 0;
		/** The masts whose sails fail (bit per mast). */
		uint32 FailMasts = 0;
		/** Share of the canvas (that is meant to move) whose movement had to be cut by more than half. */
		float Reduced = 0.f;
		FString Text;
	};

	/**
	 * Done once when the ship is generated, as a rigger and an artist would for a hand-made ship. Each sail's shape is
	 * moved clear of every rope, spar, block, fitting, deck, the hull and the other sails (by smooth bulges, square to
	 * the canvas); then each point's room is measured, exactly for the ropes and spars, and its movement bounded by it:
	 * at the far end of its movement, no sail comes nearer than Keep to anything, nor to another sail at the far end of
	 * its own (each takes half the gap).
	 */
	FFitReport Fit(TArray<FShipSail>& Sails, const TArray<FShipCapsule>& Colliders, const TArray<FShipBlock>& Blocks, const FShipHullShape& Hull);
}

/**
 * The sails moving: each point off its fitted shape, square to the canvas, by its amplitude times a bounded sum of
 * waves (the flutter running through the canvas, a slower breathing of the whole sail), scaled by the wind's strength.
 * A pure function of time: no state, nothing to collide, and never further from the shape than its amplitude. Ship space.
 */
class FSailAnimation
{
public:
	/** Verify (optional): everything drawn, in cubes, to measure that nothing is crossed (diagnostics only). */
	void Init(TArray<FShipSail> InSails, TSharedPtr<const FShipVoxels> InVerify);
	bool IsEmpty() const { return Sails.Num() == 0; }

	/**
	 * The sails at Time, under a wind of Strength (0 to 1: the share of their movement) going towards WindDir (ship
	 * space): each fills when the wind is on its back, empties and flogs as the wind comes along it or onto its face.
	 * ForcedFill (0 to 1) sets every sail's filling instead (diagnostics).
	 */
	void Evaluate(float Time, float Strength, const FVector3f& WindDir = FVector3f::ForwardVector, float ForcedFill = -1.f);

	/** Every sail merged: the triangles, UVs and colours once; positions (cm) and normals each frame. */
	void GetTopology(TArray<int32>& OutTriangles, TArray<FVector2D>& OutUV, TArray<FColor>& OutColors) const;
	void GetSurface(TArray<FVector>& OutPositionsCm, TArray<FVector>& OutNormals) const;

	/**
	 * Diagnostics: the whole movement played at full strength, Times instants over Seconds; at each, the canvas sampled
	 * 4 x 4 a facet against everything drawn (Verify), and every canvas edge against the other sails' facets.
	 */
	bool CanVerify() const { return Verify.IsValid() && Verify->IsValid(); }
	FString VerifyCycle(int32 Times, float Seconds, int32& OutInside, int32& OutCrossings, FVector3f* OutFirst = nullptr);

private:
	struct FSailState
	{
		FShipSail Spec;
		TArray<FVector3f> X, Normal, RestNormal;
		/** Metres along the canvas from its luff, and down from its head: where each point is in the waves. */
		TArray<float> Along, Down;
		float Phase = 0.f;
		/** How full it is (1: filled; 0: the wind out of it), eased towards what the wind gives it. */
		float Fill = 1.f;
		float LastTime = -1.f;
		/** Across the canvas, from the edge at Along 0 to the other (ship space), and the canvas' width. */
		FVector3f AcrossDir = FVector3f::YAxisVector;
		float Width = 1.f;
		/** Eased each frame: the share of the waves running from the edge at Along 0 (1) or from the other (0), the
		 * leading edge's luffing (0 to 1), and how deep the belly is drawn (1: running free; less on a wind). */
		float FromNear = 1.f, Luff = 0.f, Depth = 1.f;
	};

	void UpdateNormals(const FShipSail& Spec, const TArray<FVector3f>& X, TArray<FVector3f>& OutNormal) const;
	int32 CountInDrawn(int32& OutSamples, FString* OutDetail, FVector3f* OutFirst = nullptr) const;
	int32 CountSailCrossings() const;

	TArray<FSailState> Sails;
	TSharedPtr<const FShipVoxels> Verify;
};
