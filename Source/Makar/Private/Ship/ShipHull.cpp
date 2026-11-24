#include "ShipHull.h"

#include "ShipMeshBuilder.h"

using namespace ShipMesh;

// ---------------------------------------------------------------------------------------------------------------------
// Lines
// ---------------------------------------------------------------------------------------------------------------------

FHullLines::FHullLines(const FShipDesign& InDesign)
	: D(InDesign)
{
	HalfLength = D.Length * 0.5f;
	HalfBeam = D.Beam * 0.5f;
	Hmb = D.MaxBeamHeight * D.Depth;
	FrameThickness = FMath::Clamp(D.Beam * 0.032f, 0.18f, 0.4f);

	// Girth tables: for each station, the section parameter at uniform steps of girth (planks of even width).
	PTable.SetNumZeroed((Stations + 1) * (GirthSteps + 1));
	GirthTable.SetNumZeroed(Stations + 1);
	constexpr int32 Samples = 320;
	TArray<float> Lengths;
	Lengths.SetNumZeroed(Samples + 1);
	for (int32 K = 0; K <= Stations; ++K)
	{
		const float S = -1.f + 2.f * K / Stations;
		FVector3f Prev = PointSP(S, 0.f);
		for (int32 I = 1; I <= Samples; ++I)
		{
			const FVector3f P = PointSP(S, 2.f * I / Samples);
			Lengths[I] = Lengths[I - 1] + (P - Prev).Size();
			Prev = P;
		}
		const float Total = FMath::Max(Lengths[Samples], 1e-3f);
		GirthTable[K] = Total;
		int32 I = 0;
		for (int32 J = 0; J <= GirthSteps; ++J)
		{
			const float Target = Total * J / GirthSteps;
			while (I < Samples - 1 && Lengths[I + 1] < Target)
			{
				++I;
			}
			const float Frac = FMath::Clamp((Target - Lengths[I]) / FMath::Max(Lengths[I + 1] - Lengths[I], 1e-6f), 0.f, 1.f);
			PTable[K * (GirthSteps + 1) + J] = 2.f * (I + Frac) / Samples;
		}
	}
	NormalSign = 1.f;
	NormalSign = Normal(0.f, 0.6f).Y >= 0.f ? 1.f : -1.f;
}

float FHullLines::MainDeck(float X) const
{
	const float Xn = FMath::Clamp(X / HalfLength, -1.f, 1.f);
	return D.Depth + (Xn > 0.f ? D.SheerFore : D.SheerAft) * Xn * Xn;
}

float FHullLines::RaisedAt(float X) const
{
	float Raised = 0.f;
	for (const FShipDeckLevel& Level : D.RaisedDecks)
	{
		const float A = -HalfLength + Level.From * D.Length, B = -HalfLength + Level.To * D.Length;
		if (X >= A && X <= B)
		{
			Raised += Level.Height;
		}
	}
	return Raised;
}

float FHullLines::RailAt(float X) const
{
	// The rail climbs to each raised deck before its break (in the waist), so the raised deck never stands above it.
	float Raised = 0.f;
	for (const FShipDeckLevel& Level : D.RaisedDecks)
	{
		const float A = -HalfLength + Level.From * D.Length, B = -HalfLength + Level.To * D.Length;
		const float Rise = Level.From <= 0.01f ? 1.f : FMath::SmoothStep(A - 1.4f, A, X);
		const float Fall = Level.To >= 0.99f ? 1.f : 1.f - FMath::SmoothStep(B, B + 1.4f, X);
		Raised += Level.Height * Rise * Fall;
	}
	return MainDeck(X) + Raised + D.Bulwark;
}

float FHullLines::TopAt(float S) const
{
	return RailAt(S * HalfLength);
}

float FHullLines::XBow(float H) const
{
	// A quarter ellipse: square to the keel at the forefoot, upright at the stem head.
	const float Q = FMath::Clamp(H / TopAt(1.f), 0.f, 1.f);
	return HalfLength - D.StemRake + D.StemRake * FMath::Sqrt(FMath::Max(1.f - FMath::Square(1.f - Q), 0.f));
}

float FHullLines::XStern(float H) const
{
	return -HalfLength + D.SternRake * (1.f - FMath::Clamp(H / TopAt(-1.f), 0.f, 1.f));
}

float FHullLines::Longitudinal(float S, float H) const
{
	if (S >= 0.f)
	{
		const float A = D.BowFullness;
		return FMath::Pow(FMath::Max(1.f - FMath::Pow(FMath::Min(S, 1.f), A), 0.f), 1.f / A);
	}
	const float U = FMath::Min(-S, 1.f);
	const float B = D.SternFullness;
	const float Run = FMath::Pow(FMath::Max(1.f - FMath::Pow(U, B), 0.f), 1.f / B);
	// Higher up the stern ends square at the transom: the run blends into it over the tuck.
	const float W = FMath::SmoothStep(D.TransomBottom - 0.2f, D.TransomBottom + 0.04f, H / TopAt(-1.f));
	const float Upper = 1.f - (1.f - D.TransomWidth) * FMath::Pow(U, 2.5f);
	return FMath::Lerp(Run, FMath::Max(Run, Upper), W);
}

float FHullLines::Shape(float H) const
{
	if (H < Hmb)
	{
		const float N = D.SectionFullness;
		const float R = FMath::Clamp((Hmb - H) / Hmb, 0.f, 1.f);
		return FMath::Pow(FMath::Max(1.f - FMath::Pow(R, N), 0.f), 1.f / N);
	}
	const float R = (H - Hmb) / FMath::Max(D.Depth - Hmb, 0.3f);
	return FMath::Max(1.f - D.Tumblehome * R * R, 0.35f);
}

float FHullLines::HeightSP(float S, float P) const
{
	if (P <= 1.f)
	{
		const float C = FMath::Cos(P * UE_HALF_PI);
		return Hmb * (1.f - FMath::Pow(FMath::Max(C, 0.f), 2.f / D.SectionFullness));
	}
	return FMath::Lerp(Hmb, TopAt(S), FMath::Min(P - 1.f, 1.f));
}

FVector3f FHullLines::PointSP(float S, float P) const
{
	const float H = HeightSP(S, P);
	const float ShapeY = P <= 1.f ? FMath::Pow(FMath::Max(FMath::Sin(P * UE_HALF_PI), 0.f), 2.f / D.SectionFullness) : Shape(H);
	const float X = S >= 0.f ? S * XBow(H) : -S * XStern(H);
	const float Y = 0.08f + (HalfBeam - 0.08f) * Longitudinal(S, H) * ShapeY;
	return FVector3f(X, Y, Z(H));
}

FVector3f FHullLines::PointSH(float S, float H) const
{
	const float X = S >= 0.f ? S * XBow(H) : -S * XStern(H);
	const float Y = 0.08f + (HalfBeam - 0.08f) * Longitudinal(S, H) * Shape(H);
	return FVector3f(X, Y, Z(H));
}

float FHullLines::StationOf(float X, float H) const
{
	return X >= 0.f ? X / XBow(H) : -X / XStern(H);
}

float FHullLines::HalfBreadth(float X, float H) const
{
	const float S = StationOf(X, H);
	if (FMath::Abs(S) > 1.f)
	{
		return 0.f;
	}
	return 0.08f + (HalfBeam - 0.08f) * Longitudinal(S, H) * Shape(H);
}

