#include "ShipRig.h"

#include "ShipHull.h"
#include "ShipMeshBuilder.h"

using namespace ShipMesh;

namespace
{
	enum class ECourse : uint8
	{
		Square, // a square course on the lower yard
		Gaff,   // a gaff sail on the lower mast (sloops, the brigantine's main)
		Lateen, // a lateen yard (mizzen, bonaventure)
	};

	struct FMastSpec
	{
		float XShare;      // of the half-length, from midships
		float HeightShare; // of the tallest mast
		ECourse Course;
		int32 Squares;     // square sails above the course: 1 topsail, 2 topsail and topgallant
		int32 Shrouds;     // lower shrouds a side
	};

	/** One mast, laid out. Heights along the mast from the deck. */
	struct FMast
	{
		FMastSpec Spec;
		FVector3f Foot, Dir, Fwd;
		float Lower = 0.f, Top = 0.f, TopGallant = 0.f;
		float Doubling = 0.f, TopDoubling = 0.f;
		float RLower = 0.f, RTop = 0.f, RTg = 0.f;
		float TopHalfY = 0.f, TopHalfX = 0.f;
		FVector3f TopmastHeel, TgHeel;

		FVector3f LowerAt(float H) const { return Foot + Dir * H; }
		FVector3f LowerHead() const { return LowerAt(Lower); }
		/** The top's platform (centre, on the lower mast). */
		FVector3f TopCenter() const { return LowerAt(Lower - Doubling); }
		FVector3f TopmastAt(float H) const { return TopmastHeel + Dir * H; }
		FVector3f TopmastHead() const { return TopmastAt(Top); }
		FVector3f TgAt(float H) const { return TgHeel + Dir * H; }
		FVector3f TgHead() const { return TgAt(TopGallant); }
		FVector3f Highest() const { return TopGallant > 0.f ? TgHead() : (Top > 0.f ? TopmastHead() : LowerHead()); }
	};

	/** A yard and its two arms (port, starboard). */
	struct FYard
	{
		FVector3f Center;
		FVector3f Axis; // towards starboard
		float Half = 1.f;
		FVector3f Arm(float Side, float Share = 1.f) const { return Center + Axis * (Side * Half * Share); }
	};

	const FLinearColor MastWood(0.3f, 0.22f, 0.13f, 1.f);
	const FLinearColor Tarred(0.035f, 0.03f, 0.025f, 1.f);
	const FLinearColor BlockWood(0.12f, 0.08f, 0.045f, 1.f);

	struct FRigBuild
	{
		FBuilder& B;
		const FHullLines& L;
		const FShipDesign& D;
		TArray<FMast> Masts;
		/** Rope scale: thicker on bigger ships. */
		float K = 1.f;
		/** The wind comes over the port quarter: square sails belly forward, fore-and-aft sails to starboard. */
		const float Leeward = 1.f;
		/** Yards braced round: their starboard arm forward. */
		const float Brace = FMath::DegreesToRadians(14.f);
		FVector3f BowspritStart, BowspritDir, BowspritEnd, JibBoomEnd;
		bool bJibBoom = false;

		FLinearColor SailColor(float U, float V, int32 Id) const
		{
			// Weathered canvas: greyer and darker towards the foot, each sail a little different.
			FLinearColor C = Jitter(D.SailColor, 0.06f, Id, D.Seed);
			const float Grime = FMath::Lerp(1.f, 0.82f, FMath::Pow(V, 1.6f));
			return FLinearColor(C.R * Grime, C.G * Grime, C.B * Grime * 0.98f, 1.f);
		}

		void Layout();
		void Spars();
		void Bowsprit();
		void SquareSail(const FVector3f& HeadL, const FVector3f& HeadR, const FVector3f& FootL, const FVector3f& FootR, float Depth, int32 Id);
		/** A sail of four corners: luff (Throat-Tack) on a mast, head on a gaff, foot on a boom, bellied to leeward. */
		void QuadSail(const FVector3f& Throat, const FVector3f& Peak, const FVector3f& Clew, const FVector3f& Tack, float Depth, int32 Id);
		/** A triangular sail: luff Head-Tack on a stay or a yard. */
		void TriSail(const FVector3f& Head, const FVector3f& Tack, const FVector3f& Clew, float Depth, int32 Id);
		FYard Yard(const FVector3f& Center, float Half, float Radius);
		void Furled(const FYard& Y, float Radius);
		void Block(const FVector3f& At, const FVector3f& Hang);
		void Rigging();
		void Shrouds(const FMast& M, int32 MastIndex);
		void Ratlines(const TArray<FVector3f>& Bottoms, const TArray<FVector3f>& Tops, float From, float To);
		FVector3f Rail(float X, float Side, float Below = 0.f) const;
	};

	void FRigBuild::Layout()
	{
		TArray<FMastSpec> Specs;
		switch (D.Type)
		{
		case EShipType::Sloop:
			Specs = { { 0.34f, 1.f, ECourse::Gaff, 1, 3 } };
			break;
		case EShipType::Brigantine:
			Specs = { { 0.56f, 0.93f, ECourse::Square, 2, 4 }, { -0.12f, 1.f, ECourse::Gaff, 1, 4 } };
			break;
		case EShipType::Frigate:
			Specs = { { 0.6f, 0.9f, ECourse::Square, 2, 6 }, { 0.f, 1.f, ECourse::Square, 2, 6 }, { -0.56f, 0.7f, ECourse::Lateen, 1, 4 } };
			break;
		case EShipType::Galleon:
			Specs = { { 0.62f, 0.86f, ECourse::Square, 1, 6 }, { 0.02f, 1.f, ECourse::Square, 2, 7 }, { -0.46f, 0.62f, ECourse::Lateen, 1, 4 } };
			if (D.bBonaventure)
			{
				Specs.Add({ -0.74f, 0.46f, ECourse::Lateen, 0, 3 });
			}
			break;
		}
		const float Tallest = D.MastHeight * D.Length;
		for (int32 I = 0; I < Specs.Num(); ++I)
		{
			FMast M;
			M.Spec = Specs[I];
			const float Height = Tallest * M.Spec.HeightShare;
			const float Rake = FMath::DegreesToRadians(D.Type == EShipType::Sloop ? 5.f : 1.f + 1.6f * I);
			M.Dir = FVector3f(-FMath::Sin(Rake), 0.f, FMath::Cos(Rake));
			M.Fwd = FVector3f(FMath::Cos(Rake), 0.f, FMath::Sin(Rake));
			const float X = M.Spec.XShare * L.HalfLength;
			M.Foot = FVector3f(X, 0.f, L.Z(L.DeckAt(X)));
			if (M.Spec.Squares == 2)
			{
				M.Lower = Height * 0.52f;
				M.Top = Height * 0.34f;
				M.TopGallant = Height * 0.2f;
			}
			else if (M.Spec.Squares == 1)
			{
				M.Lower = Height * (M.Spec.Course == ECourse::Gaff ? 0.68f : 0.6f);
				M.Top = Height * (M.Spec.Course == ECourse::Gaff ? 0.4f : 0.46f);
			}
			else
			{
				M.Lower = Height;
			}
			M.Doubling = M.Lower * 0.11f;
			M.TopDoubling = M.Top * 0.12f;
			M.RLower = M.Lower / 64.f + 0.06f;
			M.RTop = M.Top / 72.f + 0.035f;
			M.RTg = M.TopGallant / 80.f + 0.025f;
			M.TopHalfY = (D.Type == EShipType::Sloop ? 0.1f : 0.17f) * D.Beam * FMath::Sqrt(M.Spec.HeightShare) + 0.25f;
			M.TopHalfX = M.TopHalfY * 0.6f;
			M.TopmastHeel = M.LowerAt(M.Lower - M.Doubling - 0.3f) + M.Fwd * (M.RLower + M.RTop + 0.04f);
			M.TgHeel = M.TopmastAt(M.Top - M.TopDoubling - 0.2f) + M.Fwd * (M.RTop + M.RTg + 0.03f);
			Masts.Add(M);
		}
	}

