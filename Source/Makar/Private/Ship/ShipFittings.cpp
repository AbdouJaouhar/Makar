#include "ShipFittings.h"

#include "ShipHull.h"
#include "ShipMeshBuilder.h"
#include "ShipRig.h"

using namespace ShipMesh;

namespace
{
	const FLinearColor DarkWood(0.09f, 0.06f, 0.035f, 1.f);
	const FLinearColor Oak(0.26f, 0.19f, 0.11f, 1.f);
	const FLinearColor Darkness(0.004f, 0.004f, 0.004f, 1.f);
	const FLinearColor Bronze(0.2f, 0.13f, 0.06f, 1.f);       // alpha 1: metallic
	const FLinearColor Gilt(0.5f, 0.36f, 0.12f, 1.f);
	const FLinearColor Brass(0.42f, 0.32f, 0.13f, 1.f);

	FShipFrame Frame(const FVector3f& O, const FVector3f& X, const FVector3f& Z)
	{
		FShipFrame F;
		F.O = O;
		F.X = X.GetSafeNormal();
		F.Y = FVector3f::CrossProduct(Z, F.X).GetSafeNormal();
		F.Z = FVector3f::CrossProduct(F.X, F.Y);
		return F;
	}

	/** A cast gun: cascabel, base ring, reinforces, chase, muzzle swell, bored; trunnions. Breech at Origin. */
	void Barrel(FBuilder& B, const FVector3f& Origin, const FVector3f& Axis, float Length, const FLinearColor& Color)
	{
		const float R = 0.07f + 0.05f * Length;
		const float L = Length;
		const float Bore = R * 0.42f;
		const FVector2f Profile[] = {
			{ -0.62f * R, 0.f }, { -0.6f * R, 0.25f * R }, { -0.48f * R, 0.36f * R }, { -0.36f * R, 0.25f * R }, // button
			{ -0.3f * R, 0.18f * R }, { -0.18f * R, 0.55f * R }, { -0.05f * R, 0.9f * R }, { 0.f, 1.05f * R },     // cascabel
			{ 0.12f * R, 1.05f * R }, { 0.14f * R, 0.98f * R },                                                    // base ring
			{ 0.34f * L, 0.9f * R }, { 0.345f * L, 0.97f * R }, { 0.37f * L, 0.97f * R }, { 0.375f * L, 0.86f * R }, // first reinforce
			{ 0.56f * L, 0.82f * R }, { 0.565f * L, 0.88f * R }, { 0.585f * L, 0.88f * R }, { 0.59f * L, 0.74f * R }, // second
			{ 0.9f * L, 0.62f * R }, { 0.91f * L, 0.68f * R }, { 0.93f * L, 0.66f * R },                           // chase, astragal
			{ 0.97f * L, 0.78f * R }, { L, 0.8f * R }, { L, Bore + 0.01f }, { L - 0.02f, Bore },                   // muzzle swell, face
			{ L - 0.6f, Bore }, { L - 0.6f, 0.f } };
		B.Lathe(Metal, Origin, Axis, Profile, 14, Color);
		// Trunnions.
		const FVector3f Across = FVector3f::CrossProduct(FVector3f::UpVector, Axis).GetSafeNormal();
		const FVector3f T = Origin + Axis * (0.43f * L) - FVector3f(0.f, 0.f, 0.1f * R);
		B.Tube(Metal, T - Across * (1.35f * R), T + Across * (1.35f * R), 0.33f * R, 0.33f * R, 10, Color, true);
	}