FVector3f FHullLines::SidePoint(float X, float H, float Side, FVector3f* OutNormal) const
{
	const float S = FMath::Clamp(StationOf(X, H), -1.f, 1.f);
	FVector3f P = PointSH(S, H);
	if (OutNormal)
	{
		const float E = 0.002f;
		const FVector3f DS = PointSH(FMath::Min(S + E, 1.f), H) - PointSH(FMath::Max(S - E, -1.f), H);
		const FVector3f DH = PointSH(S, H + 0.01f) - PointSH(S, H - 0.01f);
		FVector3f N = FVector3f::CrossProduct(DS, DH).GetSafeNormal();
		if (N.Y < 0.f)
		{
			N = -N;
		}
		N.Y *= Side;
		*OutNormal = N;
	}
	P.Y *= Side;
	return P;
}

FVector3f FHullLines::Surface(float S, float T) const
{
	S = FMath::Clamp(S, -1.f, 1.f);
	T = FMath::Clamp(T, 0.f, 1.f);
	const float FS = (S + 1.f) * 0.5f * Stations;
	const int32 K = FMath::Clamp(FMath::FloorToInt(FS), 0, Stations - 1);
	const float A = FS - K;
	const float FT = T * GirthSteps;
	const int32 J = FMath::Clamp(FMath::FloorToInt(FT), 0, GirthSteps - 1);
	const float Bt = FT - J;
	auto At = [this](int32 Kk, int32 Jj) { return PTable[Kk * (GirthSteps + 1) + Jj]; };
	const float P = FMath::Lerp(FMath::Lerp(At(K, J), At(K, J + 1), Bt), FMath::Lerp(At(K + 1, J), At(K + 1, J + 1), Bt), A);
	return PointSP(S, P);
}

float FHullLines::HeightAt(float S, float T) const
{
	return Surface(S, T).Z + D.Draft;
}

float FHullLines::GirthAt(float S, float H) const
{
	float Lo = 0.f, Hi = 1.f;
	for (int32 I = 0; I < 24; ++I)
	{
		const float Mid = 0.5f * (Lo + Hi);
		(HeightAt(S, Mid) < H ? Lo : Hi) = Mid;
	}
	return 0.5f * (Lo + Hi);
}

float FHullLines::Girth(float S) const
{
	const float FS = (FMath::Clamp(S, -1.f, 1.f) + 1.f) * 0.5f * Stations;
	const int32 K = FMath::Clamp(FMath::FloorToInt(FS), 0, Stations - 1);
	return FMath::Lerp(GirthTable[K], GirthTable[K + 1], FS - K);
}

FVector3f FHullLines::Normal(float S, float T) const
{
	const float ES = 0.0015f, ET = 0.004f;
	const FVector3f PS = Surface(FMath::Min(S + ES, 1.f), T) - Surface(FMath::Max(S - ES, -1.f), T);
	const FVector3f PT = Surface(S, FMath::Min(T + ET, 1.f)) - Surface(S, FMath::Max(T - ET, 0.f));
	const FVector3f N = FVector3f::CrossProduct(PS, PT).GetSafeNormal();
	return N.IsNearlyZero() ? FVector3f(0.f, 1.f, 0.f) : N * NormalSign;
}

// ---------------------------------------------------------------------------------------------------------------------
// Hull
// ---------------------------------------------------------------------------------------------------------------------

namespace
{
	/** An opening in the planking, in (station, girth). */
	struct FCut
	{
		float S0, S1, T0, T1;
		int32 Port;
	};

	/** [A, B] minus the cuts that span station S. */
	TArray<FVector2f, TInlineAllocator<3>> Clip(float A, float B, float S, const TArray<FCut>& Cuts)
	{
		TArray<FVector2f, TInlineAllocator<3>> Parts;
		Parts.Add(FVector2f(A, B));
		for (const FCut& Cut : Cuts)
		{
			if (S <= Cut.S0 || S >= Cut.S1)
			{
				continue;
			}
			TArray<FVector2f, TInlineAllocator<3>> Next;
			for (const FVector2f& Part : Parts)
			{
				if (Cut.T1 <= Part.X || Cut.T0 >= Part.Y)
				{
					Next.Add(Part);
					continue;
				}
				if (Cut.T0 > Part.X + 1e-4f)
				{
					Next.Add(FVector2f(Part.X, Cut.T0));
				}
				if (Cut.T1 < Part.Y - 1e-4f)
				{
					Next.Add(FVector2f(Cut.T1, Part.Y));
				}
			}
			Parts = Next;
		}
		return Parts;
	}

	FVector3f Mirror(FVector3f V, float Side)
	{
		V.Y *= Side;
		return V;
	}

	/** Stations from stern to stem, closer together at the ends where the hull turns fast. */
	TArray<float> BaseStations(int32 Count)
	{
		TArray<float> S;
		for (int32 I = 0; I <= Count; ++I)
		{
			const float U = -1.f + 2.f * I / Count;
			S.Add(FMath::Sin(U * UE_HALF_PI) * 0.65f + U * 0.35f);
		}
		return S;
	}

	void SortUnique(TArray<float>& Values)
	{
		Values.Sort();
		TArray<float> Out;
		for (float V : Values)
		{
			if (Out.Num() == 0 || V - Out.Last() > 1e-5f)
			{
				Out.Add(V);
			}
		}
		Values = MoveTemp(Out);
	}

	const FLinearColor Caulk(0.012f, 0.011f, 0.01f);
	const FLinearColor HoldDarkness(0.004f, 0.004f, 0.004f);

	struct FHullBuild
	{
		FBuilder& B;
		const FHullLines& L;
		const FShipDesign& D;
		TArray<FShipGunPort>& Ports;
		TArray<FCut> Cuts;
		int32 Strakes = 0;
		TArray<int32> WaleStrakes;
		int32 BandFrom = 1 << 20, BandTo = -1;

		float PlankThicknessOf(int32 Strake) const
		{
			return WaleStrakes.Contains(Strake) ? D.PlankThickness * 2.6f : D.PlankThickness;
		}

		FLinearColor PlankColor(int32 Strake, int32 Plank, float Side, bool bBottom) const
		{
			FLinearColor Base = WaleStrakes.Contains(Strake) ? D.WaleColor
				: (Strake >= BandFrom && Strake <= BandTo) ? D.BandColor : D.TopsideColor;
			if (bBottom)
			{
				Base = D.BottomColor; // the boot top: a little above the waterline
			}
			FLinearColor C = Jitter(Base, 0.14f, Plank * 31 + int32(Side), Strake, D.Seed);
			C.A = 1.f;
			return C;
		}

		void PlanPorts();
		void Planking(float Side);
		void Bulwarks(float Side);
		void PortLinings(float Side);
		void Backbone();
		void Head();
		void Transom();
		void Rails(float Side);
		void Decks();
	};

