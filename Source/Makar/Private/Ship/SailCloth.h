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
	void Init(TArray<FShipSail> InSails, const TArray<FShipCapsule>& Colliders, const TArray<FShipBlock>& Solids);
	bool IsEmpty() const { return Sails.Num() == 0; }

	/** Advances by DeltaTime under Wind (m/s, ship space: where the air goes). Time: for the gusts. */
	void Step(float DeltaTime, const FVector3f& Wind, float Time);

	/** Every sail merged: the triangles, UVs and colours once; positions (cm) and normals each frame. */
	void GetTopology(TArray<int32>& OutTriangles, TArray<FVector2D>& OutUV, TArray<FColor>& OutColors) const;
	void GetSurface(TArray<FVector>& OutPositionsCm, TArray<FVector>& OutNormals) const;
	/** The sheets: clew, block. */
	void GetSheets(TArray<TPair<FVector3f, FVector3f>>& Out) const;

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
		/** The hull's and the rig's solids this sail can reach, and where its points were when the frame began. */
		TArray<FShipBlock> NearSolids;
		TArray<FVector3f> FrameStart;
	};

	void Substep(FSailState& S, float H, const FVector3f& Wind, float Time) const;
	void UpdateNormals(FSailState& S) const;
	void CollideSails();
	void CollideSolids(FSailState& S) const;

	TArray<FSailState> Sails;
	float Accumulator = 0.f;
};