	/** A truck carriage under a gun whose trunnions are at Trunnion, on a deck at DeckZ. */
	void Carriage(FBuilder& B, const FVector3f& Trunnion, const FVector3f& Axis, float Length, float DeckZ, int32 Id)
	{
		const float R = 0.07f + 0.05f * Length;
		const FVector3f Across = FVector3f::CrossProduct(FVector3f::UpVector, Axis).GetSafeNormal();
		const float Height = Trunnion.Z - DeckZ;
		const float Truck = FMath::Clamp(Height * 0.32f, 0.13f, 0.24f);
		const FLinearColor Paint = Jitter(FLinearColor(0.28f, 0.08f, 0.04f, 1.f), 0.1f, Id); // red-painted carriages
		for (float Side : { -1.f, 1.f })
		{
			const FVector3f Cheek = Trunnion + Across * (Side * (R * 1.3f + 0.05f));
			// Stepped cheeks: high under the trunnions, lower behind.
			const float Front = 0.22f, Back = Length * 0.45f;
			const float Bottom = DeckZ + Truck * 0.9f;
			FShipFrame F = Frame(Cheek + Axis * (Front - 0.3f) * 0.5f + FVector3f(0.f, 0.f, (Bottom - Trunnion.Z) * 0.5f - 0.02f), Axis, FVector3f::UpVector);
			B.Box(Wood, F, FVector3f((Front + 0.3f) * 0.5f, 0.05f, (Trunnion.Z - Bottom) * 0.5f + 0.02f), Paint);
			const float StepTop = FMath::Lerp(Bottom, Trunnion.Z, 0.62f);
			F = Frame(Cheek - Axis * (0.3f + Back * 0.5f) + FVector3f(0.f, 0.f, (StepTop + Bottom) * 0.5f - Trunnion.Z), Axis, FVector3f::UpVector);
			B.Box(Wood, F, FVector3f(Back * 0.5f, 0.05f, (StepTop - Bottom) * 0.5f), Paint);
			// Cap square over the trunnion.
			F = Frame(Cheek + FVector3f(0.f, 0.f, 0.33f * R), Axis, FVector3f::UpVector);
			B.Box(Metal, F, FVector3f(0.36f * R, 0.055f, 0.03f), FBuilder::IronColor);
		}
		// Axletrees and trucks.
		for (float Along : { 0.05f, -(0.3f + Length * 0.4f) })
		{
			const FVector3f Center = FVector3f(Trunnion.X, Trunnion.Y, DeckZ + Truck) + Axis * Along;
			FShipFrame F = Frame(Center, Across, FVector3f::UpVector);
			B.Box(Wood, F, FVector3f(R * 1.3f + 0.2f, 0.06f, 0.06f), Paint);
			for (float Side : { -1.f, 1.f })
			{
				const FVector3f Hub = Center + Across * (Side * (R * 1.3f + 0.16f));
				const FVector2f Wheel[] = { { -0.05f, 0.f }, { -0.05f, Truck * 0.95f }, { -0.04f, Truck }, { 0.04f, Truck }, { 0.05f, Truck * 0.95f }, { 0.05f, 0.f } };
				B.Lathe(Wood, Hub, Across, Wheel, 12, Paint * 0.8f);
			}
		}
	}

	/** Belaying pins standing in a rail. */
	void Pins(FBuilder& B, const FVector3f& From, const FVector3f& To, float Spacing)
	{
		const int32 Count = FMath::Max(1, FMath::FloorToInt((To - From).Size() / Spacing));
		const FVector2f Pin[] = { { -0.12f, 0.f }, { -0.12f, 0.012f }, { 0.f, 0.015f }, { 0.02f, 0.022f }, { 0.12f, 0.02f }, { 0.14f, 0.f } };
		for (int32 I = 0; I <= Count; ++I)
		{
			B.Lathe(Wood, FMath::Lerp(From, To, float(I) / Count), FVector3f::UpVector, Pin, 6, Oak);
		}
	}

	struct FFitBuild
	{
		FBuilder& B;
		const FHullLines& L;
		const FShipDesign& D;
		const TArray<FShipGunPort>& Ports;
		const TArray<FShipMastInfo>& Masts;
		const FShipInterior& Interior;

		float DeckZ(float X) const { return L.Z(L.DeckAt(X)); }
		float InnerHalf(float X) const { return FMath::Max(L.HalfBreadth(X, L.DeckAt(X)) - L.FrameThickness, 0.f); }
		/** The waist: no raised deck over X. */
		bool InWaist(float X) const { return L.RaisedAt(X) <= 0.f; }

		void Guns();
		void Hatch(float X, float Length, float Width, bool bGrating);
		void Hatches();
		void Capstan(const FVector3f& At);
		void Windlass(float X);
		void Steering();
		void MastFeet();
		void Lanterns();
		void Anchors();
		void Bell();
	};