	void FHullBuild::PlanPorts()
	{
		auto AddRow = [&](int32 Count, float XFrom, float XTo, bool bLower)
		{
			if (Count <= 0)
			{
				return;
			}
			const float Width = 0.3f + D.GunLength * 0.07f;   // half-width
			const float Height = 0.42f + D.GunLength * 0.07f;
			for (int32 I = 0; I < Count; ++I)
			{
				const float X = Count == 1 ? 0.5f * (XFrom + XTo) : FMath::Lerp(XFrom, XTo, float(I) / (Count - 1));
				FShipGunPort Port;
				Port.X = X;
				Port.bLower = bLower;
				Port.HalfWidth = Width;
				Port.DeckH = bLower ? L.MainDeck(X) - 2.25f : L.MainDeck(X);
				Port.H0 = Port.DeckH + (D.Type == EShipType::Sloop ? 0.25f : 0.4f);
				Port.H1 = FMath::Min(Port.H0 + Height, L.RailAt(X) - 0.14f);
				if (Port.H1 - Port.H0 < 0.3f || L.Z(Port.H0) < 0.45f)
				{
					continue;
				}
				Ports.Add(Port);
			}
		};
		const float HL = L.HalfLength;
		const float Aft = D.Type == EShipType::Sloop ? -0.58f : -0.74f;
		const float Fore = D.Type == EShipType::Sloop ? 0.42f : 0.6f;
		AddRow(D.Guns, HL * Aft, HL * Fore, false);
		AddRow(D.LowerGuns, HL * -0.66f, HL * 0.52f, true);
		for (int32 I = 0; I < Ports.Num(); ++I)
		{
			const FShipGunPort& Port = Ports[I];
			const float Hc = 0.5f * (Port.H0 + Port.H1);
			FCut Cut;
			Cut.S0 = L.StationOf(Port.X - Port.HalfWidth, Hc);
			Cut.S1 = L.StationOf(Port.X + Port.HalfWidth, Hc);
			const float Sc = L.StationOf(Port.X, Hc);
			Cut.T0 = L.GirthAt(Sc, Port.H0);
			Cut.T1 = L.GirthAt(Sc, Port.H1);
			Cut.Port = I;
			Cuts.Add(Cut);
		}
	}

	void FHullBuild::Planking(float Side)
	{
		const TArray<float> Base = BaseStations(200);
		const float PlankS = D.PlankLength / L.HalfLength;
		for (int32 Strake = 0; Strake < Strakes; ++Strake)
		{
			const float TA = float(Strake) / Strakes, TB = float(Strake + 1) / Strakes;
			const float Thick = PlankThicknessOf(Strake);
			// Butt joints staggered from strake to strake (never two neighbours on the same frame).
			const float Offset = FMath::Frac(Strake * 0.37f + Hash(Strake, D.Seed) * 0.13f) * PlankS;
			TArray<float> Ss = Base;
			TArray<float> Butts;
			for (float S = -1.f + Offset; S < 1.f; S += PlankS)
			{
				if (S > -1.f)
				{
					Butts.Add(S);
				}
			}
			Ss.Append(Butts);
			for (const FCut& Cut : Cuts)
			{
				if (Cut.T1 > TA && Cut.T0 < TB)
				{
					Ss.Add(Cut.S0);
					Ss.Add(Cut.S1);
				}
			}
			SortUnique(Ss);
			auto IsBoundary = [&](float S)
			{
				if (S <= -1.f + 1e-5f || S >= 1.f - 1e-5f)
				{
					return true;
				}
				for (float Butt : Butts)
				{
					if (FMath::Abs(Butt - S) < 1e-5f)
					{
						return true;
					}
				}
				for (const FCut& Cut : Cuts)
				{
					if (FMath::Abs(Cut.S0 - S) < 1e-5f || FMath::Abs(Cut.S1 - S) < 1e-5f)
					{
						return true;
					}
				}
				return false;
			};
			for (int32 K = 0; K + 1 < Ss.Num(); ++K)
			{
				const float S0 = Ss[K], S1 = Ss[K + 1], Sm = 0.5f * (S0 + S1);
				const int32 Plank = FMath::FloorToInt((Sm + 1.f - Offset) / PlankS) + 1;
				// Split at the boot top, so the bottom's paint ends on a clean line rather than in the strakes' steps.
				const float Boot = L.GirthAt(Sm, D.Draft + 0.15f);
				const float Boot0 = L.GirthAt(S0, D.Draft + 0.15f), Boot1 = L.GirthAt(S1, D.Draft + 0.15f);
				TArray<FVector2f, TInlineAllocator<4>> Parts;
				for (const FVector2f& Part : Clip(TA, TB, Sm, Cuts))
				{
					if (Boot > Part.X + 1e-4f && Boot < Part.Y - 1e-4f)
					{
						Parts.Add(FVector2f(Part.X, Boot));
						Parts.Add(FVector2f(Boot, Part.Y));
					}
					else
					{
						Parts.Add(Part);
					}
				}
				for (const FVector2f& Part : Parts)
				{
					const bool bBottom = 0.5f * (Part.X + Part.Y) < Boot;
					// Seams: open and pitched on the topsides; filled and painted over on the bottom.
					const float Seam = bBottom ? 0.0012f : 0.004f;
					const float Gap0 = Seam / L.Girth(S0), Gap1 = Seam / L.Girth(S1);
					// The boot top's girth at each end of the segment (not its middle's: the paint line would zigzag).
					auto AtEnd = [&](float T, int32 E) { return T == Boot ? (E == 0 ? Boot0 : Boot1) : T; };
					const float Ts[2][2] = { { AtEnd(Part.X, 0) + (Part.X == TA ? Gap0 : 0.f), AtEnd(Part.Y, 0) - (Part.Y == TB ? Gap0 : 0.f) },
						{ AtEnd(Part.X, 1) + (Part.X == TA ? Gap1 : 0.f), AtEnd(Part.Y, 1) - (Part.Y == TB ? Gap1 : 0.f) } };
					const float Sv[2] = { S0, S1 };
					FVector3f In[2][2], Out[2][2], N[2][2];
					FLinearColor C[2][2];
					for (int32 E = 0; E < 2; ++E)
					{
						for (int32 F = 0; F < 2; ++F)
						{
							const FVector3f P = L.Surface(Sv[E], Ts[E][F]);
							const FVector3f Nn = L.Normal(Sv[E], Ts[E][F]);
							In[E][F] = Mirror(P, Side);
							Out[E][F] = Mirror(P + Nn * Thick, Side);
							N[E][F] = Mirror(Nn, Side);
							C[E][F] = PlankColor(Strake, Plank, Side, bBottom);
						}
					}
					// UV: U along the grain (m, a random start per plank), V across.
					const float U0 = In[0][0].X + Plank * 3.7f + Strake * 1.3f, U1 = In[1][0].X + Plank * 3.7f + Strake * 1.3f;
					const float W0 = (Ts[0][1] - Ts[0][0]) * L.Girth(S0), W1 = (Ts[1][1] - Ts[1][0]) * L.Girth(S1);
					// Caulking under the seams (the whole strake width): pitch, but paint over it below the waterline.
					{
						const FLinearColor SeamColor = C[0][0] * (bBottom ? 0.7f : 0.35f);
						const FVector3f A = Mirror(L.Surface(S0, AtEnd(Part.X, 0)), Side), Bv = Mirror(L.Surface(S1, AtEnd(Part.X, 1)), Side);
						const FVector3f Cc = Mirror(L.Surface(S1, AtEnd(Part.Y, 1)), Side), Dd = Mirror(L.Surface(S0, AtEnd(Part.Y, 0)), Side);
						const int32 I0 = B.Vertex(A, N[0][0], FVector2f(0.f, 0.f), SeamColor);
						const int32 I1 = B.Vertex(Bv, N[1][0], FVector2f(1.f, 0.f), SeamColor);
						const int32 I2 = B.Vertex(Cc, N[1][1], FVector2f(1.f, 1.f), SeamColor);
						const int32 I3 = B.Vertex(Dd, N[0][1], FVector2f(0.f, 1.f), SeamColor);
						B.Quad(Wood, I0, I1, I2, I3);
					}
					// Face.
					{
						const int32 I0 = B.Vertex(Out[0][0], N[0][0], FVector2f(U0, 0.f), C[0][0]);
						const int32 I1 = B.Vertex(Out[1][0], N[1][0], FVector2f(U1, 0.f), C[1][0]);
						const int32 I2 = B.Vertex(Out[1][1], N[1][1], FVector2f(U1, W1), C[1][1]);
						const int32 I3 = B.Vertex(Out[0][1], N[0][1], FVector2f(U0, W0), C[0][1]);
						B.Quad(Wood, I0, I1, I2, I3);
					}
					// Edges (seen at the seams and in the ports).
					for (int32 F = 0; F < 2; ++F)
					{
						const FVector3f Across = ((Out[0][1] - Out[0][0]) * (F ? 1.f : -1.f)).GetSafeNormal();
						const FLinearColor Ce = C[0][F] * 0.8f;
						const int32 I0 = B.Vertex(In[0][F], Across, FVector2f(U0, 0.f), Ce);
						const int32 I1 = B.Vertex(In[1][F], Across, FVector2f(U1, 0.f), Ce);
						const int32 I2 = B.Vertex(Out[1][F], Across, FVector2f(U1, Thick), Ce);
						const int32 I3 = B.Vertex(Out[0][F], Across, FVector2f(U0, Thick), Ce);
						B.Quad(Wood, I0, I1, I2, I3);
					}
					// Ends at butts and port edges.
					for (int32 E = 0; E < 2; ++E)
					{
						if (!IsBoundary(Sv[E]))
						{
							continue;
						}
						const FVector3f Along = ((Out[1][0] - Out[0][0]) * (E ? 1.f : -1.f)).GetSafeNormal();
						B.FlatQuad(Wood, In[E][0], In[E][1], Out[E][1], Out[E][0], C[E][0] * 0.7f, Along);
					}
				}
			}
		}
	}