	FYard FRigBuild::Yard(const FVector3f& Center, float Half, float Radius)
	{
		FYard Y;
		Y.Center = Center;
		Y.Axis = FVector3f(FMath::Sin(Brace), FMath::Cos(Brace), 0.f);
		Y.Half = Half;
		const FVector3f Points[5] = { Y.Arm(-1.f), Y.Arm(-1.f, 0.5f), Center, Y.Arm(1.f, 0.5f), Y.Arm(1.f) };
		const float Radii[5] = { Radius * 0.45f, Radius * 0.85f, Radius, Radius * 0.85f, Radius * 0.45f };
		B.Tube(Wood, Points, Radii, 8, Tarred, true);
		// Blocks at the arms (lifts, braces) and the footropes under the yard.
		for (float Side : { -1.f, 1.f })
		{
			Block(Y.Arm(Side, 0.97f) - FVector3f(0.f, 0.f, Radius), FVector3f(0.f, 0.f, -1.f));
			B.Line(Y.Arm(Side, 0.12f) - FVector3f(0.f, 0.f, Radius * 0.8f), Y.Arm(Side, 0.96f) - FVector3f(0.f, 0.f, Radius * 0.8f),
				0.013f * K, FMath::Min(0.7f, Half * 0.12f), FBuilder::TarredRopeColor);
		}
		return Y;
	}

	void FRigBuild::Block(const FVector3f& At, const FVector3f& Hang)
	{
		FShipFrame F = FShipFrame::Along(At + Hang * 0.09f * K, Hang, FVector3f::ForwardVector);
		B.Box(Wood, F, FVector3f(0.09f, 0.05f, 0.035f) * K, BlockWood);
	}

	void FRigBuild::Furled(const FYard& Y, float Radius)
	{
		const FVector3f Up(0.f, 0.f, Radius * 0.9f);
		TArray<FVector3f> Points;
		TArray<float> Radii;
		for (int32 I = 0; I <= 12; ++I)
		{
			const float T = -0.88f + 1.76f * I / 12;
			Points.Add(Y.Center + Y.Axis * (T * Y.Half) + Up + FVector3f(0.03f, 0.f, 0.f));
			Radii.Add(Radius * (0.65f + 0.35f * FMath::Sqrt(FMath::Max(1.f - T * T, 0.f))) * (0.9f + 0.2f * Hash(I, int32(Y.Center.Z * 10.f))));
		}
		B.Tube(Sail, Points, Radii, 8, D.SailColor * 0.85f, true);
		// Gaskets binding it.
		for (int32 I = 1; I < 6; ++I)
		{
			const FVector3f C = Y.Center + Y.Axis * (Y.Half * (-0.8f + 1.6f * I / 6)) + Up;
			B.Tube(Rope, C - Y.Axis * 0.02f, C + Y.Axis * 0.02f, Radius * 1.05f, Radius * 1.05f, 8, FBuilder::RopeColor);
		}
	}

	void FRigBuild::SquareSail(const FVector3f& HL, const FVector3f& HR, const FVector3f& FL, const FVector3f& FR, float Depth, int32 Id)
	{
		const FVector3f Across = HR - HL;
		const FVector3f Down = (FL + FR) * 0.5f - (HL + HR) * 0.5f;
		FVector3f Belly = FVector3f::CrossProduct(Across, Down).GetSafeNormal();
		if (Belly.X < 0.f)
		{
			Belly = -Belly;
		}
		const float Height = Down.Size();
		auto Position = [&](float U, float V)
		{
			const FVector3f Top = FMath::Lerp(HL, HR, U), Bottom = FMath::Lerp(FL, FR, U);
			FVector3f P = FMath::Lerp(Top, Bottom, V);
			const float Arch = 4.f * U * (1.f - U);
			// The foot's roach (curved up), the belly: held at the head on the yard, deepest a third of the way down.
			P.Z += Arch * V * V * Height * 0.07f;
			const float Bag = FMath::Pow(Arch, 0.85f) * (1.f - FMath::Pow(1.f - V, 2.4f)) * (1.f - 0.35f * V);
			return P + Belly * (Depth * Bag);
		};
		TFunction<FLinearColor(float, float)> ColorFn = [&](float U, float V) { return SailColor(U, V, Id); };
		TFunctionRef<FLinearColor(float, float)> ColorRef(ColorFn);
		B.Grid(Sail, 16, 14, Position, D.SailColor, false, &ColorRef);
		// Bolt ropes along the leeches and the foot.
		TArray<FVector3f> Leech0, Leech1, Foot;
		TArray<float> Radii;
		for (int32 I = 0; I <= 14; ++I)
		{
			Leech0.Add(Position(0.f, I / 14.f));
			Leech1.Add(Position(1.f, I / 14.f));
			Foot.Add(Position(I / 14.f, 1.f));
			Radii.Add(0.02f * K);
		}
		B.Tube(Rope, Leech0, Radii, 4, FBuilder::RopeColor);
		B.Tube(Rope, Leech1, Radii, 4, FBuilder::RopeColor);
		B.Tube(Rope, Foot, Radii, 4, FBuilder::RopeColor);
	}

