#include "Ship/ShipDesign.h"

#include "Math/RandomStream.h"

namespace
{
	FShipDeckLevel Deck(float From, float To, float Height)
	{
		FShipDeckLevel Level;
		Level.From = From;
		Level.To = To;
		Level.Height = Height;
		return Level;
	}

	FLinearColor Vary(FRandomStream& Random, const FLinearColor& Color, float Amount)
	{
		const float Light = 1.f + Random.FRandRange(-Amount, Amount);
		return FLinearColor(Color.R * Light * (1.f + Random.FRandRange(-0.05f, 0.05f)), Color.G * Light,
			Color.B * Light * (1.f + Random.FRandRange(-0.05f, 0.05f)), Color.A);
	}
}

void FShipDesign::Trim(float WindFrom, float& OutBrace, float& OutLeeward, float& OutSwing, bool& bOutAback)
{
	// The wind's angle off the bow, 0 (dead ahead) to 180 (dead astern), and the side it comes over.
	const float Off = FMath::Abs(FMath::UnwindDegrees(WindFrom));
	const float Sine = FMath::Sin(FMath::DegreesToRadians(WindFrom));
	OutLeeward = Sine > 0.05f ? -1.f : 1.f;
	// Square yards: square before the wind, braced round as it comes forward, up to the shrouds (about 35 degrees),
	// the windward arms aft. Booms: right out running, close in on the wind.
	OutBrace = OutLeeward * FMath::Clamp((180.f - Off) * 0.5f, 0.f, 35.f);
	OutSwing = FMath::Clamp(Off * 0.45f - 5.f, 8.f, 70.f);
	// Square sails can be braced up to about 55 degrees off the wind: closer, it is on their faces.
	bOutAback = Off < 55.f;
}