	/** Girth of the top deck's edge on station S (iterated: the deck height depends on X, X on the height). */
	float DeckGirth(const FHullLines& L, float S)
	{
		float T = L.GirthAt(S, L.DeckAt(S * L.HalfLength));
		for (int32 I = 0; I < 2; ++I)
		{
			T = L.GirthAt(S, L.DeckAt(L.Surface(S, T).X));
		}
		return T;
	}

	void FHullBuild::Bulwarks(float Side)
	{
		// The inside of the bulwarks: the frames' inner face, ceiled with planks, from the deck to the rail.
		const FLinearColor Inside = (D.Type == EShipType::Frigate || D.Type == EShipType::Galleon)
			? FLinearColor(0.26f, 0.055f, 0.035f, 1.f) // red ochre, the fashion of the time
			: FLinearColor(D.TopsideColor.R * 1.6f, D.TopsideColor.G * 1.6f, D.TopsideColor.B * 1.6f, 1.f);
		TArray<float> Ss = BaseStations(200);
		for (const FCut& Cut : Cuts)
		{
			Ss.Add(Cut.S0);
			Ss.Add(Cut.S1);
		}
		SortUnique(Ss);
		const float FT = L.FrameThickness;
		for (int32 K = 0; K + 1 < Ss.Num(); ++K)
		{
			const float Sv[2] = { Ss[K], Ss[K + 1] };
			if (Sv[1] > 0.995f)
			{
				continue;
			}
			const float Sm = 0.5f * (Sv[0] + Sv[1]);
			const float Td = DeckGirth(L, Sm);
			for (const FVector2f& Part : Clip(Td, 1.f, Sm, Cuts))
			{
				constexpr int32 Rows = 3;
				for (int32 R = 0; R < Rows; ++R)
				{
					const float Ta = FMath::Lerp(Part.X, Part.Y, float(R) / Rows), Tb = FMath::Lerp(Part.X, Part.Y, float(R + 1) / Rows);
					int32 Idx[4];
					const float Corner[4][2] = { { Sv[0], Ta }, { Sv[1], Ta }, { Sv[1], Tb }, { Sv[0], Tb } };
					for (int32 C = 0; C < 4; ++C)
					{
						const FVector3f N = L.Normal(Corner[C][0], Corner[C][1]);
						const FVector3f P = L.Surface(Corner[C][0], Corner[C][1]) - N * FT;
						Idx[C] = B.Vertex(Mirror(P, Side), Mirror(-N, Side), FVector2f(P.X, P.Z), Jitter(Inside, 0.08f, K / 6, int32(Side)));
					}
					B.Quad(Wood, Idx[0], Idx[1], Idx[2], Idx[3]);
				}
			}
		}
	}

	void FHullBuild::PortLinings(float Side)
	{
		const float FT = L.FrameThickness;
		for (const FCut& Cut : Cuts)
		{
			const FShipGunPort& Port = Ports[Cut.Port];
			const FLinearColor Lining = (D.Type == EShipType::Frigate || D.Type == EShipType::Galleon)
				? FLinearColor(0.26f, 0.055f, 0.035f, 1.f) : D.TopsideColor * 1.4f;
			const float Corner[4][2] = { { Cut.S0, Cut.T0 }, { Cut.S1, Cut.T0 }, { Cut.S1, Cut.T1 }, { Cut.S0, Cut.T1 } };
			FVector3f Out[4], In[4];
			for (int32 C = 0; C < 4; ++C)
			{
				const FVector3f N = L.Normal(Corner[C][0], Corner[C][1]);
				const FVector3f P = L.Surface(Corner[C][0], Corner[C][1]);
				Out[C] = Mirror(P + N * D.PlankThickness, Side);
				In[C] = Mirror(P - N * FT, Side);
			}
			const FVector3f Center = (Out[0] + Out[1] + Out[2] + Out[3]) * 0.25f;
			for (int32 C = 0; C < 4; ++C)
			{
				const int32 Next = (C + 1) % 4;
				const FVector3f Mid = (Out[C] + Out[Next]) * 0.5f;
				B.FlatQuad(Wood, Out[C], Out[Next], In[Next], In[C], Lining, Center - Mid);
			}
			// Ports under a raised deck or on the lower gun deck open on the dark inside, not on the open deck.
			const bool bClosedBehind = Port.bLower || L.RaisedAt(Port.X) > 0.f;
			if (bClosedBehind)
			{
				const FVector3f Inward = Mirror(FVector3f(0.f, -1.f, 0.f), Side);
				FVector3f Back[4];
				for (int32 C = 0; C < 4; ++C)
				{
					Back[C] = In[C] + Inward * 0.9f;
				}
				B.FlatQuad(Wood, Back[0], Back[1], Back[2], Back[3], HoldDarkness, -Inward);
				for (int32 C = 0; C < 4; ++C)
				{
					const int32 Next = (C + 1) % 4;
					B.FlatQuad(Wood, In[C], In[Next], Back[Next], Back[C], HoldDarkness, Center - (In[C] + In[Next]) * 0.5f);
				}
			}
		}
	}

