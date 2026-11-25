#pragma once

#include "CoreMinimal.h"

class FHullLines;
struct FShipSail;
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

	/** Where the masts stand: X (m) and the lower mast's radius at the deck (holes in the decks, hatches clear of them). */
	TArray<FVector2f> MastStations(const FHullLines& Lines);

	/** The same, the sails set given as cloth (OutSails) rather than drawn in the mesh in their cut shape. */
	void Build(ShipMesh::FBuilder& B, const FHullLines& Lines, TArray<FShipMastInfo>& OutMasts, TArray<FShipSail>* OutSails);
}