	void FFitBuild::Guns()
	{
		const FLinearColor Iron = FBuilder::IronColor;
		int32 Id = 0;
		for (const FShipGunPort& Port : Ports)
		{
			for (float Side : { -1.f, 1.f })
			{
				const float Hc = 0.5f * (Port.H0 + Port.H1) - 0.03f;
				FVector3f N;
				const FVector3f Out = L.SidePoint(Port.X, Hc, Side, &N);
				const FVector3f Axis = FVector3f(N.X, N.Y, 0.f).GetSafeNormal();
				const FVector3f Muzzle = Out + Axis * (D.PlankThickness + (Port.bLower ? 0.12f : 0.3f));
				const FVector3f Origin = Muzzle - Axis * D.GunLength;
				Barrel(B, Origin, Axis, D.GunLength, D.Type == EShipType::Galleon ? Bronze : Iron);
				{
					const FVector3f Trunnion = Origin + Axis * (0.43f * D.GunLength);
					Carriage(B, Trunnion, Axis, D.GunLength, L.Z(Port.DeckH), Id++);
					// Breeching rope from the side, round the cascabel.
					const FVector3f Inner = Out - Axis * L.FrameThickness;
					for (float Along : { -1.f, 1.f })
					{
						const FVector3f Ring = Inner + FVector3f(Along * (Port.HalfWidth + 0.25f), 0.f, -0.1f);
						B.Line(Ring, Origin + Axis * 0.05f, 0.025f, 0.18f);
					}
				}
				if (Port.bLower)
				{
					// The lid, hinged at the top and swung up.
					const float Sc = L.StationOf(Port.X, Hc);
					const float T1 = L.GirthAt(Sc, Port.H1);
					const FVector3f Hinge = L.Surface(Sc, T1) * FVector3f(1.f, Side, 1.f) + Axis * (D.PlankThickness + 0.04f);
					const FVector3f Down = (L.SidePoint(Port.X, Port.H0, Side) - L.SidePoint(Port.X, Port.H1, Side)).GetSafeNormal();
					const float Angle = FMath::DegreesToRadians(105.f);
					const FVector3f Dir = Down * FMath::Cos(Angle) + Axis * FMath::Sin(Angle);
					const float Height = Port.H1 - Port.H0 + 0.08f;
					FShipFrame Lid;
					Lid.O = Hinge + Dir * (Height * 0.5f);
					Lid.X = FVector3f(1.f, 0.f, 0.f);
					Lid.Z = Dir;
					Lid.Y = FVector3f::CrossProduct(Lid.Z, Lid.X);
					B.Box(Wood, Lid, FVector3f(Port.HalfWidth + 0.05f, 0.05f, Height * 0.5f), FLinearColor(0.26f, 0.055f, 0.035f, 1.f));
				}
			}
		}
		// Sloops: swivel guns on the rail.
		if (D.Type == EShipType::Sloop || D.Type == EShipType::Brigantine)
		{
			for (float Share : { -0.75f, -0.35f, 0.62f })
			{
				const float X = Share * L.HalfLength;
				for (float Side : { -1.f, 1.f })
				{
					const float H = L.RailAt(X);
					const FVector3f Rail = L.SidePoint(X, H, Side) - FVector3f(0.f, Side * (L.FrameThickness - D.PlankThickness) * 0.5f, 0.f) + FVector3f(0.f, 0.f, 0.1f);
					B.Tube(Metal, Rail, Rail + FVector3f(0.f, 0.f, 0.22f), 0.025f, 0.025f, 6, FBuilder::IronColor);
					const FVector3f Axis = FVector3f(0.35f, Side, 0.08f).GetSafeNormal();
					const FVector3f Pivot = Rail + FVector3f(0.f, 0.f, 0.3f);
					Barrel(B, Pivot - Axis * 0.45f, Axis, 1.f, FBuilder::IronColor);
					B.Tube(Metal, Pivot - Axis * 0.45f, Pivot - Axis * 0.95f, 0.02f, 0.02f, 5, FBuilder::IronColor, true); // tiller
				}
			}
		}
	}

	void FFitBuild::Hatch(float X, float Length, float Width, bool bGrating)
	{
		const float Z = DeckZ(X);
		const float Coaming = 0.32f;
		FShipFrame F;
		F.O = FVector3f(X, 0.f, Z + Coaming * 0.5f);
		const FLinearColor C = Jitter(Oak * 0.8f, 0.1f, int32(X * 10.f));
		for (float Side : { -1.f, 1.f })
		{
			F.O = FVector3f(X + Side * Length * 0.5f, 0.f, Z + Coaming * 0.5f);
			B.Box(Wood, F, FVector3f(0.07f, Width * 0.5f + 0.07f, Coaming * 0.5f), C);
			F.O = FVector3f(X, Side * Width * 0.5f, Z + Coaming * 0.5f);
			B.Box(Wood, F, FVector3f(Length * 0.5f, 0.07f, Coaming * 0.5f), C);
		}
		if (!bGrating)
		{
			return; // an open hatchway: its ladder goes down below
		}
		// Grating: ledges across, battens along, halved into each other.
		const float Top = Z + Coaming - 0.02f;
		for (float Y = -Width * 0.5f + 0.06f; Y < Width * 0.5f; Y += 0.12f)
		{
			TArray<FVector3f> Path = { FVector3f(X - Length * 0.5f, Y, Top - 0.03f), FVector3f(X + Length * 0.5f, Y, Top - 0.03f) };
			B.Beam(Wood, Path, FVector3f::UpVector, 0.045f, 0.05f, C * 1.1f);
		}
		for (float Xi = X - Length * 0.5f + 0.06f; Xi < X + Length * 0.5f; Xi += 0.12f)
		{
			TArray<FVector3f> Path = { FVector3f(Xi, -Width * 0.5f, Top - 0.015f), FVector3f(Xi, Width * 0.5f, Top - 0.015f) };
			B.Beam(Wood, Path, FVector3f::UpVector, 0.04f, 0.03f, C);
		}
	}