	void FHullBuild::Backbone()
	{
		const float KeelWidth = FMath::Clamp(D.Beam * 0.045f, 0.24f, 0.46f);
		const FLinearColor Bottom(D.BottomColor.R * 0.9f, D.BottomColor.G * 0.9f, D.BottomColor.B * 0.9f, 1.f);
		const FLinearColor Black(D.WaleColor.R, D.WaleColor.G, D.WaleColor.B, 1.f);
		// Keel.
		{
			const float Z = L.Z(-D.KeelDepth * 0.5f);
			TArray<FVector3f> Path = { FVector3f(L.XStern(0.f) - 0.12f, 0.f, Z), FVector3f(L.XBow(0.f) + 0.05f, 0.f, Z) };
			B.Beam(Wood, Path, FVector3f::UpVector, KeelWidth, D.KeelDepth + 0.04f, Bottom);
		}
		// Stem: the forefoot to the stem head, standing above the rail; split at the boot top for its paint.
		{
			const float Top = L.RailAt(L.HalfLength) + FMath::Clamp(D.Length * 0.03f, 0.4f, 1.f);
			const float Hw = D.Draft + 0.15f;
			auto StemPath = [&](float H0, float H1)
			{
				TArray<FVector3f> Path;
				for (int32 I = 0; I <= 16; ++I)
				{
					const float H = FMath::Lerp(H0, H1, float(I) / 16);
					const float Hc = FMath::Min(H, L.RailAt(L.HalfLength));
					Path.Add(FVector3f(L.XBow(FMath::Max(Hc, 0.f)) + 0.1f + (H - Hc) * 0.25f, 0.f, L.Z(H)));
				}
				return Path;
			};
			B.Beam(Wood, StemPath(-D.KeelDepth * 0.5f, Hw), FVector3f(-1.f, 0.f, 1.f), KeelWidth, 0.32f, Bottom);
			B.Beam(Wood, StemPath(Hw, Top), FVector3f(-1.f, 0.f, 1.f), KeelWidth, 0.32f, Black);
		}
		// Sternpost.
		{
			const float Top = L.RailAt(-L.HalfLength);
			const float Hw = D.Draft + 0.15f;
			auto PostPath = [&](float H0, float H1)
			{
				TArray<FVector3f> Path = { FVector3f(L.XStern(FMath::Max(H0, 0.f)) - 0.1f, 0.f, L.Z(H0)),
					FVector3f(L.XStern(H1) - 0.1f, 0.f, L.Z(H1)) };
				return Path;
			};
			B.Beam(Wood, PostPath(-D.KeelDepth, Hw), FVector3f(1.f, 0.f, 1.f), KeelWidth, 0.3f, Bottom);
			// Up to the counter: above it is the transom, its planks across.
			const float Counter = FMath::Max(D.TransomBottom * Top + 0.2f, Hw + 0.3f);
			B.Beam(Wood, PostPath(Hw, Counter), FVector3f(1.f, 0.f, 1.f), KeelWidth, 0.3f, Black);
		}
		Head();
		// Rudder: a blade hung on the sternpost, wide at the heel, its head through the counter.
		{
			const float H0 = -D.KeelDepth * 0.6f;
			const float H1 = FMath::Min(L.MainDeck(-L.HalfLength) + 0.2f, D.TransomBottom * L.RailAt(-L.HalfLength) + 0.5f);
			const FVector3f P0(L.XStern(0.f) - 0.25f, 0.f, L.Z(H0));
			const FVector3f P1(L.XStern(H1) - 0.25f, 0.f, L.Z(H1));
			const FVector3f Up = (P1 - P0).GetSafeNormal();
			const FVector3f Aft = FVector3f::CrossProduct(FVector3f::RightVector, Up).GetSafeNormal() * -1.f;
			const float Chord0 = FMath::Clamp(D.Length * 0.035f, 0.5f, 1.4f), Chord1 = 0.32f;
			constexpr int32 Pieces = 6;
			for (int32 I = 0; I < Pieces; ++I)
			{
				const float A = float(I) / Pieces, Bf = float(I + 1) / Pieces;
				const float Chord = FMath::Lerp(Chord0, Chord1, FMath::Pow((A + Bf) * 0.5f, 0.7f));
				FShipFrame F;
				F.X = Up;
				F.Y = FVector3f::RightVector;
				F.Z = Aft.Dot(FVector3f::ForwardVector) < 0.f ? Aft : -Aft;
				F.O = FMath::Lerp(P0, P1, (A + Bf) * 0.5f) + F.Z * (Chord * 0.5f);
				const bool bWet = F.O.Z < 0.15f;
				B.Box(Wood, F, FVector3f((P1 - P0).Size() / Pieces * 0.5f, 0.09f, Chord * 0.5f), bWet ? Bottom : Black);
			}
			// Pintles and gudgeons.
			for (int32 I = 1; I < 5; ++I)
			{
				FShipFrame F;
				F.X = Up;
				F.Y = FVector3f::RightVector;
				F.Z = Aft.Dot(FVector3f::ForwardVector) < 0.f ? Aft : -Aft;
				F.O = FMath::Lerp(P0, P1, I / 5.5f) + F.Z * 0.05f;
				B.Box(Metal, F, FVector3f(0.04f, 0.11f, 0.32f), FBuilder::IronColor);
			}
		}
	}

	void FHullBuild::Head()
	{
		// The head: a knee standing out before the stem under the bowsprit, with its cutwater, rails and figurehead
		// (frigates, galleons with a long beakhead, brigantines a small one; sloops have a plain raked stem).
		if (D.Type == EShipType::Sloop)
		{
			return;
		}
		const float Top = L.RailAt(L.HalfLength);
		const float Reach = D.Length * (D.Type == EShipType::Galleon ? 0.1f : (D.Type == EShipType::Frigate ? 0.055f : 0.04f));
		const float H0 = D.Draft + 0.3f;
		const FVector3f Start(L.XBow(H0) + 0.15f, 0.f, L.Z(H0));
		const FVector3f End(L.XBow(Top) + Reach, 0.f, L.Z(Top) - 1.f);
		const FVector3f Control(Start.X + Reach * 0.85f, 0.f, Start.Z + 0.4f);
		auto Knee = [&](float T) { return FMath::Lerp(FMath::Lerp(Start, Control, T), FMath::Lerp(Control, End, T), T); };
		const FLinearColor Black(D.WaleColor.R * 1.2f, D.WaleColor.G * 1.2f, D.WaleColor.B * 1.2f, 1.f);
		TArray<FVector3f> Path;
		for (int32 I = 0; I <= 12; ++I)
		{
			Path.Add(Knee(I / 12.f));
		}
		B.Beam(Wood, Path, FVector3f(-1.f, 0.f, 1.f), 0.26f, 0.32f, Black);
		// The cheek between the knee and the stem, both faces.
		for (float Side : { -1.f, 1.f })
		{
			auto Web = [&](float U, float V)
			{
				const FVector3f K = Knee(U);
				const float H = FMath::Clamp(K.Z + D.Draft, H0, Top);
				const FVector3f S(L.XBow(H) + 0.1f, 0.f, K.Z);
				return FMath::Lerp(S, K, V) + FVector3f(0.f, Side * 0.11f, 0.f);
			};
			B.Grid(Wood, 12, 3, Web, FLinearColor(D.TopsideColor.R * 0.8f, D.TopsideColor.G * 0.8f, D.TopsideColor.B * 0.8f, 1.f), Side < 0.f);
		}
		// Head rails: from the knee's end back to the bow, two a side, sweeping up.
		for (float Side : { -1.f, 1.f })
		{
			for (int32 Rail = 0; Rail < 2; ++Rail)
			{
				const float H = Top - 0.7f - Rail * 0.55f;
				const float Xb = L.XBow(H) - 1.2f - Rail * 0.4f;
				const FVector3f Bow = L.SidePoint(Xb, H, Side) + FVector3f(0.f, Side * D.PlankThickness, 0.f);
				const FVector3f Tip = End + FVector3f(-0.3f - Rail * 0.4f, Side * 0.16f, 0.15f - Rail * 0.35f);
				TArray<FVector3f> Sweep;
				for (int32 I = 0; I <= 10; ++I)
				{
					const float T = I / 10.f;
					FVector3f P = FMath::Lerp(Tip, Bow, T);
					P.Y = FMath::Lerp(Tip.Y, Bow.Y, FMath::Pow(T, 0.6f));
					P.Z -= FMath::Sin(T * UE_PI) * 0.25f;
					Sweep.Add(P);
				}
				B.Beam(Wood, Sweep, FVector3f::UpVector, 0.09f, 0.13f, Black);
			}
		}
		// Galleons: the beakhead's platform over the knee.
		if (D.Type == EShipType::Galleon)
		{
			const FVector3f A(L.XBow(Top) - 0.2f, 0.f, End.Z + 0.25f);
			FShipFrame Deck;
			Deck.O = (A + FVector3f(End.X - 0.4f, 0.f, End.Z + 0.25f)) * 0.5f;
			B.Box(Wood, Deck, FVector3f((End.X - 0.4f - A.X) * 0.5f, 0.75f, 0.05f), D.DeckColor * 0.8f);
		}
		// Figurehead: a crowned lion (gilt) on galleons, a painted figure on the others, leaning forward on the knee.
		{
			const FVector3f Dir = FVector3f(0.55f, 0.f, 1.f).GetSafeNormal();
			const float S = FMath::Clamp(D.Length / 30.f, 0.7f, 1.4f);
			const FVector2f Profile[] = { { 0.f, 0.f }, { 0.f, 0.2f * S }, { 0.15f * S, 0.26f * S }, { 0.45f * S, 0.24f * S }, { 0.7f * S, 0.17f * S },
				{ 0.8f * S, 0.12f * S }, { 0.86f * S, 0.15f * S }, { 1.f * S, 0.18f * S }, { 1.12f * S, 0.15f * S }, { 1.2f * S, 0.08f * S }, { 1.23f * S, 0.f } };
			const FLinearColor Color = D.Type == EShipType::Galleon ? FLinearColor(0.5f, 0.36f, 0.12f, 1.f) : FLinearColor(0.36f, 0.26f, 0.09f, 1.f);
			B.Lathe(Metal, End - Dir * 0.2f - FVector3f(0.25f, 0.f, 0.f), Dir, Profile, 10, Color);
		}
	}

