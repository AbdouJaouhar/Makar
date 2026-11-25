#pragma once

#include "CoreMinimal.h"
#include "ShipDesign.generated.h"

/** The ship types of the Golden Age of Piracy in the Caribbean (c. 1650-1730). */
UENUM(BlueprintType)
enum class EShipType : uint8
{
	/** Single-masted, fore-and-aft rigged with a square topsail, a long bowsprit, fast: the typical pirate vessel. */
	Sloop,
	/** Two masts: the fore square-rigged, the main fore-and-aft with a square topsail. */
	Brigantine,
	/** Three square-rigged masts (lateen mizzen), quarterdeck and forecastle, a battery on the upper deck. */
	Frigate,
	/** Three or four masts (lateen mizzen and bonaventure), towering castles, strong tumblehome, two batteries. */
	Galleon,
};

/** A raised deck over part of the length: forecastle, quarterdeck, poop. */
USTRUCT(BlueprintType)
struct MAKAR_API FShipDeckLevel
{
	GENERATED_BODY()

	/** Its extent along the hull, as shares of the length from the stern (0) to the stem (1). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deck") float From = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deck") float To = 0.f;
	/** Height above the deck below it (m). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deck") float Height = 0.f;
};

/**
 * Every dimension of one ship (metres), drawn from its type and a seed by FShipDesign::Make. Axes: X forward (the bow
 * at +X), Y to starboard, Z up; Z = 0 at the waterline, X = 0 midships.
 */
USTRUCT(BlueprintType)
struct MAKAR_API FShipDesign
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") EShipType Type = EShipType::Sloop;

	// --- Lines ---
	/** Length on deck, stem to sternpost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float Length = 18.f;
	/** Greatest breadth (outside the frames). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float Beam = 6.f;
	/** Bottom of the hull (top of the keel) to the main deck at midships. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float Depth = 3.2f;
	/** Bottom of the hull to the waterline. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float Draft = 2.3f;
	/** Depth of the keel below the hull. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float KeelDepth = 0.3f;
	/** Height of the greatest breadth above the hull bottom, as a share of Depth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float MaxBeamHeight = 0.62f;
	/** Fullness of the midship section: 1 a V, 2 an ellipse, 3 a flat floor and hard bilges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float SectionFullness = 2.2f;
	/** Fullness of the waterlines towards the bow and the stern (1 straight wedge, 2 elliptic, 3 bluff). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float BowFullness = 2.4f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float SternFullness = 1.7f;
	/** Inward lean of the topsides from the greatest breadth to the deck, as a share of the half-breadth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float Tumblehome = 0.05f;
	/** Rise of the sheer above the midship deck height, at the bow and at the stern. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float SheerFore = 0.7f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float SheerAft = 0.5f;
	/** How far aft of the stem head the forefoot meets the keel (the stem's curve), and the sternpost's rake. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float StemRake = 1.6f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float SternRake = 0.4f;
	/** Flat transom: its half-width as a share of the half-beam, and its lower edge as a share of the hull's height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float TransomWidth = 0.6f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float TransomBottom = 0.55f;
	/** Bulwark: height of the rail above the deck. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") float Bulwark = 1.f;
	/** Raised decks: forecastle, quarterdeck, poop (a poop sits on the quarterdeck). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") TArray<FShipDeckLevel> RaisedDecks;

	// --- Planking ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planking") float PlankWidth = 0.24f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planking") float PlankThickness = 0.06f;
	/** Longest plank before a butt joint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planking") float PlankLength = 7.f;
	/** Wales: thick strakes along the side, at these heights above the waterline (m, at midships). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planking") TArray<float> Wales;

	// --- Armament ---
	/** Guns a side on the main (upper) deck, firing through ports in the bulwark. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Guns") int32 Guns = 4;
	/** Guns a side on a lower gun deck inside the hull (galleons), behind lidded ports. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Guns") int32 LowerGuns = 0;
	/** Barrel length (m): 4-pounders 1.8 m, 12-pounders 2.7 m. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Guns") float GunLength = 1.8f;

	// --- Rig ---
	/** Height of the mainmast (lower mast + topmasts) above the deck, as a share of Length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rig") float MastHeight = 1.f;
	/** Bowsprit outboard of the stem, as a share of Length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rig") float Bowsprit = 0.45f;
	/** Galleons: a fourth (bonaventure) mast. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rig") bool bBonaventure = false;
	/** Sails set and drawing (true) or furled on their yards. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rig") bool bSailsSet = true;

	// --- Colours (linear albedo) ---
	/** Topsides: oiled and tarred oak, or paint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Colours") FLinearColor TopsideColor = FLinearColor(0.11f, 0.075f, 0.045f);
	/** Wales and the rail: blackened with tar. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Colours") FLinearColor WaleColor = FLinearColor(0.025f, 0.022f, 0.02f);
	/** Below the waterline: the "white stuff" of the period (tallow, sulphur and rosin). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Colours") FLinearColor BottomColor = FLinearColor(0.42f, 0.39f, 0.32f);
	/** A painted band along the gun ports (frigates: ochre; galleons: red). Alpha 0: none. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Colours") FLinearColor BandColor = FLinearColor(0.f, 0.f, 0.f, 0.f);
	/** Deck planks: weathered pine or oak. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Colours") FLinearColor DeckColor = FLinearColor(0.32f, 0.26f, 0.19f);
	/** Sail canvas: unbleached flax or hemp, weathered. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Colours") FLinearColor SailColor = FLinearColor(0.56f, 0.5f, 0.39f);

	/** Variation of the plank colours and of the sails' (seed of the noise). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hull") int32 Seed = 0;

	/** A ship of this type, varied by Seed (proportions, sheer, castles, colours, armament). */
	static FShipDesign Make(EShipType Type, int32 Seed);

	/** The wind the rig is trimmed to (degrees from the bow, clockwise): the yards braced, the booms swung out, the
	 *  fore-and-aft sails sheeted to leeward. Set by the ship from its wind, not part of the design's look. */
	float WindFrom = 210.f;

	/** How the crew trims to a wind: the yards' angle from square (degrees, + : starboard arms forward), the side the
	 *  fore-and-aft sails go to (+1 starboard), the booms' swing out (degrees). */
	static void Trim(float WindFrom, float& OutBrace, float& OutLeeward, float& OutSwing);
};