	void FFitBuild::Hatches()
	{
		for (const FShipHatch& H : Interior.Hatches)
		{
			Hatch(H.X, H.HalfLength * 2.f, H.HalfWidth * 2.f, !H.bOpen);
		}
	}

	void FFitBuild::Capstan(const FVector3f& At)
	{
		const float S = FMath::Clamp(D.Beam / 9.f, 0.7f, 1.2f);
		const FVector2f Profile[] = { { 0.f, 0.f }, { 0.f, 0.55f * S }, { 0.12f * S, 0.55f * S }, { 0.14f * S, 0.32f * S }, { 0.75f * S, 0.27f * S },
			{ 0.78f * S, 0.5f * S }, { 1.f * S, 0.52f * S }, { 1.02f * S, 0.4f * S }, { 1.08f * S, 0.15f * S }, { 1.1f * S, 0.f } };
		const FLinearColor C = Oak;
		B.Lathe(Wood, At, FVector3f::UpVector, Profile, 16, C);
		// Whelps round the barrel.
		for (int32 I = 0; I < 8; ++I)
		{
			const float A = 2.f * UE_PI * I / 8;
			const FVector3f Out(FMath::Cos(A), FMath::Sin(A), 0.f);
			FShipFrame F = Frame(At + Out * (0.33f * S) + FVector3f(0.f, 0.f, 0.45f * S), Out, FVector3f::UpVector);
			B.Box(Wood, F, FVector3f(0.07f * S, 0.05f * S, 0.3f * S), C * 0.85f);
		}
	}

	void FFitBuild::Windlass(float X)
	{
		const float Z = DeckZ(X);
		const float Half = FMath::Max(InnerHalf(X) - 0.5f, 0.6f);
		// The barrel between two bitts, with its pawl post.
		B.Tube(Wood, FVector3f(X, -Half, Z + 0.55f), FVector3f(X, Half, Z + 0.55f), 0.22f, 0.22f, 8, Oak * 0.9f, true);
		for (float Side : { -1.f, 1.f })
		{
			FShipFrame F;
			F.O = FVector3f(X, Side * (Half + 0.12f), Z + 0.5f);
			B.Box(Wood, F, FVector3f(0.15f, 0.11f, 0.5f), DarkWood);
		}
		FShipFrame P;
		P.O = FVector3f(X + 0.35f, 0.f, Z + 0.45f);
		B.Box(Wood, P, FVector3f(0.12f, 0.12f, 0.45f), DarkWood);
	}