	void FHullBuild::Transom()
	{
		const float Top = L.RailAt(-L.HalfLength);
		float Start = 0.f;
		while (Start < Top && L.PointSH(-1.f, Start).Y < 0.28f)
		{
			Start += 0.02f;
		}
		const float Row = D.PlankWidth * 0.9f;
		const float Thick = D.PlankThickness;
		int32 Index = 0;
		for (float H0 = Start; H0 < Top - 0.02f; H0 += Row, ++Index)
		{
			const float H1 = FMath::Min(H0 + Row, Top);
			const float Hm = 0.5f * (H0 + H1);
			const FVector3f A(L.XStern(H0), 0.f, L.Z(H0)), Bv(L.XStern(H1), 0.f, L.Z(H1));
			const FVector3f Up = (Bv - A).GetSafeNormal();
			const FVector3f Aft = FVector3f(-Up.Z, 0.f, Up.X).GetSafeNormal() * (Up.X > 0.f ? -1.f : 1.f);
			const float Half = L.PointSH(-1.f, Hm).Y;
			const FVector3f Out = Aft.X < 0.f ? Aft : -Aft;
			FLinearColor Color = L.Z(Hm) < 0.15f ? D.BottomColor : (D.BandColor.A > 0.f && Hm > L.MainDeck(-L.HalfLength) - 0.4f ? D.BandColor : D.TopsideColor);
			Color = Jitter(Color, 0.12f, Index, 77, D.Seed);
			Color.A = 1.f;
			{
				// A trapezoid following the stern's outline (the hull's planks end under its edges).
				const float Y0 = L.PointSH(-1.f, H0).Y + D.PlankThickness, Y1 = L.PointSH(-1.f, H1).Y + D.PlankThickness;
				const FVector3f A0 = A + Up * 0.003f, B0 = Bv - Up * 0.003f;
				const FVector3f A1 = A0 + Out * Thick, B1 = B0 + Out * Thick;
				const FVector3f Y(0.f, 1.f, 0.f);
				B.FlatQuad(Wood, A1 - Y * Y0, A1 + Y * Y0, B1 + Y * Y1, B1 - Y * Y1, Color, Out);
				B.FlatQuad(Wood, A0 - Y * Y0, A0 + Y * Y0, A1 + Y * Y0, A1 - Y * Y0, Color * 0.7f, -Up);
				B.FlatQuad(Wood, B0 - Y * Y1, B0 + Y * Y1, B1 + Y * Y1, B1 - Y * Y1, Color * 0.7f, Up);
				for (float Side : { -1.f, 1.f })
				{
					B.FlatQuad(Wood, A0 + Y * (Side * Y0), A1 + Y * (Side * Y0), B1 + Y * (Side * Y1), B0 + Y * (Side * Y1), Color * 0.8f, Y * Side);
				}
			}
			const FShipFrame F = FShipFrame{ (A + Bv) * 0.5f, FVector3f::RightVector, Out, Up };
			// Caulking behind.
			B.FlatQuad(Wood, A + FVector3f(0.f, -Half, 0.f), A + FVector3f(0.f, Half, 0.f), Bv + FVector3f(0.f, Half, 0.f), Bv + FVector3f(0.f, -Half, 0.f), Caulk, F.Y);
		}
		// Inside of the transom above the top deck.
		{
			const float Hd = L.DeckAt(L.XStern(Top) + L.FrameThickness + 0.05f);
			const float X0 = L.XStern(Hd) + L.FrameThickness, X1 = L.XStern(Top) + L.FrameThickness;
			const float W0 = L.PointSH(-1.f, Hd).Y - L.FrameThickness, W1 = L.PointSH(-1.f, Top).Y - L.FrameThickness;
			const FLinearColor Inside = (D.Type == EShipType::Frigate || D.Type == EShipType::Galleon)
				? FLinearColor(0.26f, 0.055f, 0.035f, 1.f) : D.TopsideColor * 1.6f;
			B.FlatQuad(Wood, FVector3f(X0, -W0, L.Z(Hd)), FVector3f(X0, W0, L.Z(Hd)), FVector3f(X1, W1, L.Z(Top)), FVector3f(X1, -W1, L.Z(Top)),
				Inside, FVector3f::ForwardVector);
		}
		// Stern windows of the great cabin (frigates, galleons), between the main deck and the quarterdeck.
		if (D.Type == EShipType::Frigate || D.Type == EShipType::Galleon || D.Type == EShipType::Brigantine)
		{
			const float Hd = L.MainDeck(-L.HalfLength);
			const float Raised = L.RaisedAt(-L.HalfLength + 0.5f);
			const int32 Tiers = D.Type == EShipType::Galleon && Raised > 3.f ? 2 : 1;
			for (int32 Tier = 0; Tier < Tiers; ++Tier)
			{
				const float Floor = Tier == 0 ? Hd : Hd + D.RaisedDecks[0].Height;
				const float W0 = Floor + 0.75f, W1 = Floor + (D.Type == EShipType::Brigantine ? 1.45f : 1.75f);
				if (W1 > Top - 0.25f)
				{
					continue;
				}
				const float Half = L.PointSH(-1.f, W0).Y;
				const int32 Count = FMath::Clamp(FMath::FloorToInt(Half * 2.f / 0.95f), 2, 7);
				const float Pitch = Half * 2.f / Count;
				for (int32 I = 0; I < Count; ++I)
				{
					const float Yc = -Half + Pitch * (I + 0.5f);
					const FVector3f A(L.XStern(W0), 0.f, L.Z(W0)), Bv(L.XStern(W1), 0.f, L.Z(W1));
					const FVector3f Up = (Bv - A).GetSafeNormal();
					const FVector3f Aft = FVector3f(-1.f, 0.f, 0.f);
					const float Offset = D.PlankThickness + 0.02f;
					const float Wh = Pitch * 0.5f - 0.12f;
					// Glass in small leaded panes, a little in from the frame.
					const FVector3f G0 = A + Aft * Offset + FVector3f(0.f, Yc - Wh, 0.f), G1 = A + Aft * Offset + FVector3f(0.f, Yc + Wh, 0.f);
					const FVector3f G2 = Bv + Aft * Offset + FVector3f(0.f, Yc + Wh, 0.f), G3 = Bv + Aft * Offset + FVector3f(0.f, Yc - Wh, 0.f);
					B.FlatQuad(Gloss, G0, G1, G2, G3, FLinearColor(0.025f, 0.035f, 0.04f, 1.f), Aft);
					// Frame: four bars; mullions dividing the panes.
					const FLinearColor FrameColor(0.3f, 0.26f, 0.18f, 1.f);
					auto Bar = [&](const FVector3f& P, const FVector3f& Q, float Width)
					{
						TArray<FVector3f> Path = { P + Aft * 0.04f, Q + Aft * 0.04f };
						// Up = aft: the bar's width lies in the transom, its thickness along X.
						B.Beam(Wood, Path, FVector3f(-1.f, 0.f, 0.f), Width, 0.06f, FrameColor);
					};
					Bar(G0 + FVector3f(0.f, -0.05f, 0.f), G1 + FVector3f(0.f, 0.05f, 0.f), 0.1f);
					Bar(G3 + FVector3f(0.f, -0.05f, 0.f), G2 + FVector3f(0.f, 0.05f, 0.f), 0.1f);
					Bar(G0, G3, 0.1f);
					Bar(G1, G2, 0.1f);
					Bar((G0 + G1) * 0.5f, (G3 + G2) * 0.5f, 0.035f);
					for (int32 Pane = 1; Pane < 3; ++Pane)
					{
						const float T = Pane / 3.f;
						Bar(FMath::Lerp(G0, G3, T), FMath::Lerp(G1, G2, T), 0.03f);
					}
				}
				// A carved, gilded moulding under the windows.
				{
					const FVector3f A(L.XStern(W0 - 0.15f) - D.PlankThickness - 0.05f, 0.f, L.Z(W0 - 0.15f));
					TArray<FVector3f> Path = { A + FVector3f(0.f, -Half - 0.05f, 0.f), A + FVector3f(0.f, Half + 0.05f, 0.f) };
					B.Beam(Metal, Path, FVector3f::UpVector, 0.12f, 0.1f, D.Type == EShipType::Galleon ? FLinearColor(0.5f, 0.36f, 0.12f, 1.f) : FLinearColor(0.32f, 0.22f, 0.07f, 1.f));
				}
			}
		}
	}

