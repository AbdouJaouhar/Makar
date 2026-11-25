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
 * The ship's wood and iron as drawn, in cubes (ship space): what the canvas never passes through. Each cell: bit 1, a
 * drawn surface goes through it; bit 2, next to one (the solid grown by a cell: no gap between two cells touching only
 * at a corner, and a little air kept round everything).
 */
struct FShipVoxels
{
	FVector3f Origin = FVector3f::ZeroVector;
	float Cell = 0.07f;
	FIntVector Size = FIntVector::ZeroValue;
	TArray<uint8> Cells;

	bool IsValid() const { return Cells.Num() > 0; }
	uint8 Get(const FVector3f& P) const
	{
		const FVector3f Q = (P - Origin) / Cell;
		const int32 X = FMath::FloorToInt(Q.X), Y = FMath::FloorToInt(Q.Y), Z = FMath::FloorToInt(Q.Z);
		if (X < 0 || Y < 0 || Z < 0 || X >= Size.X || Y >= Size.Y || Z >= Size.Z)
		{
			return 0;
		}
		return Cells[(int64(Z) * Size.Y + Y) * Size.X + X];
	}
	/** Inside the grown solid: the canvas stops here. */
	bool Blocked(const FVector3f& P) const { return Get(P) != 0; }
	/** Inside the drawn wood itself: a true crossing. */
	bool InWood(const FVector3f& P) const { return (Get(P) & 1) != 0; }

	/** The grid over Bounds; then Fill each triangle, then Grow. */
	void Reset(const FBox3f& Bounds, float InCell);
	void Fill(const FVector3f& A, const FVector3f& B, const FVector3f& C);
	void Grow();
};

/**
 * A sail as the generator cuts it: a grid of (NU + 1) x (NV + 1) points in its cut shape (the canvas' built-in
 * camber: the rest lengths of the cloth), the points bent to the spars and stays (pinned), the clews held by sheets.
 */
struct FShipSail
{
	int32 NU = 0;
	int32 NV = 0;
	TArray<FVector3f> Rest;
	TArray<bool> Pinned;
	TArray<FVector2f> UV;
	TArray<FLinearColor> Color;

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

/**
 * The sails as cloth (position-based dynamics, small substeps): canvas that does not stretch but folds, weight,
 * aerodynamic pressure of the apparent wind on each facet (it fills when the wind is on its back, flogs when it lies
 * along it, is taken aback when on its face), held by its spars and sheets, resting against the ropes and spars
 * (capsules) and the other sails instead of passing through them. Ship space.
 */
class FSailSimulation
{
public:
	void Init(TArray<FShipSail> InSails, const TArray<FShipCapsule>& Colliders, const TArray<FShipBlock>& Solids, const FShipHullShape& InHull,
		TSharedPtr<const FShipVoxels> InVoxels);
	bool IsEmpty() const { return Sails.Num() == 0; }

	/** Advances by DeltaTime under Wind (m/s, ship space: where the air goes). Time: for the gusts. */
	void Step(float DeltaTime, const FVector3f& Wind, float Time);

	/** Every sail merged: the triangles, UVs and colours once; positions (cm) and normals each frame. */
	void GetTopology(TArray<int32>& OutTriangles, TArray<FVector2D>& OutUV, TArray<FColor>& OutColors) const;
	void GetSurface(TArray<FVector>& OutPositionsCm, TArray<FVector>& OutNormals) const;
	/** The sheets: clew, block. */
	void GetSheets(TArray<TPair<FVector3f, FVector3f>>& Out) const;
	/** The canvas sampled finely (4 x 4 per facet): how many samples lie inside the hull's side, out of how many. */
	int32 CountInHull(int32& OutSamples) const;
	/** The same against everything drawn in wood and iron (the voxels' drawn surfaces). */
	int32 CountInWood(int32& OutSamples) const;

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
		TArray<FShipCapsule> Near;
		/** The near ropes and spars sorted into cells: a cell lists every capsule within a cell of it. */
		FVector3f GridOrigin = FVector3f::ZeroVector;
		FIntVector GridSize = FIntVector::ZeroValue;
		TArray<int32> GridStart, GridItems;
		/** The hull's and the rig's solids this sail can reach, and where its points were when the frame began. */
		TArray<FShipBlock> NearSolids;
		TArray<FVector3f> FrameStart;
	};

	void Substep(FSailState& S, float H, const FVector3f& Wind, float Time) const;
	void UpdateNormals(FSailState& S) const;
	void CollideSails();
	void CollideSolids(FSailState& S) const;
	static void BuildGrid(FSailState& S);
	/** The capsules near P (indices into S.Near). */
	static TArrayView<const int32> CapsulesAt(const FSailState& S, const FVector3f& P);

	TArray<FSailState> Sails;
	FShipHullShape Hull;
	TSharedPtr<const FShipVoxels> Voxels;

public:
	/** Seconds spent: substeps, solids, sails apart, normals; substeps; frames (diagnostics). */
	double Profile[6] = {};
	FString TakeProfile()
	{
		const double F = FMath::Max(Profile[5], 1.0);
		int32 Particles = 0, Capsules = 0;
		for (const FSailState& S : Sails)
		{
			Particles += S.X.Num();
			Capsules += S.Near.Num();
		}
		const FString Out = FString::Printf(TEXT("substeps %.1f ms (%.1f a frame), solids %.1f ms, sails apart %.1f ms, normals %.1f ms; %d points, %d capsules near the sails"),
			Profile[0] * 1000.0 / F, Profile[4] / F, Profile[1] * 1000.0 / F, Profile[2] * 1000.0 / F, Profile[3] * 1000.0 / F, Particles, Capsules);
		FMemory::Memzero(Profile, sizeof(Profile));
		return Out;
	}
	float Accumulator = 0.f;
};