	void FFitBuild::Steering()
	{
		const float Xs = L.XStern(L.DeckAt(-L.HalfLength + 1.f));
		if (D.Type == EShipType::Sloop || D.Type == EShipType::Brigantine)
		{
			// A long tiller from the rudder head forward over the deck.
			const float X0 = Xs + L.FrameThickness + 0.3f;
			const float Z0 = DeckZ(X0) + 0.65f;
			const float Length = D.Length * 0.16f;
			B.Tube(Wood, FVector3f(X0, 0.f, Z0), FVector3f(X0 + Length, 0.f, Z0 + 0.12f), 0.08f, 0.05f, 8, Oak, true);
			B.Tube(Wood, FVector3f(X0, 0.f, DeckZ(X0) - 0.1f), FVector3f(X0, 0.f, Z0 + 0.1f), 0.17f, 0.17f, 8, DarkWood, true); // rudder head
			return;
		}
		if (D.Type == EShipType::Galleon)
		{
			// Whipstaff through the quarterdeck.
			const float X = -L.HalfLength * 0.55f;
			const float Z = DeckZ(X);
			B.Tube(Wood, FVector3f(X, 0.f, Z - 0.3f), FVector3f(X + 0.25f, 0.f, Z + 1.4f), 0.06f, 0.045f, 8, Oak, true);
			return;
		}
		// The wheel on the quarterdeck, ahead of the mizzen; its binnacle before it.
		const float X = Masts.Num() >= 3 ? Masts[2].Foot.X + 1.8f : -L.HalfLength * 0.5f;
		const float Z = DeckZ(X);
		const float Radius = 0.72f;
		const FVector3f Center(X, 0.f, Z + 1.15f);
		TArray<FVector3f> Rim;
		TArray<float> Radii;
		for (int32 I = 0; I <= 32; ++I)
		{
			const float A = 2.f * UE_PI * I / 32;
			Rim.Add(Center + FVector3f(0.f, FMath::Cos(A), FMath::Sin(A)) * Radius);
			Radii.Add(0.035f);
		}
		B.Tube(Wood, Rim, Radii, 6, Oak);
		for (int32 I = 0; I < 10; ++I)
		{
			const float A = 2.f * UE_PI * I / 10;
			const FVector3f Out = FVector3f(0.f, FMath::Cos(A), FMath::Sin(A));
			B.Tube(Wood, Center + Out * 0.1f, Center + Out * (Radius + 0.2f), 0.025f, 0.02f, 6, Oak, true);
		}
		B.Lathe(Wood, Center - FVector3f(0.12f, 0.f, 0.f), FVector3f(1.f, 0.f, 0.f), { FVector2f(0.f, 0.f), FVector2f(0.f, 0.13f), FVector2f(0.24f, 0.13f), FVector2f(0.24f, 0.f) }, 12, Brass);
		B.Tube(Wood, Center - FVector3f(0.4f, 0.f, 0.f), Center + FVector3f(0.4f, 0.f, 0.f), 0.05f, 0.05f, 8, DarkWood, true);
		for (float Side : { -1.f, 1.f })
		{
			FShipFrame F;
			F.O = FVector3f(X + Side * 0.4f, 0.f, Z + 0.62f);
			B.Box(Wood, F, FVector3f(0.07f, 0.1f, 0.62f), DarkWood);
		}
		// Binnacle.
		FShipFrame Bn;
		Bn.O = FVector3f(X + 1.1f, 0.f, Z + 0.55f);
		B.Box(Wood, Bn, FVector3f(0.28f, 0.32f, 0.55f), DarkWood);
		B.Lathe(Gloss, FVector3f(X + 1.1f, 0.f, Z + 1.1f), FVector3f::UpVector, { FVector2f(0.f, 0.2f), FVector2f(0.12f, 0.17f), FVector2f(0.2f, 0.f) }, 12, FLinearColor(0.03f, 0.03f, 0.03f, 1.f));
	}

	void FFitBuild::MastFeet()
	{
		for (int32 I = 0; I < Masts.Num(); ++I)
		{
			const FShipMastInfo& M = Masts[I];
			const float X = M.Foot.X, Z = M.Foot.Z;
			// Bitts before the mast.
			for (float Side : { -1.f, 1.f })
			{
				FShipFrame F;
				F.O = FVector3f(X + M.Radius + 0.45f, Side * (M.Radius + 0.3f), Z + 0.55f);
				B.Box(Wood, F, FVector3f(0.1f, 0.1f, 0.55f), DarkWood);
			}
			FShipFrame Cross;
			Cross.O = FVector3f(X + M.Radius + 0.45f, 0.f, Z + 0.85f);
			B.Box(Wood, Cross, FVector3f(0.08f, M.Radius + 0.5f, 0.08f), DarkWood);
			// Fife rail abaft it, full of pins.
			const float Half = M.Radius + 0.55f;
			for (float Side : { -1.f, 1.f })
			{
				FShipFrame Post;
				Post.O = FVector3f(X - Half, Side * Half, Z + 0.42f);
				B.Box(Wood, Post, FVector3f(0.06f, 0.06f, 0.42f), DarkWood);
			}
			TArray<FVector3f> Rail = { FVector3f(X - Half, -Half - 0.1f, Z + 0.8f), FVector3f(X - Half, Half + 0.1f, Z + 0.8f) };
			B.Beam(Wood, Rail, FVector3f::UpVector, 0.14f, 0.07f, Oak * 0.8f);
			Pins(B, FVector3f(X - Half, -Half + 0.05f, Z + 0.88f), FVector3f(X - Half, Half - 0.05f, Z + 0.88f), 0.2f);
			// Pin rails along the bulwarks abreast the shrouds.
			for (float Side : { -1.f, 1.f })
			{
				const float X0 = X + 0.4f, X1 = X - 2.6f;
				const float H = L.RailAt(X) - 0.38f;
				const FVector3f A = L.SidePoint(X0, H, Side) - FVector3f(0.f, Side * (L.FrameThickness + 0.08f), 0.f);
				const FVector3f Bp = L.SidePoint(X1, H, Side) - FVector3f(0.f, Side * (L.FrameThickness + 0.08f), 0.f);
				TArray<FVector3f> Path = { A, Bp };
				B.Beam(Wood, Path, FVector3f::UpVector, 0.14f, 0.07f, Oak * 0.8f);
				Pins(B, A + FVector3f(0.f, 0.f, 0.07f), Bp + FVector3f(0.f, 0.f, 0.07f), 0.22f);
			}
		}
	}