	void FHullBuild::Rails(float Side)
	{
		const float FT = L.FrameThickness;
		const FLinearColor RailColor(D.WaleColor.R * 1.3f, D.WaleColor.G * 1.3f, D.WaleColor.B * 1.3f, 1.f);
		TArray<FVector3f> Path;
		for (int32 I = 0; I <= 160; ++I)
		{
			const float S = FMath::Lerp(-1.f, 0.985f, float(I) / 160);
			const FVector3f N = L.Normal(S, 1.f);
			const FVector3f P = L.Surface(S, 1.f) + N * ((D.PlankThickness - FT) * 0.5f) + FVector3f(0.f, 0.f, 0.05f);
			Path.Add(Mirror(P, Side));
		}
		B.Beam(Wood, Path, FVector3f::UpVector, FT + D.PlankThickness + 0.07f, 0.1f, RailColor);
		if (Side > 0.f)
		{
			// Taffrail across the stern.
			const float Top = L.RailAt(-L.HalfLength);
			const float Half = L.PointSH(-1.f, Top).Y;
			const FVector3f A(L.XStern(Top) + FT * 0.4f, 0.f, L.Z(Top) + 0.05f);
			TArray<FVector3f> Taff = { A + FVector3f(0.f, -Half, 0.f), A + FVector3f(0.f, Half, 0.f) };
			B.Beam(Wood, Taff, FVector3f::UpVector, FT + 0.12f, 0.12f, RailColor);
		}
	}