	void FRigBuild::QuadSail(const FVector3f& Throat, const FVector3f& Peak, const FVector3f& Clew, const FVector3f& Tack, float Depth, int32 Id)
	{
		const FVector3f Normal = FVector3f::CrossProduct(Peak - Throat, Tack - Throat).GetSafeNormal() * (FVector3f::CrossProduct(Peak - Throat, Tack - Throat).GetSafeNormal().Y * Leeward < 0.f ? -1.f : 1.f);
		auto Position = [&](float U, float V)
		{
			const FVector3f P = FMath::Lerp(FMath::Lerp(Throat, Peak, U), FMath::Lerp(Tack, Clew, U), V);
			const float Bag = FMath::Pow(4.f * U * (1.f - U), 0.8f) * FMath::Pow(FMath::Max(4.f * V * (1.f - V), 0.f), 0.6f);
			return P + Normal * (Depth * Bag);
		};
		TFunction<FLinearColor(float, float)> ColorFn = [&](float U, float V) { return SailColor(U, V, Id); };
		TFunctionRef<FLinearColor(float, float)> ColorRef(ColorFn);
		B.Grid(Sail, 16, 14, Position, D.SailColor, false, &ColorRef);
		TArray<FVector3f> Leech;
		TArray<float> Radii;
		for (int32 I = 0; I <= 14; ++I)
		{
			Leech.Add(Position(1.f, I / 14.f));
			Radii.Add(0.02f * K);
		}
		B.Tube(Rope, Leech, Radii, 4, FBuilder::RopeColor);
		// Mast hoops along the luff.
		for (int32 I = 1; I < 8; ++I)
		{
			const FVector3f P = FMath::Lerp(Throat, Tack, I / 8.f);
			B.Lathe(Wood, P - FVector3f(0.f, 0.f, 0.03f), FVector3f::UpVector,
				{ FVector2f(0.f, 0.24f * K), FVector2f(0.06f, 0.24f * K) }, 10, BlockWood);
		}
	}

	void FRigBuild::TriSail(const FVector3f& Head, const FVector3f& Tack, const FVector3f& Clew, float Depth, int32 Id)
	{
		FVector3f Normal = FVector3f::CrossProduct(Tack - Head, Clew - Head).GetSafeNormal();
		if (Normal.Y * Leeward < 0.f)
		{
			Normal = -Normal;
		}
		auto Position = [&](float U, float V)
		{
			const FVector3f P = FMath::Lerp(FMath::Lerp(Head, Tack, V), FMath::Lerp(Head, Clew, V), U);
			const float Bag = FMath::Pow(4.f * U * (1.f - U), 0.8f) * FMath::Min(1.f, 2.2f * V) * (1.f - 0.4f * V);
			return P + Normal * (Depth * Bag);
		};
		TFunction<FLinearColor(float, float)> ColorFn = [&](float U, float V) { return SailColor(U, V, Id); };
		TFunctionRef<FLinearColor(float, float)> ColorRef(ColorFn);
		B.Grid(Sail, 12, 16, Position, D.SailColor, false, &ColorRef);
		TArray<FVector3f> Leech, Foot;
		TArray<float> Radii;
		for (int32 I = 0; I <= 12; ++I)
		{
			Leech.Add(Position(1.f, I / 12.f));
			Foot.Add(Position(I / 12.f, 1.f));
			Radii.Add(0.018f * K);
		}
		B.Tube(Rope, Leech, Radii, 4, FBuilder::RopeColor);
		B.Tube(Rope, Foot, Radii, 4, FBuilder::RopeColor);
	}

	FVector3f FRigBuild::Rail(float X, float Side, float Below) const
	{
		const float H = L.RailAt(X) - Below;
		FVector3f N;
		const FVector3f P = L.SidePoint(X, H, Side, &N);
		return P + N * (D.PlankThickness * 0.5f) - FVector3f(0.f, Side, 0.f) * (L.FrameThickness * 0.5f);
	}

	void FRigBuild::Bowsprit()
	{
		const float Top = L.RailAt(L.HalfLength);
		const float Steeve = FMath::DegreesToRadians(D.Type == EShipType::Galleon ? 28.f : (D.Type == EShipType::Sloop ? 10.f : 18.f));
		BowspritDir = FVector3f(FMath::Cos(Steeve), 0.f, FMath::Sin(Steeve));
		BowspritStart = FVector3f(L.XBow(Top) - 2.f, 0.f, L.Z(Top) + 0.05f);
		const float Length = D.Bowsprit * D.Length + 2.f;
		BowspritEnd = BowspritStart + BowspritDir * Length;
		const float R = Masts.Num() ? Masts.Last().RLower : 0.3f;
		const float R0 = (D.Type == EShipType::Sloop ? Masts[0].RLower * 0.8f : R) * 0.95f;
		{
			const FVector3f Points[3] = { BowspritStart, BowspritStart + BowspritDir * (Length * 0.35f), BowspritEnd };
			const float Radii[3] = { R0, R0, R0 * 0.62f };
			B.Tube(Wood, Points, Radii, 10, MastWood, true);
		}
		// Gammoning lashings at the stem head.
		for (int32 I = 0; I < 3; ++I)
		{
			const FVector3f P = BowspritStart + BowspritDir * (2.1f + I * 0.14f);
			B.Tube(Rope, P, P + BowspritDir * 0.1f, R0 * 1.12f, R0 * 1.12f, 10, FBuilder::TarredRopeColor);
		}
		bJibBoom = D.Type == EShipType::Brigantine || D.Type == EShipType::Frigate;
		JibBoomEnd = BowspritEnd;
		if (bJibBoom)
		{
			const FVector3f Heel = BowspritEnd - BowspritDir * (Length * 0.3f) + FVector3f(0.f, 0.f, R0 * 0.8f + 0.1f);
			JibBoomEnd = Heel + BowspritDir * (Length * 0.85f);
			B.Tube(Wood, Heel, JibBoomEnd, R0 * 0.55f, R0 * 0.35f, 8, MastWood, true);
		}
		// Bowsprit cap.
		{
			FShipFrame F = FShipFrame::Along(BowspritEnd, BowspritDir);
			B.Box(Wood, F, FVector3f(0.1f, R0 * 1.3f, R0 * (bJibBoom ? 2.2f : 1.2f)), Tarred);
		}
		// Bobstays to the stem near the waterline.
		{
			const float H = D.Draft + 0.6f;
			const FVector3f Stem(L.XBow(H) + 0.25f, 0.f, L.Z(H));
			B.Line(BowspritStart + BowspritDir * (Length * 0.72f), Stem, 0.04f * K, 0.f, FBuilder::TarredRopeColor);
		}
		// Galleons: a sprit topmast at the end, with its yard.
		if (D.Type == EShipType::Galleon)
		{
			const FVector3f Top0 = BowspritEnd;
			const FVector3f Top1 = Top0 + FVector3f(0.15f, 0.f, D.Length * 0.12f);
			B.Tube(Wood, Top0, Top1, R0 * 0.4f, R0 * 0.3f, 8, MastWood, true);
			Yard(FMath::Lerp(Top0, Top1, 0.8f), D.Beam * 0.3f, 0.08f);
		}
		// Spritsail under the bowsprit (frigates, galleons).
		if (D.Type == EShipType::Frigate || D.Type == EShipType::Galleon)
		{
			const FYard Y = Yard(BowspritStart + BowspritDir * (Length * 0.68f) - FVector3f(0.f, 0.f, R0 + 0.35f), D.Beam * 0.62f, 0.12f * K);
			if (D.bSailsSet)
			{
				const float Drop = Y.Half * 0.95f;
				SquareSail(Y.Arm(-1.f, 0.93f), Y.Arm(1.f, 0.93f), Y.Arm(-1.f, 1.f) - FVector3f(0.6f, 0.f, Drop),
					Y.Arm(1.f, 1.f) - FVector3f(0.6f, 0.f, Drop), Y.Half * 0.22f, 90);
			}
			else
			{
				Furled(Y, 0.13f);
			}
		}
	}