FShipDesign FShipDesign::Make(EShipType Type, int32 Seed)
{
	FRandomStream Random(Seed * 7919 + int32(Type) * 104729 + 17);
	auto Range = [&Random](float Min, float Max) { return Random.FRandRange(Min, Max); };
	FShipDesign D;
	D.Type = Type;
	D.Seed = Seed;
	switch (Type)
	{
	case EShipType::Sloop:
		// Jamaica / Bermuda sloops: long, low, sharp-floored (strong deadrise), raked stem, a tall single mast and a very
		// long bowsprit; a few small guns on an open deck, sometimes a slightly raised quarterdeck.
		D.Length = Range(17.f, 23.f);
		D.Beam = D.Length * Range(0.29f, 0.33f);
		D.Depth = D.Length * Range(0.15f, 0.17f);
		D.Draft = D.Depth * Range(0.72f, 0.8f);
		D.KeelDepth = 0.25f;
		D.MaxBeamHeight = Range(0.6f, 0.7f);
		D.SectionFullness = Range(1.45f, 1.75f);
		D.BowFullness = Range(1.8f, 2.1f);
		D.SternFullness = Range(1.4f, 1.6f);
		D.Tumblehome = 0.03f;
		D.SheerFore = Range(0.5f, 0.75f);
		D.SheerAft = Range(0.3f, 0.45f);
		D.StemRake = D.Depth * Range(0.7f, 0.9f);
		D.SternRake = D.Depth * Range(0.12f, 0.2f);
		D.TransomWidth = Range(0.5f, 0.6f);
		D.TransomBottom = Range(0.58f, 0.66f);
		D.Bulwark = Range(0.75f, 0.95f);
		if (Random.FRand() < 0.5f)
		{
			D.RaisedDecks.Add(Deck(0.f, Range(0.18f, 0.24f), 0.45f));
		}
		D.PlankWidth = 0.22f;
		D.PlankThickness = 0.05f;
		D.Wales = { D.Depth - D.Draft - 0.25f };
		D.Guns = Random.RandRange(3, 5);
		D.GunLength = 1.7f;
		D.MastHeight = Range(1.05f, 1.2f);
		D.Bowsprit = Range(0.5f, 0.62f);
		D.TopsideColor = Random.FRand() < 0.5f ? FLinearColor(0.1f, 0.07f, 0.045f) : FLinearColor(0.16f, 0.11f, 0.06f);
		D.BandColor = Random.FRand() < 0.4f ? FLinearColor(0.32f, 0.2f, 0.05f, 1.f) : FLinearColor(0.f, 0.f, 0.f, 0.f);
		break;

	case EShipType::Brigantine:
		D.Length = Range(24.f, 29.f);
		D.Beam = D.Length * Range(0.27f, 0.3f);
		D.Depth = D.Length * Range(0.14f, 0.155f);
		D.Draft = D.Depth * Range(0.74f, 0.8f);
		D.KeelDepth = 0.3f;
		D.MaxBeamHeight = Range(0.62f, 0.7f);
		D.SectionFullness = Range(1.8f, 2.2f);
		D.BowFullness = Range(2.1f, 2.5f);
		D.SternFullness = Range(1.5f, 1.8f);
		D.Tumblehome = 0.05f;
		D.SheerFore = Range(0.55f, 0.8f);
		D.SheerAft = Range(0.35f, 0.5f);
		D.StemRake = D.Depth * Range(0.6f, 0.75f);
		D.SternRake = D.Depth * Range(0.1f, 0.16f);
		D.TransomWidth = Range(0.55f, 0.65f);
		D.TransomBottom = Range(0.56f, 0.62f);
		D.Bulwark = Range(0.9f, 1.05f);
		D.RaisedDecks.Add(Deck(0.f, Range(0.22f, 0.28f), Range(0.9f, 1.2f)));
		if (Random.FRand() < 0.6f)
		{
			D.RaisedDecks.Add(Deck(Range(0.86f, 0.9f), 1.f, 0.8f));
		}
		D.PlankWidth = 0.24f;
		D.PlankThickness = 0.06f;
		D.Wales = { D.Depth - D.Draft - 0.3f, D.Depth - D.Draft - 0.85f };
		D.Guns = Random.RandRange(5, 7);
		D.GunLength = 2.f;
		D.MastHeight = Range(0.95f, 1.08f);
		D.Bowsprit = Range(0.42f, 0.5f);
		D.TopsideColor = FLinearColor(0.12f, 0.08f, 0.045f);
		D.BandColor = Random.FRand() < 0.6f ? FLinearColor(0.34f, 0.22f, 0.05f, 1.f) : FLinearColor(0.f, 0.f, 0.f, 0.f);
		break;

	case EShipType::Frigate:
		// A sixth rate of c. 1700: a single battery on the upper deck, quarterdeck and forecastle, ochre band and
		// blackened wales.
		D.Length = Range(32.f, 37.f);
		D.Beam = D.Length * Range(0.27f, 0.29f);
		D.Depth = D.Length * Range(0.13f, 0.14f);
		D.Draft = D.Depth * Range(0.76f, 0.82f);
		D.KeelDepth = 0.35f;
		D.MaxBeamHeight = Range(0.68f, 0.74f);
		D.SectionFullness = Range(2.2f, 2.6f);
		D.BowFullness = Range(2.4f, 2.8f);
		D.SternFullness = Range(1.6f, 1.9f);
		D.Tumblehome = Range(0.06f, 0.09f);
		D.SheerFore = Range(0.7f, 1.f);
		D.SheerAft = Range(0.5f, 0.75f);
		D.StemRake = D.Depth * Range(0.55f, 0.7f);
		D.SternRake = D.Depth * Range(0.08f, 0.14f);
		D.TransomWidth = Range(0.62f, 0.7f);
		D.TransomBottom = Range(0.6f, 0.66f);
		D.Bulwark = Range(1.2f, 1.4f);
		D.RaisedDecks.Add(Deck(0.f, Range(0.32f, 0.38f), Range(1.9f, 2.1f)));
		D.RaisedDecks.Add(Deck(Range(0.82f, 0.86f), 1.f, Range(1.8f, 2.f)));
		D.PlankWidth = 0.26f;
		D.PlankThickness = 0.08f;
		D.Wales = { D.Depth - D.Draft - 0.35f, D.Depth - D.Draft - 0.9f };
		D.Guns = Random.RandRange(9, 11);
		D.GunLength = 2.5f;
		D.MastHeight = Range(1.f, 1.1f);
		D.Bowsprit = Range(0.36f, 0.42f);
		D.TopsideColor = Vary(Random, FLinearColor(0.13f, 0.085f, 0.045f), 0.1f);
		D.BandColor = FLinearColor(0.42f, 0.28f, 0.07f, 1.f);
		break;

	case EShipType::Galleon:
		// Spanish galleon: high castles fore and aft, strong tumblehome, two batteries, red and black paint.
		D.Length = Range(34.f, 40.f);
		D.Beam = D.Length * Range(0.29f, 0.32f);
		D.Depth = D.Length * Range(0.17f, 0.19f);
		D.Draft = D.Depth * Range(0.6f, 0.65f);
		D.KeelDepth = 0.4f;
		D.MaxBeamHeight = Range(0.62f, 0.68f);
		D.SectionFullness = Range(2.4f, 2.9f);
		D.BowFullness = Range(2.6f, 3.f);
		D.SternFullness = Range(1.8f, 2.2f);
		D.Tumblehome = Range(0.14f, 0.2f);
		D.SheerFore = Range(1.2f, 1.6f);
		D.SheerAft = Range(1.f, 1.4f);
		D.StemRake = D.Depth * Range(0.6f, 0.75f);
		D.SternRake = D.Depth * Range(0.08f, 0.12f);
		D.TransomWidth = Range(0.6f, 0.68f);
		D.TransomBottom = Range(0.55f, 0.6f);
		D.Bulwark = Range(1.1f, 1.3f);
		D.RaisedDecks.Add(Deck(0.f, Range(0.36f, 0.42f), Range(2.2f, 2.4f)));
		D.RaisedDecks.Add(Deck(0.f, Range(0.18f, 0.22f), Range(2.f, 2.2f)));
		D.RaisedDecks.Add(Deck(Range(0.8f, 0.84f), 0.97f, Range(2.2f, 2.5f)));
		D.PlankWidth = 0.28f;
		D.PlankThickness = 0.09f;
		D.Wales = { D.Depth - D.Draft - 2.7f, D.Depth - D.Draft - 0.3f };
		D.Guns = Random.RandRange(5, 7);
		D.LowerGuns = Random.RandRange(8, 10);
		D.GunLength = 2.6f;
		D.MastHeight = Range(0.85f, 0.95f);
		D.Bowsprit = Range(0.36f, 0.42f);
		D.bBonaventure = Random.FRand() < 0.5f;
		D.TopsideColor = FLinearColor(0.07f, 0.05f, 0.035f);
		D.BandColor = Random.FRand() < 0.6f ? FLinearColor(0.3f, 0.045f, 0.03f, 1.f) : FLinearColor(0.36f, 0.25f, 0.07f, 1.f);
		break;
	}
	D.TopsideColor = Vary(Random, D.TopsideColor, 0.12f);
	D.BottomColor = Vary(Random, D.BottomColor, 0.1f);
	D.DeckColor = Vary(Random, D.DeckColor, 0.12f);
	D.SailColor = Vary(Random, D.SailColor, 0.08f);
	return D;
}