	void FHullBuild::Decks()
	{
		const float FT = L.FrameThickness;
		// Deck regions: between the edges of the raised decks.
		TArray<float> Edges = { -L.HalfLength, L.HalfLength };
		for (const FShipDeckLevel& Level : D.RaisedDecks)
		{
			Edges.Add(-L.HalfLength + Level.From * D.Length);
			Edges.Add(-L.HalfLength + Level.To * D.Length);
		}
		SortUnique(Edges);
		const float Lane = FMath::Clamp(D.PlankWidth * 0.85f, 0.16f, 0.26f);
		const float DeckThick = 0.07f;
		int32 Region = 0;
		for (int32 E = 0; E + 1 < Edges.Num(); ++E, ++Region)
		{
			const float XA = Edges[E], XB = Edges[E + 1];
			if (XB - XA < 0.2f)
			{
				continue;
			}
			const float Raise = L.RaisedAt(0.5f * (XA + XB));
			auto Hd = [&](float X) { return L.MainDeck(X) + Raise; };
			auto Inner = [&](float X)
			{
				if (X < L.XStern(Hd(X)) + FT)
				{
					return 0.f;
				}
				return FMath::Max(L.HalfBreadth(X, Hd(X)) - FT, 0.f);
			};
			// Pitch-black seams: a dark floor under the planks.
			{
				auto Floor = [&](float U, float V)
				{
					const float X = FMath::Lerp(XA, XB, U);
					const float Hb = FMath::Max(Inner(X), 0.02f);
					return FVector3f(X, FMath::Lerp(-Hb, Hb, V), L.Z(Hd(X)) - DeckThick + 0.005f);
				};
				const int32 NU = FMath::Max(4, FMath::CeilToInt((XB - XA) / 0.4f));
				B.Grid(Wood, NU, 6, Floor, Caulk);
			}
			// Planks: straight lanes parallel to the centreline, butts staggered, ending where the hull closes in.
			const int32 Lanes = FMath::CeilToInt(L.HalfBeam / Lane) + 1;
			for (int32 J = -Lanes; J <= Lanes; ++J)
			{
				const float Y = J * Lane;
				const float Need = FMath::Abs(Y) + Lane * 0.5f + 0.18f; // a margin for the waterway
				float RunStart = TNumericLimits<float>::Max();
				const float Step = 0.08f;
				const float Butt = D.PlankLength * 0.8f;
				const float Offset = Hash(J, Region, D.Seed) * Butt;
				auto EmitRun = [&](float R0, float R1)
				{
					if (R1 - R0 < 0.3f)
					{
						return;
					}
					float P0 = R0;
					int32 Piece = 0;
					while (P0 < R1 - 0.05f)
					{
						float P1 = FMath::Min(R1, XA + Offset + Butt * (FMath::FloorToFloat((P0 - XA - Offset) / Butt) + 1.f));
						if (P1 - P0 < 0.05f)
						{
							P1 = FMath::Min(R1, P1 + Butt);
						}
						TArray<FVector3f> Path;
						const int32 Count = FMath::Max(1, FMath::CeilToInt((P1 - P0) / 0.6f));
						for (int32 I = 0; I <= Count; ++I)
						{
							const float X = FMath::Lerp(P0 + 0.004f, P1 - 0.004f, float(I) / Count);
							Path.Add(FVector3f(X, Y, L.Z(Hd(X)) - DeckThick * 0.5f));
						}
						FLinearColor C = Jitter(D.DeckColor, 0.16f, J * 13 + Piece, Region, D.Seed);
						C.A = 1.f;
						B.Beam(Wood, Path, FVector3f::UpVector, Lane - 0.008f, DeckThick, C);
						P0 = P1;
						++Piece;
					}
				};
				for (float X = XA + 0.02f; X <= XB - 0.02f + 1e-4f; X += Step)
				{
					const bool bIn = Inner(X) >= Need;
					if (bIn && RunStart == TNumericLimits<float>::Max())
					{
						RunStart = X;
					}
					if ((!bIn || X + Step > XB - 0.02f) && RunStart != TNumericLimits<float>::Max())
					{
						EmitRun(RunStart, bIn ? XB - 0.02f : X - Step);
						RunStart = TNumericLimits<float>::Max();
					}
				}
			}
			// Waterways along the sides, covering the plank ends.
			for (float Side : { 1.f, -1.f })
			{
				TArray<FVector3f> Path;
				for (float X = XA; X <= XB + 1e-3f; X += 0.25f)
				{
					const float Hb = Inner(X);
					if (Hb < 0.3f)
					{
						if (Path.Num() > 1)
						{
							B.Beam(Wood, Path, FVector3f::UpVector, 0.36f, 0.09f, Jitter(D.DeckColor * 0.7f, 0.05f, Region, int32(Side)));
						}
						Path.Reset();
						continue;
					}
					Path.Add(FVector3f(X, Side * (Hb - 0.16f), L.Z(Hd(X)) - 0.01f));
				}
				if (Path.Num() > 1)
				{
					B.Beam(Wood, Path, FVector3f::UpVector, 0.36f, 0.09f, Jitter(D.DeckColor * 0.7f, 0.05f, Region, int32(Side)));
				}
			}
		}
		// Breaks of the raised decks: a planked bulkhead from the deck below, a beam on its edge, a balustrade on top.
		for (const FShipDeckLevel& Level : D.RaisedDecks)
		{
			for (int32 End = 0; End < 2; ++End)
			{
				const float Share = End == 0 ? Level.From : Level.To;
				if (Share <= 0.01f || Share >= 0.99f)
				{
					continue;
				}
				const float X = -L.HalfLength + Share * D.Length;
				const float Outward = End == 0 ? -1.f : 1.f; // from the raised deck towards the lower one
				const float Top = L.DeckAt(X - Outward * 0.05f);
				const float Below = L.DeckAt(X + Outward * 0.05f);
				if (Top - Below < 0.2f)
				{
					continue;
				}
				const float Hb = FMath::Max(L.HalfBreadth(X, Below) - FT, 0.3f);
				const int32 Boards = FMath::CeilToInt(Hb * 2.f / 0.28f);
				const float Pitch = Hb * 2.f / Boards;
				for (int32 I = 0; I < Boards; ++I)
				{
					FShipFrame F;
					F.X = FVector3f(Outward, 0.f, 0.f);
					F.Y = FVector3f::RightVector;
					F.Z = FVector3f::UpVector;
					F.O = FVector3f(X + Outward * 0.03f, -Hb + Pitch * (I + 0.5f), L.Z(0.5f * (Top + Below)));
					FLinearColor C = Jitter(D.TopsideColor * 1.5f, 0.12f, I, int32(X * 10.f), D.Seed);
					C.A = 1.f;
					B.Box(Wood, F, FVector3f(0.03f, Pitch * 0.5f - 0.004f, (Top - Below) * 0.5f), C);
				}
				// Doors to the cabins under it (only under a quarterdeck or poop: facing forward).
				if (Outward > 0.f && Top - Below > 1.7f)
				{
					for (float Yd : { -1.f, 1.f })
					{
						FShipFrame F;
						F.X = FVector3f(1.f, 0.f, 0.f);
						F.Y = FVector3f::RightVector;
						F.Z = FVector3f::UpVector;
						F.O = FVector3f(X + 0.07f, Yd * FMath::Min(1.3f, Hb * 0.5f), L.Z(Below + 0.85f));
						B.Box(Wood, F, FVector3f(0.02f, 0.38f, 0.85f), FLinearColor(0.07f, 0.045f, 0.025f, 1.f));
					}
				}
				// Edge beam and balustrade.
				const float HbTop = FMath::Max(L.HalfBreadth(X, Top) - FT, 0.3f);
				{
					TArray<FVector3f> Path = { FVector3f(X + Outward * 0.04f, -HbTop, L.Z(Top) - 0.06f), FVector3f(X + Outward * 0.04f, HbTop, L.Z(Top) - 0.06f) };
					B.Beam(Wood, Path, FVector3f::UpVector, 0.16f, 0.16f, FLinearColor(D.WaleColor.R * 1.5f, D.WaleColor.G * 1.5f, D.WaleColor.B * 1.5f, 1.f));
					TArray<FVector3f> Rail = { FVector3f(X - Outward * 0.08f, -HbTop, L.Z(Top) + 0.95f), FVector3f(X - Outward * 0.08f, HbTop, L.Z(Top) + 0.95f) };
					B.Beam(Wood, Rail, FVector3f::UpVector, 0.12f, 0.09f, FLinearColor(D.WaleColor.R * 1.5f, D.WaleColor.G * 1.5f, D.WaleColor.B * 1.5f, 1.f));
				}
				const FVector2f Baluster[] = { { 0.f, 0.035f }, { 0.08f, 0.035f }, { 0.12f, 0.05f }, { 0.35f, 0.04f }, { 0.55f, 0.028f },
					{ 0.75f, 0.032f }, { 0.85f, 0.03f }, { 0.9f, 0.035f } };
				const int32 Count = FMath::FloorToInt(HbTop * 2.f / 0.3f);
				for (int32 I = 0; I <= Count; ++I)
				{
					const float Y = -HbTop + HbTop * 2.f * I / FMath::Max(Count, 1);
					B.Lathe(Wood, FVector3f(X - Outward * 0.08f, Y, L.Z(Top)), FVector3f::UpVector, Baluster, 6,
						FLinearColor(D.TopsideColor.R * 1.8f, D.TopsideColor.G * 1.8f, D.TopsideColor.B * 1.8f, 1.f));
				}
			}
		}
	}
}

void ShipHull::Build(FBuilder& B, const FHullLines& L, TArray<FShipGunPort>& OutPorts)
{
	FHullBuild Build{ B, L, L.D, OutPorts };
	const FShipDesign& D = L.D;
	Build.Strakes = FMath::Max(10, FMath::RoundToInt(L.Girth(0.f) / D.PlankWidth));
	for (float Wale : D.Wales)
	{
		const float T = L.GirthAt(0.f, Wale + D.Draft);
		const int32 Strake = FMath::Clamp(FMath::FloorToInt(T * Build.Strakes), 1, Build.Strakes - 3);
		Build.WaleStrakes.AddUnique(Strake);
		Build.WaleStrakes.AddUnique(Strake + 1);
	}
	if (D.BandColor.A > 0.f)
	{
		int32 Highest = 0;
		for (int32 S : Build.WaleStrakes)
		{
			Highest = FMath::Max(Highest, S);
		}
		Build.BandFrom = Highest + 1;
		Build.BandTo = Build.Strakes - 2;
	}
	Build.PlanPorts();
	for (float Side : { 1.f, -1.f })
	{
		Build.Planking(Side);
		Build.Bulwarks(Side);
		Build.PortLinings(Side);
		Build.Rails(Side);
	}
	Build.Backbone();
	Build.Transom();
	Build.Decks();
}