	void FFitBuild::Lanterns()
	{
		const float Top = L.RailAt(-L.HalfLength);
		const FVector3f Taff(L.XStern(Top) + L.FrameThickness * 0.4f, 0.f, L.Z(Top) + 0.11f);
		const float Half = L.PointSH(-1.f, Top).Y;
		const bool bBig = D.Type == EShipType::Frigate || D.Type == EShipType::Galleon;
		const float S = D.Type == EShipType::Galleon ? 1.6f : (bBig ? 1.2f : 0.9f);
		TArray<float> Ys = { 0.f };
		if (bBig)
		{
			Ys.Add(-Half * 0.75f);
			Ys.Add(Half * 0.75f);
		}
		for (float Y : Ys)
		{
			const FVector3f Foot = Taff + FVector3f(0.f, Y, 0.f);
			const FVector3f Base = Foot + FVector3f(-0.35f * S, 0.f, 0.55f * S);
			// Iron bracket curving out over the stern.
			const FVector3f Arc[] = { Foot, Foot + FVector3f(-0.05f, 0.f, 0.25f * S), Foot + FVector3f(-0.2f * S, 0.f, 0.42f * S), Base };
			const float R[] = { 0.03f, 0.028f, 0.026f, 0.025f };
			B.Tube(Metal, Arc, R, 6, FBuilder::IronColor);
			const FLinearColor Frame = D.Type == EShipType::Galleon ? Gilt : FLinearColor(0.04f, 0.035f, 0.03f, 0.f);
			const FVector2f Body[] = { { 0.f, 0.f }, { 0.f, 0.1f * S }, { 0.06f * S, 0.15f * S }, { 0.1f * S, 0.16f * S } };
			const FVector2f Glass[] = { { 0.1f * S, 0.16f * S }, { 0.32f * S, 0.19f * S }, { 0.5f * S, 0.16f * S } };
			const FVector2f Cap[] = { { 0.5f * S, 0.16f * S }, { 0.53f * S, 0.18f * S }, { 0.6f * S, 0.12f * S }, { 0.72f * S, 0.05f * S }, { 0.82f * S, 0.03f * S }, { 0.86f * S, 0.f } };
			B.Lathe(Metal, Base, FVector3f::UpVector, Body, 6, Frame);
			B.Lathe(Gloss, Base, FVector3f::UpVector, Glass, 6, FLinearColor(0.16f, 0.12f, 0.06f, 1.f));
			B.Lathe(Metal, Base, FVector3f::UpVector, Cap, 6, Frame);
			for (int32 I = 0; I < 6; ++I)
			{
				const float A = 2.f * UE_PI * (I + 0.5f) / 6;
				const FVector3f Out(FMath::Cos(A), FMath::Sin(A), 0.f);
				B.Tube(Metal, Base + Out * 0.17f * S + FVector3f(0.f, 0.f, 0.1f * S), Base + Out * 0.17f * S + FVector3f(0.f, 0.f, 0.5f * S), 0.012f * S, 0.012f * S, 4, Frame);
			}
		}
	}

