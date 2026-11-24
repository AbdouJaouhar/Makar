#pragma once

#include "CoreMinimal.h"
#include "Ship/ShipDesign.h"

namespace ShipMesh { class FBuilder; }

/**
 * The lines of a hull: its outside surface (outside the frames, under the planking) as a function of a station S
 * (-1 at the sternpost and transom, 0 midships, 1 at the stem) and of the girth T (0 at the keel, 1 at the top of the
 * rail), for the starboard side (Y > 0; the port side is its mirror). Heights H are above the hull bottom (the top of
 * the keel); ship space has Z = H - Draft.
 *
 * Sections: a superellipse from the keel to the greatest breadth (SectionFullness: V, round or flat-floored), then the
 * topsides leaning in (tumblehome) up to the rail. Waterlines: a superellipse towards the stem (BowFullness), and aft a
 * run that narrows to the sternpost low down and ends at a flat transom higher up. The stem is a quarter ellipse from
 * the forefoot to the stem head, the sternpost a straight rake.
 */
class FHullLines
{
public:
	explicit FHullLines(const FShipDesign& InDesign);

	const FShipDesign& D;
	float HalfLength;
	float HalfBeam;
	/** Height of the greatest breadth. */
	float Hmb;
	/** Thickness of the frames: from the outside of the hull to the inside of the bulwarks. */
	float FrameThickness;

	/** Main deck height at X (sheer included), the top deck at X (raised decks included), the rail. */
	float MainDeck(float X) const;
	float RaisedAt(float X) const;
	float DeckAt(float X) const { return MainDeck(X) + RaisedAt(X); }
	float RailAt(float X) const;

	/** The stem and the sternpost: X at height H. */
	float XBow(float H) const;
	float XStern(float H) const;

	/** Half-breadth outside the frames at (X, H); 0 outside the hull. */
	float HalfBreadth(float X, float H) const;
	/** Station of X at height H (beyond +-1: outside the hull). */
	float StationOf(float X, float H) const;

	/** A point of the starboard surface (ship space, m) at station S, girth T; and its outward normal. */
	FVector3f Surface(float S, float T) const;
	FVector3f Normal(float S, float T) const;
	/** Height H at (S, T), and the girth T at height H on station S. */
	float HeightAt(float S, float T) const;
	float GirthAt(float S, float H) const;
	/** Total girth (m) of station S, keel to rail. */
	float Girth(float S) const;

	/** Ship space point of the starboard surface at station S and height H. */
	FVector3f PointSH(float S, float H) const;
	/** The outside of the hull at (X, H) on the side Side (+1 starboard, -1 port), and its outward normal. */
	FVector3f SidePoint(float X, float H, float Side, FVector3f* OutNormal = nullptr) const;

	float Z(float H) const { return H - D.Draft; }

private:
	/** The surface by station and section parameter P: 0-1 up the bilge (angle), 1-2 up the topsides (height). */
	FVector3f PointSP(float S, float P) const;
	float HeightSP(float S, float P) const;
	float Longitudinal(float S, float H) const;
	float Shape(float H) const;
	float TopAt(float S) const;

	static constexpr int32 Stations = 800;
	static constexpr int32 GirthSteps = 128;
	/** Section parameter P at uniform girth steps, per station; and each station's girth. */
	TArray<float> PTable;
	TArray<float> GirthTable;
	float NormalSign = 1.f;
};

/** A gun port, for the guns behind it (ShipFittings). */
struct FShipGunPort
{
	float X = 0.f;
	/** Sill and lintel heights (H). */
	float H0 = 0.f;
	float H1 = 0.f;
	float HalfWidth = 0.3f;
	/** The deck the gun stands on (H). */
	float DeckH = 0.f;
	/** On the lower gun deck: a lidded port in the hull side. */
	bool bLower = false;
};

namespace ShipHull
{
	/** Keel, stem, sternpost, rudder, planked hull with wales and paint, transom, bulwarks, rails, decks, gun ports. */
	void Build(ShipMesh::FBuilder& B, const FHullLines& Lines, TArray<FShipGunPort>& OutPorts);
}