	void FRigBuild::Spars()
	{
		for (int32 I = 0; I < Masts.Num(); ++I)
		{
			const FMast& M = Masts[I];
			// Lower mast: through the deck, its head (the doubling) blackened; woolding bands.
			{
				const FVector3f Points[3] = { M.Foot - M.Dir * 0.6f, M.LowerAt(M.Lower - M.Doubling), M.LowerHead() };
				const float Radii[3] = { M.RLower, M.RLower * 0.86f, M.RLower * 0.8f };
				B.Tube(Wood, TConstArrayView<FVector3f>(Points, 2), TConstArrayView<float>(Radii, 2), 14, MastWood, false);
				B.Tube(Wood, TConstArrayView<FVector3f>(Points + 1, 2), TConstArrayView<float>(Radii + 1, 2), 14, Tarred, true);
				for (float H = 1.6f; H < M.Lower * 0.6f; H += 1.25f)
				{
					const float R = FMath::Lerp(M.RLower, M.RLower * 0.86f, H / (M.Lower - M.Doubling)) + 0.012f;
					B.Tube(Rope, M.LowerAt(H), M.LowerAt(H + 0.14f), R, R, 14, FBuilder::TarredRopeColor);
				}
				// Partners: a collar where it passes through the deck.
				B.Lathe(Wood, M.Foot, M.Dir, { FVector2f(0.f, M.RLower + 0.18f), FVector2f(0.12f, M.RLower + 0.12f), FVector2f(0.14f, M.RLower) }, 14, Tarred);
			}
			// The top: a platform on trestle and cross trees; the cap over the doubling.
			if (M.Top > 0.f)
			{
				FShipFrame F;
				F.O = M.TopCenter() + M.Fwd * (M.TopHalfX * 0.2f);
				F.X = M.Fwd;
				F.Y = FVector3f::RightVector;
				F.Z = M.Dir;
				B.Box(Wood, F, FVector3f(M.TopHalfX, M.TopHalfY, 0.06f), MastWood * 0.7f);
				// Rim.
				FShipFrame Rim = F;
				Rim.O = F.O - M.Fwd * (M.TopHalfX - 0.05f) + M.Dir * 0.12f;
				B.Box(Wood, Rim, FVector3f(0.05f, M.TopHalfY, 0.07f), Tarred);
				for (float Side : { -1.f, 1.f })
				{
					FShipFrame T = F;
					T.O = F.O + FVector3f::RightVector * (Side * (M.RLower + 0.12f)) - M.Dir * 0.14f;
					B.Box(Wood, T, FVector3f(M.TopHalfX * 1.05f, 0.07f, 0.09f), Tarred);
				}
				FShipFrame Cap;
				Cap.X = M.Fwd;
				Cap.Y = FVector3f::RightVector;
				Cap.Z = M.Dir;
				const float Span = M.RLower + M.RTop + 0.04f;
				Cap.O = M.LowerHead() + M.Fwd * (Span * 0.5f) + M.Dir * 0.08f;
				B.Box(Wood, Cap, FVector3f(Span * 0.5f + M.RTop + 0.08f, FMath::Max(M.RLower, M.RTop) + 0.08f, 0.13f), Tarred);
				// Topmast.
				const FVector3f Tm[3] = { M.TopmastHeel, M.TopmastAt(M.Top - M.TopDoubling), M.TopmastHead() };
				const float Tr[3] = { M.RTop, M.RTop * 0.82f, M.RTop * 0.75f };
				B.Tube(Wood, TConstArrayView<FVector3f>(Tm, 2), TConstArrayView<float>(Tr, 2), 10, MastWood, true);
				B.Tube(Wood, TConstArrayView<FVector3f>(Tm + 1, 2), TConstArrayView<float>(Tr + 1, 2), 10, Tarred, true);
				// Crosstrees at the topmast head.
				FShipFrame Ct = Cap;
				Ct.O = M.TopmastAt(M.Top - M.TopDoubling);
				B.Box(Wood, Ct, FVector3f(0.08f, M.TopHalfY * 0.45f + 0.2f, 0.05f), Tarred);
				if (M.TopGallant > 0.f)
				{
					B.Tube(Wood, M.TgHeel, M.TgHead(), M.RTg, M.RTg * 0.7f, 8, MastWood, true);
					FShipFrame Cap2 = Cap;
					Cap2.O = M.TopmastHead() + M.Fwd * ((M.RTop + M.RTg) * 0.5f) + M.Dir * 0.06f;
					B.Box(Wood, Cap2, FVector3f((M.RTop + M.RTg) * 0.5f + M.RTg + 0.05f, M.RTop + 0.05f, 0.09f), Tarred);
				}
				// The truck on top.
				const FVector3f Head = M.Highest();
				B.Lathe(Wood, Head, M.Dir, { FVector2f(0.f, 0.f), FVector2f(0.f, 0.12f), FVector2f(0.1f, 0.12f), FVector2f(0.1f, 0.f) }, 10, Tarred);
			}

			// Yards and sails.
			const int32 Id = I * 10;
			const float DeckZ = M.Foot.Z;
			const float Scale = FMath::Pow(M.Spec.HeightShare, 0.7f);
			if (M.Spec.Course == ECourse::Square)
			{
				const float CourseHalf = (D.Type == EShipType::Galleon ? 1.05f : 1.f) * D.Beam * Scale;
				const FYard Course = Yard(M.LowerAt(M.Lower - M.Doubling - 0.9f) + M.Fwd * (M.RLower + 0.28f), CourseHalf, CourseHalf / 55.f + 0.06f);
				FYard Top = Course, Tg = Course;
				if (M.Spec.Squares >= 1)
				{
					Top = Yard(M.TopmastAt(M.Top - M.TopDoubling - 0.6f) + M.Fwd * (M.RTop + 0.18f), CourseHalf * 0.74f, CourseHalf / 75.f + 0.05f);
				}
				if (M.Spec.Squares >= 2)
				{
					Tg = Yard(M.TgAt(M.TopGallant - 0.7f) + M.Fwd * (M.RTg + 0.12f), CourseHalf * 0.5f, CourseHalf / 100.f + 0.035f);
				}
				if (D.bSailsSet)
				{
					const float RailZ = L.Z(L.RailAt(M.Foot.X)) + 0.6f;
					const FVector3f Clew(-0.6f, 0.f, 0.f);
					SquareSail(Course.Arm(-1.f, 0.95f), Course.Arm(1.f, 0.95f),
						FVector3f(Course.Arm(-1.f, 1.05f).X, Course.Arm(-1.f, 1.05f).Y, RailZ) + Clew,
						FVector3f(Course.Arm(1.f, 1.05f).X, Course.Arm(1.f, 1.05f).Y, RailZ) + Clew, CourseHalf * 0.3f, Id);
					if (M.Spec.Squares >= 1)
					{
						SquareSail(Top.Arm(-1.f, 0.94f), Top.Arm(1.f, 0.94f), Course.Arm(-1.f, 0.92f) + FVector3f(0.f, 0.f, 0.2f),
							Course.Arm(1.f, 0.92f) + FVector3f(0.f, 0.f, 0.2f), Top.Half * 0.3f, Id + 1);
					}
					if (M.Spec.Squares >= 2)
					{
						SquareSail(Tg.Arm(-1.f, 0.94f), Tg.Arm(1.f, 0.94f), Top.Arm(-1.f, 0.9f) + FVector3f(0.f, 0.f, 0.15f),
							Top.Arm(1.f, 0.9f) + FVector3f(0.f, 0.f, 0.15f), Tg.Half * 0.28f, Id + 2);
					}
					// Course sheets to the side, aft.
					for (float Side : { -1.f, 1.f })
					{
						const FVector3f From = FVector3f(Course.Arm(Side, 1.05f).X, Course.Arm(Side, 1.05f).Y, RailZ) + Clew;
						B.Line(From, Rail(FMath::Max(From.X - 4.f, -L.HalfLength + 1.f), Side), 0.018f * K, 0.25f);
					}
				}
				else
				{
					Furled(Course, 0.2f * K);
					if (M.Spec.Squares >= 1)
					{
						Furled(Top, 0.16f * K);
					}
					if (M.Spec.Squares >= 2)
					{
						Furled(Tg, 0.12f * K);
					}
				}
				// Lifts, braces, halyards.
				const FMast* Aft = I + 1 < Masts.Num() ? &Masts[I + 1] : nullptr;
				const FYard* Yards[3] = { &Course, M.Spec.Squares >= 1 ? &Top : nullptr, M.Spec.Squares >= 2 ? &Tg : nullptr };
				const FVector3f Heads[3] = { M.LowerHead(), M.Top > 0.f ? M.TopmastHead() : M.LowerHead(), M.TopGallant > 0.f ? M.TgHead() : M.Highest() };
				for (int32 Level = 0; Level < 3; ++Level)
				{
					if (!Yards[Level])
					{
						continue;
					}
					const FYard& Y = *Yards[Level];
					B.Line(Y.Center + FVector3f(0.f, 0.f, 0.1f), Heads[Level] - M.Dir * 0.2f, 0.022f * K, 0.f, FBuilder::RopeColor);
					for (float Side : { -1.f, 1.f })
					{
						const FVector3f Arm = Y.Arm(Side, 0.97f);
						B.Line(Arm, Heads[Level] + FVector3f::RightVector * (Side * 0.15f) - M.Dir * 0.3f, 0.014f * K, 0.03f, FBuilder::RopeColor);
						FVector3f To;
						if (Aft && Aft->Spec.Course != ECourse::Gaff)
						{
							const float Hs[3] = { Aft->Lower * 0.55f, Aft->Lower - Aft->Doubling, Aft->Lower + 0.2f };
							To = Aft->LowerAt(FMath::Min(Hs[Level], Aft->Lower)) + FVector3f::RightVector * (Side * Aft->RLower);
						}
						else
						{
							To = Rail(-L.HalfLength * 0.82f, Side);
						}
						B.Line(Arm, To, 0.014f * K, FMath::Min(0.5f, (To - Arm).Size() * 0.015f), FBuilder::RopeColor);
					}
				}
			}
			else if (M.Spec.Course == ECourse::Gaff)
			{
				// Boom swung out to leeward, gaff peaked up; a square topsail above (and a course yard on sloops).
				const float BoomLength = D.Type == EShipType::Sloop ? D.Length * 0.72f : (M.Foot.X - L.XStern(L.DeckAt(M.Foot.X))) + 1.2f;
				const float Swing = FMath::DegreesToRadians(D.bSailsSet ? 16.f : 0.f);
				const FVector3f BoomDir = FVector3f(-FMath::Cos(Swing), Leeward * FMath::Sin(Swing), 0.04f).GetSafeNormal();
				const FVector3f Jaw = M.LowerAt(1.6f) - M.Fwd * (M.RLower + 0.05f);
				const FVector3f BoomEnd = Jaw + BoomDir * BoomLength;
				const float BoomR = FMath::Clamp(BoomLength / 80.f, 0.09f, 0.2f);
				B.Tube(Wood, Jaw, BoomEnd, BoomR, BoomR * 0.75f, 10, MastWood * 0.8f, true);
				const float GaffLength = BoomLength * 0.6f;
				const float Peak = FMath::DegreesToRadians(D.bSailsSet ? 38.f : 8.f);
				const FVector3f GaffDir = FVector3f(-FMath::Cos(Peak) * FMath::Cos(Swing * 0.8f), Leeward * FMath::Cos(Peak) * FMath::Sin(Swing * 0.8f), FMath::Sin(Peak)).GetSafeNormal();
				const FVector3f GaffJaw = D.bSailsSet ? M.LowerAt(M.Lower - M.Doubling - 0.9f) - M.Fwd * (M.RLower + 0.05f) : Jaw + FVector3f(0.f, 0.f, 0.5f);
				const FVector3f GaffEnd = GaffJaw + GaffDir * GaffLength;
				B.Tube(Wood, GaffJaw, GaffEnd, BoomR * 0.85f, BoomR * 0.6f, 10, MastWood * 0.8f, true);
				if (D.bSailsSet)
				{
					QuadSail(GaffJaw - FVector3f(0.f, 0.f, 0.05f), GaffEnd, BoomEnd - BoomDir * 0.3f + FVector3f(0.f, 0.f, 0.1f), Jaw + FVector3f(0.f, 0.f, 0.15f),
						BoomLength * 0.09f, Id);
				}
				else
				{
					FYard Bundle;
					Bundle.Center = (Jaw + BoomEnd) * 0.5f;
					Bundle.Axis = BoomDir;
					Bundle.Half = BoomLength * 0.5f;
					Furled(Bundle, 0.25f * K);
				}
				// Peak and throat halyards, topping lift, main sheet to the taffrail.
				B.Line(GaffEnd, M.LowerAt(M.Lower - M.Doubling * 0.5f), 0.018f * K, 0.1f);
				B.Line(FMath::Lerp(GaffJaw, GaffEnd, 0.5f), M.LowerAt(M.Lower - M.Doubling * 0.3f), 0.018f * K, 0.05f);
				B.Line(BoomEnd, M.LowerHead(), 0.016f * K, 0.f);
				{
					const float Top = L.RailAt(-L.HalfLength);
					const FVector3f Taff(L.XStern(Top) + 0.3f, 0.f, L.Z(Top) + 0.1f);
					B.Line(BoomEnd - BoomDir * 0.8f, Taff, 0.02f * K, 0.1f);
					Block(BoomEnd - BoomDir * 0.8f, (Taff - BoomEnd).GetSafeNormal());
				}
				// Square topsail (and the sloop's course yard, its sail furled: set only before the wind).
				if (M.Spec.Squares >= 1 && M.Top > 0.f)
				{
					const float CourseHalf = (D.Type == EShipType::Sloop ? 0.68f : 0.9f) * D.Beam * Scale;
					const FYard Cross = Yard(M.LowerAt(M.Lower - M.Doubling - 0.6f) + M.Fwd * (M.RLower + 0.25f), CourseHalf, CourseHalf / 55.f + 0.05f);
					const FYard Top = Yard(M.TopmastAt(M.Top - M.TopDoubling - 0.5f) + M.Fwd * (M.RTop + 0.16f), CourseHalf * 0.74f, CourseHalf / 75.f + 0.04f);
					if (D.Type == EShipType::Sloop)
					{
						Furled(Cross, 0.17f * K);
					}
					if (D.bSailsSet)
					{
						SquareSail(Top.Arm(-1.f, 0.94f), Top.Arm(1.f, 0.94f), Cross.Arm(-1.f, 0.92f) + FVector3f(0.f, 0.f, 0.2f),
							Cross.Arm(1.f, 0.92f) + FVector3f(0.f, 0.f, 0.2f), Top.Half * 0.3f, Id + 1);
					}
					else
					{
						Furled(Top, 0.14f * K);
					}
					for (const FYard* Y : { &Cross, &Top })
					{
						for (float Side : { -1.f, 1.f })
						{
							B.Line(Y->Arm(Side, 0.97f), M.Highest() - M.Dir * 0.3f, 0.013f * K, 0.03f);
							B.Line(Y->Arm(Side, 0.97f), Rail(-L.HalfLength * 0.7f, Side), 0.013f * K, 0.3f);
						}
					}
				}
			}
			else // Lateen
			{
				const float YardLength = M.Lower * 1.15f;
				const FVector3f Dir = FVector3f(-FMath::Cos(FMath::DegreesToRadians(42.f)), Leeward * 0.12f, FMath::Sin(FMath::DegreesToRadians(42.f))).GetSafeNormal();
				const FVector3f Hang = M.LowerAt(M.Lower * 0.66f) + M.Fwd * (M.RLower + 0.15f);
				const FVector3f Fore = Hang - Dir * (YardLength * 0.38f);
				const FVector3f AftEnd = Hang + Dir * (YardLength * 0.62f);
				const FVector3f Points[3] = { Fore, Hang, AftEnd };
				const float R = YardLength / 90.f + 0.05f;
				const float Radii[3] = { R * 0.6f, R, R * 0.5f };
				B.Tube(Wood, Points, Radii, 8, Tarred, true);
				B.Line(Hang, M.LowerAt(M.Lower - 0.3f), 0.025f * K);
				if (D.bSailsSet)
				{
					const float Top = L.RailAt(M.Foot.X - 2.f);
					const FVector3f Clew(FMath::Max(AftEnd.X + 1.5f, L.XStern(Top) + 0.8f), Leeward * 0.6f, L.Z(Top) + 1.2f);
					TriSail(AftEnd - Dir * 0.2f, Fore + Dir * 0.3f, Clew, YardLength * 0.07f, Id);
					const FVector3f Taff(L.XStern(L.RailAt(-L.HalfLength)) + 0.4f, Leeward * 0.4f, L.Z(L.RailAt(-L.HalfLength)) + 0.1f);
					B.Line(Clew, Taff, 0.018f * K, 0.1f);
				}
				else
				{
					FYard Bundle;
					Bundle.Center = (Fore + AftEnd) * 0.5f;
					Bundle.Axis = Dir;
					Bundle.Half = YardLength * 0.5f;
					Furled(Bundle, 0.16f * K);
				}
				// A square topsail above (frigate's mizzen).
				if (M.Spec.Squares >= 1 && M.Top > 0.f)
				{
					const float Half = D.Beam * 0.55f * Scale;
					const FYard Cross = Yard(M.LowerAt(M.Lower - M.Doubling - 0.5f) + M.Fwd * (M.RLower + 0.2f), Half, Half / 60.f + 0.04f);
					const FYard Top = Yard(M.TopmastAt(M.Top - M.TopDoubling - 0.4f) + M.Fwd * (M.RTop + 0.14f), Half * 0.72f, Half / 80.f + 0.035f);
					if (D.bSailsSet)
					{
						SquareSail(Top.Arm(-1.f, 0.94f), Top.Arm(1.f, 0.94f), Cross.Arm(-1.f, 0.92f) + FVector3f(0.f, 0.f, 0.2f),
							Cross.Arm(1.f, 0.92f) + FVector3f(0.f, 0.f, 0.2f), Top.Half * 0.28f, Id + 1);
					}
					else
					{
						Furled(Top, 0.12f * K);
					}
				}
			}
		}
	}

