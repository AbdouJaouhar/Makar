#pragma once

#include "CoreMinimal.h"

class FHullLines;
struct FShipGunPort;
struct FShipMastInfo;
namespace ShipMesh { class FBuilder; }

namespace ShipFittings
{
	/**
	 * Guns on truck carriages behind the ports (iron; bronze on galleons) and swivel guns on the rail of sloops; hatches
	 * with gratings, capstan or windlass, wheel or tiller, binnacle, bitts and fife rails with belaying pins, pin rails
	 * at the shrouds, a bell, stern lanterns, catheads with their anchors, hawse holes, a ship's boat on skids.
	 */
	void Build(ShipMesh::FBuilder& B, const FHullLines& Lines, const TArray<FShipGunPort>& Ports, const TArray<FShipMastInfo>& Masts);

	/**
	 * The ship on a building berth: keel blocks under the keel and bilge shores both sides, standing on the ground
	 * BlockHeight below the keel. Returns the ground's Z (ship space, m).
	 */
	float Cradle(ShipMesh::FBuilder& B, const FHullLines& Lines, float BlockHeight);
}