	void FFitBuild::Anchors()
	{
		const float Top = L.RailAt(L.HalfLength * 0.85f);
		const float Shank = FMath::Clamp(D.Length * 0.1f, 1.6f, 4.f);
		for (float Side : { -1.f, 1.f })
		{
			// Cathead: a heavy timber out over the bow.
			const float X = L.HalfLength * 0.82f;
			FVector3f N;
			const FVector3f Root = L.SidePoint(X, Top - 0.1f, Side, &N) - FVector3f(0.f, Side * L.FrameThickness, 0.f);
			const FVector3f Dir = FVector3f(0.55f, Side * 1.f, 0.12f).GetSafeNormal();
			const FVector3f End = Root + Dir * (L.FrameThickness + 1.4f);
			TArray<FVector3f> Path = { Root, End };
			B.Beam(Wood, Path, FVector3f::UpVector, 0.3f, 0.3f, FLinearColor(D.WaleColor.R * 1.5f, D.WaleColor.G * 1.5f, D.WaleColor.B * 1.5f, 1.f));
			// The anchor hanging from it, crown down, against the bow.
			const FVector3f Ring = End - FVector3f(0.f, 0.f, 0.45f);
			B.Line(End - FVector3f(0.f, 0.f, 0.1f), Ring, 0.035f, 0.f, FBuilder::TarredRopeColor);
			const FVector3f Crown = Ring - FVector3f(0.f, 0.f, Shank);
			const float R = Shank * 0.035f;
			B.Tube(Metal, Ring - FVector3f(0.f, 0.f, 0.15f), Crown, R * 0.8f, R * 1.1f, 8, FBuilder::IronColor, true);
			TArray<FVector3f> RingPoints;
			TArray<float> RingRadii;
			for (int32 I = 0; I <= 16; ++I)
			{
				const float A = 2.f * UE_PI * I / 16;
				RingPoints.Add(Ring + FVector3f(FMath::Cos(A) * 0.18f, 0.f, FMath::Sin(A) * 0.18f) * (Shank / 3.f));
				RingRadii.Add(R * 0.4f);
			}
			B.Tube(Metal, RingPoints, RingRadii, 6, FBuilder::IronColor);
			// Wooden stock across, at the top.
			FShipFrame Stock;
			Stock.O = Ring - FVector3f(0.f, 0.f, 0.28f);
			Stock.X = FVector3f(0.f, Side, 0.f);
			Stock.Y = FVector3f(1.f, 0.f, 0.f);
			Stock.Z = FVector3f::UpVector;
			B.Box(Wood, Stock, FVector3f(Shank * 0.42f, R * 1.6f, R * 1.6f), DarkWood);
			// Arms curving up to the flukes, fore and aft.
			for (float Arm : { -1.f, 1.f })
			{
				TArray<FVector3f> Points;
				TArray<float> Radii;
				for (int32 I = 0; I <= 8; ++I)
				{
					const float A = FMath::DegreesToRadians(90.f * I / 8);
					Points.Add(Crown + FVector3f(Arm * FMath::Sin(A) * Shank * 0.36f, 0.f, (1.f - FMath::Cos(A)) * Shank * 0.36f));
					Radii.Add(R * FMath::Lerp(1.1f, 0.7f, I / 8.f));
				}
				B.Tube(Metal, Points, Radii, 8, FBuilder::IronColor, true);
				FShipFrame Fluke;
				Fluke.O = Points.Last() + FVector3f(0.f, 0.f, -Shank * 0.08f) + FVector3f(-Arm * 0.03f, 0.f, 0.f);
				Fluke.X = FVector3f(0.f, 0.f, 1.f);
				Fluke.Y = FVector3f(0.f, 1.f, 0.f);
				Fluke.Z = FVector3f(-1.f, 0.f, 0.f);
				B.Box(Metal, Fluke, FVector3f(Shank * 0.1f, Shank * 0.08f, R * 0.4f), FBuilder::IronColor);
			}
			// Hawse holes.
			for (int32 I = 0; I < 2; ++I)
			{
				const float H = L.MainDeck(L.HalfLength) - 0.35f;
				const float Xh = L.XBow(H) - 0.7f - I * 0.55f;
				FVector3f Nh;
				const FVector3f P = L.SidePoint(Xh, H, Side, &Nh) + Nh * (D.PlankThickness + 0.01f);
				B.Lathe(Wood, P, -Nh, { FVector2f(0.f, 0.f), FVector2f(0.f, 0.13f) }, 12, Darkness);
				B.Lathe(Metal, P - Nh * 0.005f, -Nh, { FVector2f(-0.03f, 0.13f), FVector2f(-0.03f, 0.18f), FVector2f(0.f, 0.18f) }, 12, FBuilder::IronColor);
			}
		}
	}

	void FFitBuild::Bell()
	{
		// Belfry at the break of the forecastle (or before the foremast).
		float X = Masts.Num() ? Masts[0].Foot.X + 1.6f : L.HalfLength * 0.5f;
		for (const FShipDeckLevel& Level : D.RaisedDecks)
		{
			if (Level.To >= 0.97f)
			{
				X = -L.HalfLength + Level.From * D.Length + 0.4f;
			}
		}
		const float Z = DeckZ(X);
		for (float Side : { -1.f, 1.f })
		{
			FShipFrame F;
			F.O = FVector3f(X, Side * 0.32f, Z + 0.55f);
			B.Box(Wood, F, FVector3f(0.06f, 0.06f, 0.55f), DarkWood);
		}
		FShipFrame Roof;
		Roof.O = FVector3f(X, 0.f, Z + 1.14f);
		B.Box(Wood, Roof, FVector3f(0.14f, 0.42f, 0.05f), DarkWood);
		const FVector2f Profile[] = { { 0.f, 0.f }, { 0.f, 0.17f }, { 0.04f, 0.16f }, { 0.12f, 0.12f }, { 0.25f, 0.1f }, { 0.3f, 0.06f }, { 0.32f, 0.f } };
		B.Lathe(Metal, FVector3f(X, 0.f, Z + 0.72f), FVector3f::UpVector, Profile, 14, Brass);
	}
}

