#pragma once

#include "CoreMinimal.h"
#include "SailCloth.h"

namespace ShipMesh { class FBuilder; }

/** A rope as rigged: its two ends made fast, its sag at the middle (0: set up taut), its look. */
struct FShipRope
{
	FVector3f A = FVector3f::ZeroVector;
	FVector3f B = FVector3f::ZeroVector;
	float Radius = 0.02f;
	float Sag = 0.f;
	FLinearColor Color = FLinearColor::Black;
};

/** A solid block of the rig (top, cap, crosstrees...): an oriented box. */
struct FShipBlock
{
	FVector3f O = FVector3f::ZeroVector;
	FVector3f X = FVector3f::ForwardVector;
	FVector3f Y = FVector3f::RightVector;
	FVector3f Z = FVector3f::UpVector;
	FVector3f Half = FVector3f::ZeroVector;
};

namespace ShipRopes
{
	/**
	 * Lays every rope as a physical line: a chain that does not stretch (a taut one, set up to the straight distance
	 * between its ends; a slack one, as long as its sag requires), made fast at both ends, hanging under its weight,
	 * and wrapped round the spars and blocks in its way instead of passing through them (position-based relaxation, in
	 * parallel). Draws them into B (Rope section); returns how many points of rope are still inside a solid.
	 */
	int32 Lay(ShipMesh::FBuilder& B, const TArray<FShipRope>& Ropes, const TArray<FShipCapsule>& Spars, const TArray<FShipBlock>& Blocks);
}