	void FRigBuild::Ratlines(const TArray<FVector3f>& Bottoms, const TArray<FVector3f>& Tops, float From, float To)
	{
		// Rungs of tarred line seized across the shrouds every 38 cm, as far up as the shrouds stay apart.
		for (float Z = From; Z < To; Z += 0.38f)
		{
			TArray<FVector3f> Points;
			TArray<float> Radii;
			for (int32 I = 0; I < Bottoms.Num(); ++I)
			{
				const float T = (Z - Bottoms[I].Z) / FMath::Max(Tops[I].Z - Bottoms[I].Z, 0.01f);
				if (T < 0.f || T > 1.f)
				{
					continue;
				}
				Points.Add(FMath::Lerp(Bottoms[I], Tops[I], T));
				Radii.Add(0.011f * K);
			}
			if (Points.Num() >= 2 && (Points.Last() - Points[0]).Size() > 0.35f)
			{
				B.Tube(Rope, Points, Radii, 4, FBuilder::TarredRopeColor);
			}
		}
	}

	void FRigBuild::Shrouds(const FMast& M, int32 MastIndex)
	{
		const int32 Count = M.Spec.Shrouds;
		const float Spacing = D.Type == EShipType::Sloop ? 0.62f : 0.72f;
		const float DeadEye = (D.Type == EShipType::Sloop ? 0.1f : 0.13f) * FMath::Max(K, 0.8f);
		const FVector2f Disc[] = { { -0.04f, 0.f }, { -0.04f, DeadEye * 0.85f }, { -0.025f, DeadEye }, { 0.025f, DeadEye }, { 0.04f, DeadEye * 0.85f }, { 0.04f, 0.f } };
		for (float Side : { -1.f, 1.f })
		{
			TArray<FVector3f> Bottoms, Tops;
			const float X0 = M.Foot.X + 0.3f, X1 = M.Foot.X + 0.3f - Spacing * (Count - 1);
			const float Below = D.Type == EShipType::Sloop ? 0.12f : 0.4f;
			// Chainwale: a thick plank standing out from the side, so the shrouds clear the rail.
			{
				TArray<FVector3f> Path;
				for (int32 I = 0; I <= 8; ++I)
				{
					const float X = FMath::Lerp(X0 + 0.45f, X1 - 0.45f, I / 8.f);
					FVector3f N;
					const FVector3f P = L.SidePoint(X, L.RailAt(X) - Below, Side, &N);
					Path.Add(P + N * (D.PlankThickness + 0.24f));
				}
				B.Beam(Wood, Path, FVector3f::UpVector, 0.5f, 0.1f, FLinearColor(D.WaleColor.R * 1.4f, D.WaleColor.G * 1.4f, D.WaleColor.B * 1.4f, 1.f));
			}
			const FVector3f Head = M.LowerAt(M.Lower - M.Doubling - 0.3f);
			for (int32 I = 0; I < Count; ++I)
			{
				const float X = FMath::Lerp(X0, X1, Count > 1 ? float(I) / (Count - 1) : 0.f);
				const float H = L.RailAt(X) - Below;
				FVector3f N;
				const FVector3f Side0 = L.SidePoint(X, H, Side, &N);
				const FVector3f Lower = Side0 + N * (D.PlankThickness + 0.4f) + FVector3f(0.f, 0.f, 0.05f + DeadEye);
				const FVector3f Top = Head + FVector3f::RightVector * (Side * M.RLower * 0.9f) - M.Fwd * (0.05f * I);
				const FVector3f Up = (Top - Lower).GetSafeNormal();
				const FVector3f Upper = Lower + Up * (0.75f + DeadEye * 2.f);
				const FVector3f Facing = FVector3f(N.X, N.Y, 0.f).GetSafeNormal();
				B.Lathe(Wood, Lower, Facing, Disc, 10, BlockWood);
				B.Lathe(Wood, Upper, Facing, Disc, 10, BlockWood);
				for (int32 J = -1; J <= 1; ++J)
				{
					const FVector3f Off = FVector3f(1.f, 0.f, 0.f) * (J * DeadEye * 0.45f);
					B.Line(Lower + Off, Upper + Off, 0.01f * K, 0.f, FBuilder::RopeColor);
				}
				// Chain plate down the side to below the channel.
				B.Tube(Metal, Lower - FVector3f(0.f, 0.f, DeadEye), L.SidePoint(X, H - 1.1f, Side) + N * (D.PlankThickness + 0.03f), 0.022f, 0.022f, 5, FBuilder::IronColor);
				B.Line(Upper + Up * DeadEye, Top, 0.022f * K, 0.f, FBuilder::TarredRopeColor);
				Bottoms.Add(Upper + Up * (DeadEye + 0.4f));
				Tops.Add(Top);
			}
			Ratlines(Bottoms, Tops, Bottoms[0].Z, Head.Z - 1.4f);
			// Futtock shrouds and topmast shrouds from the rim of the top.
			if (M.Top > 0.f)
			{
				const int32 TopCount = D.Type == EShipType::Sloop ? 2 : 3;
				const FVector3f TopHead = M.TopmastAt(M.Top - M.TopDoubling - 0.2f);
				TArray<FVector3f> TB, TT;
				for (int32 I = 0; I < TopCount; ++I)
				{
					const FVector3f Rim = M.TopCenter() + FVector3f::RightVector * (Side * M.TopHalfY * 0.95f) + M.Fwd * (M.TopHalfX * (0.4f - 0.5f * I)) + M.Dir * 0.1f;
					B.Line(Rim - M.Dir * 0.05f, M.LowerAt(M.Lower - M.Doubling - 1.8f) + FVector3f::RightVector * (Side * M.RLower), 0.022f * K, 0.f, FBuilder::TarredRopeColor);
					const FVector3f Top = TopHead + FVector3f::RightVector * (Side * M.RTop * 0.9f);
					B.Line(Rim + M.Dir * 0.25f, Top, 0.024f * K, 0.f, FBuilder::TarredRopeColor);
					B.Lathe(Wood, Rim + M.Dir * 0.15f, FVector3f::RightVector * Side, Disc, 8, BlockWood);
					TB.Add(Rim + M.Dir * 0.5f);
					TT.Add(Top);
				}
				Ratlines(TB, TT, TB[0].Z, TopHead.Z - 1.f);
				if (M.TopGallant > 0.f)
				{
					const FVector3f Cross = M.TopmastAt(M.Top - M.TopDoubling) + FVector3f::RightVector * (Side * (M.TopHalfY * 0.45f + 0.18f));
					B.Line(Cross, M.TgAt(M.TopGallant - 0.5f), 0.018f * K, 0.f, FBuilder::TarredRopeColor);
					B.Line(Cross + M.Fwd * -0.2f, M.TgAt(M.TopGallant - 0.5f), 0.018f * K, 0.f, FBuilder::TarredRopeColor);
				}
				// Backstays: from the topmast and topgallant heads down to the side, abaft the chainwale.
				for (int32 J = 0; J < 2; ++J)
				{
					const float X = X1 - 0.9f - J * 0.8f;
					const FVector3f Foot = L.SidePoint(X, L.RailAt(X) - Below, Side) + FVector3f(0.f, Side * 0.45f, 0.1f);
					const FVector3f From = J == 0 || M.TopGallant <= 0.f ? TopHead : M.TgAt(M.TopGallant - 0.5f);
					B.Line(Foot, From + FVector3f::RightVector * (Side * 0.1f), 0.026f * K, 0.f, FBuilder::TarredRopeColor);
					B.Lathe(Wood, Foot, FVector3f::RightVector * Side, Disc, 8, BlockWood);
				}
			}
		}
		(void)MastIndex;
	}

