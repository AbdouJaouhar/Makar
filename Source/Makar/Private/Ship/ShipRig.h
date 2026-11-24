#pragma once

#include "CoreMinimal.h"

class FHullLines;
namespace ShipMesh { class FBuilder; }

/** Where a mast stands, for the deck fittings around its foot (bitts, fife rails). */
struct FShipMastInfo
{
	FVector3f Foot = FVector3f::ZeroVector; // at the deck
	float Radius = 0.3f;
};

namespace ShipRig
{
	/**
	 * Masts (lower masts, tops, topmasts, topgallants, caps, crosstrees), bowsprit and jib-boom, yards, gaffs and booms;
	 * sails set and drawing (square sails bellied forward, fore-and-aft sails to leeward) or furled on their spars;
	 * standing rigging (shrouds with deadeyes and lanyards on chainwales, ratlines, stays, backstays, bobstay) and
	 * running rigging (lifts, braces, sheets, halyards, footropes), by type.
	 */
	void Build(ShipMesh::FBuilder& B, const FHullLines& Lines, TArray<FShipMastInfo>& OutMasts);
}