void ShipFittings::Build(FBuilder& B, const FHullLines& L, const TArray<FShipGunPort>& Ports, const TArray<FShipMastInfo>& Masts,
	const FShipInterior& Interior)
{
	FFitBuild Fit{ B, L, L.D, Ports, Masts, Interior };
	const FShipDesign& D = L.D;
	Fit.Guns();
	Fit.Hatches();
	Fit.MastFeet();
	Fit.Steering();
	Fit.Lanterns();
	Fit.Anchors();
	if (D.Type == EShipType::Sloop || D.Type == EShipType::Brigantine)
	{
		Fit.Windlass(Masts.Num() ? Masts[0].Foot.X + 1.6f : L.HalfLength * 0.5f);
	}
	if (D.Type != EShipType::Sloop && Masts.Num() >= 2)
	{
		const float X = D.Type == EShipType::Brigantine ? Masts[1].Foot.X - 2.2f : FMath::Lerp(Masts[1].Foot.X, Masts.Last().Foot.X, 0.45f);
		Fit.Capstan(FVector3f(X, 0.f, Fit.DeckZ(X)));
	}
	if (D.Type == EShipType::Frigate || D.Type == EShipType::Galleon)
	{
		Fit.Bell();
	}
}

float ShipFittings::Cradle(FBuilder& B, const FHullLines& L, float BlockHeight)
{
	const FShipDesign& D = L.D;
	const float KeelBottom = L.Z(-D.KeelDepth);
	const float Ground = KeelBottom - BlockHeight;
	const FLinearColor Timber(0.2f, 0.17f, 0.13f, 1.f);
	// Keel blocks: crossed timbers stacked under the keel, a wedge on top.
	int32 Id = 0;
	for (float X = L.XStern(0.f) + 1.f; X < L.XBow(0.f) - 0.4f; X += 2.2f, ++Id)
	{
		const int32 Layers = FMath::Max(2, FMath::RoundToInt(BlockHeight / 0.3f));
		const float Layer = BlockHeight / Layers;
		for (int32 I = 0; I < Layers; ++I)
		{
			FShipFrame F;
			F.O = FVector3f(X, 0.f, Ground + Layer * (I + 0.5f));
			if (I % 2)
			{
				F.X = FVector3f::RightVector;
				F.Y = FVector3f(-1.f, 0.f, 0.f);
			}
			B.Box(Wood, F, FVector3f(0.75f, 0.18f, Layer * 0.5f - 0.004f), Jitter(Timber, 0.15f, Id, I));
		}
	}
	// Bilge shores: raking timbers from the ground to the turn of the bilge, on sleepers.
	for (float X = -L.HalfLength * 0.7f; X <= L.HalfLength * 0.6f; X += FMath::Max(3.f, D.Length * 0.12f))
	{
		for (float Side : { -1.f, 1.f })
		{
			const float H = L.Hmb * 0.55f;
			FVector3f N;
			const FVector3f Hull = L.SidePoint(X, H, Side, &N) + N * (D.PlankThickness + 0.05f);
			const FVector3f Foot(X, Side * (L.HalfBeam * 1.25f + BlockHeight), Ground + 0.12f);
			TArray<FVector3f> Path = { Foot, Hull };
			B.Beam(Wood, Path, FVector3f(1.f, 0.f, 0.f), 0.2f, 0.2f, Jitter(Timber, 0.15f, int32(X * 10.f), int32(Side)));
			FShipFrame Sleeper;
			Sleeper.O = Foot - FVector3f(0.f, 0.f, 0.06f);
			Sleeper.X = FVector3f::RightVector;
			Sleeper.Y = FVector3f(-1.f, 0.f, 0.f);
			B.Box(Wood, Sleeper, FVector3f(0.6f, 0.25f, 0.06f), Jitter(Timber, 0.15f, int32(X * 7.f), 3));
		}
	}
	return Ground;
}