	void FRigBuild::Rigging()
	{
		for (int32 I = 0; I < Masts.Num(); ++I)
		{
			Shrouds(Masts[I], I);
		}
		// Stays: each masthead held forward, to the bowsprit or to the foot or the top of the mast ahead.
		for (int32 I = 0; I < Masts.Num(); ++I)
		{
			const FMast& M = Masts[I];
			const float R = 0.04f * K;
			const FVector3f Head = M.LowerAt(M.Lower - M.Doubling - 0.1f) + M.Fwd * M.RLower;
			FVector3f Lower, Upper, Gallant;
			if (I == 0)
			{
				Lower = BowspritStart + BowspritDir * (2.8f + (D.Type == EShipType::Sloop ? 1.5f : 0.f));
				Upper = BowspritEnd - BowspritDir * 0.3f;
				Gallant = JibBoomEnd - BowspritDir * 0.2f;
			}
			else
			{
				const FMast& Ahead = Masts[I - 1];
				Lower = Ahead.Foot + FVector3f(-Ahead.RLower - 0.3f, 0.f, 1.2f);
				Upper = Ahead.TopCenter() + Ahead.Fwd * -0.1f;
				Gallant = Ahead.Top > 0.f ? Ahead.TopmastAt(Ahead.Top - Ahead.TopDoubling) : Ahead.LowerHead();
				if (M.Spec.Course == ECourse::Lateen)
				{
					Lower = Ahead.LowerAt(Ahead.Lower * 0.35f) - Ahead.Fwd * Ahead.RLower;
				}
			}
			B.Line(Head, Lower, R, 0.f, FBuilder::TarredRopeColor);
			if (M.Top > 0.f)
			{
				B.Line(M.TopmastAt(M.Top - M.TopDoubling - 0.1f) + M.Fwd * M.RTop, Upper, R * 0.7f, 0.f, FBuilder::TarredRopeColor);
			}
			if (M.TopGallant > 0.f)
			{
				B.Line(M.TgAt(M.TopGallant - 0.4f) + M.Fwd * M.RTg, Gallant, R * 0.5f, 0.f, FBuilder::TarredRopeColor);
			}
			// Headsails on the foremast's stays: fore staysail and jib.
			if (I == 0 && D.Type != EShipType::Galleon)
			{
				const float RailZ = L.Z(L.RailAt(M.Foot.X + 2.f));
				if (D.bSailsSet)
				{
					// Fore staysail.
					const FVector3f TackS = FMath::Lerp(Lower, Head, 0.04f);
					const FVector3f HeadS = FMath::Lerp(Lower, Head, 0.72f);
					const FVector3f ClewS(M.Foot.X + 1.2f, Leeward * (D.Beam * 0.18f + 0.3f), RailZ + 1.2f);
					TriSail(HeadS, TackS, ClewS, (HeadS - TackS).Size() * 0.06f, 80);
					B.Line(ClewS, Rail(M.Foot.X - 0.5f, Leeward), 0.016f * K, 0.1f);
					// Jib on the topmast stay.
					if (M.Top > 0.f)
					{
						const FVector3f From = M.TopmastAt(M.Top - M.TopDoubling - 0.1f) + M.Fwd * M.RTop;
						const FVector3f TackJ = FMath::Lerp(Upper, From, 0.03f);
						const FVector3f HeadJ = FMath::Lerp(Upper, From, 0.7f);
						const FVector3f ClewJ(FMath::Lerp(TackJ.X, M.Foot.X, 0.55f), Leeward * (D.Beam * 0.25f + 0.4f), FMath::Max(RailZ + 2.2f, TackJ.Z - 1.f));
						TriSail(HeadJ, TackJ, ClewJ, (HeadJ - TackJ).Size() * 0.06f, 81);
						B.Line(ClewJ, Rail(M.Foot.X + 0.5f, Leeward), 0.016f * K, 0.3f);
					}
				}
				else
				{
					FYard Bundle;
					Bundle.Center = FMath::Lerp(Lower, Upper, 0.5f) + FVector3f(0.f, 0.f, 0.15f);
					Bundle.Axis = (Upper - Lower).GetSafeNormal();
					Bundle.Half = (Upper - Lower).Size() * 0.45f;
					Furled(Bundle, 0.14f * K);
				}
			}
		}
	}
}

void ShipRig::Build(FBuilder& B, const FHullLines& L, TArray<FShipMastInfo>& OutMasts)
{
	FRigBuild Rig{ B, L, L.D };
	Rig.K = FMath::Clamp(L.D.Length / 28.f, 0.75f, 1.35f);
	Rig.Layout();
	Rig.Bowsprit();
	Rig.Spars();
	Rig.Rigging();
	for (const FMast& M : Rig.Masts)
	{
		FShipMastInfo Info;
		Info.Foot = M.Foot;
		Info.Radius = M.RLower;
		OutMasts.Add(Info);
	}
}
