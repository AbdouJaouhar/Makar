#include "Ocean/OceanFlora.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

int32 FOceanMeshData::AddVertex(const FVector3f& Position, const FVector3f& Normal, const FVector2f& UV, const FLinearColor& Color)
{
	return Vertices.Add({ Position, Normal.GetSafeNormal(), UV, Color, CurrentWind });
}

void FOceanMeshData::AddTriangle(int32 Section, int32 A, int32 B, int32 C)
{
	Sections[Section].Append({ A, B, C });
}

namespace
{
	constexpr int32 SectionBark = 0;
	constexpr int32 SectionFoliage = 1;
	// Palms only: their foliage material is a textured leaf card, so the coconuts get a vertex-coloured section.
	constexpr int32 SectionNuts = 2;
	// Plants with real leaves (near level of detail): the leaves in their own section, with their own material (the
	// far level's cards keep section 1).
	constexpr int32 SectionLeaves = 2;

	// --- 3D value noise -------------------------------------------------------------------------------------

	uint32 Hash3(int32 X, int32 Y, int32 Z)
	{
		uint32 H = uint32(X) * 0x8da6b343u ^ uint32(Y) * 0xd8163841u ^ uint32(Z) * 0xcb1ab31fu;
		H ^= H >> 16;
		H *= 0x7feb352du;
		H ^= H >> 15;
		H *= 0x846ca68bu;
		H ^= H >> 16;
		return H;
	}

	float ValueNoise3(const FVector3f& P)
	{
		const FVector3f Cell(FMath::FloorToFloat(P.X), FMath::FloorToFloat(P.Y), FMath::FloorToFloat(P.Z));
		const FVector3f F = P - Cell;
		const FVector3f U(F.X * F.X * (3.f - 2.f * F.X), F.Y * F.Y * (3.f - 2.f * F.Y), F.Z * F.Z * (3.f - 2.f * F.Z));
		const int32 X = int32(Cell.X), Y = int32(Cell.Y), Z = int32(Cell.Z);
		auto V = [&](int32 DX, int32 DY, int32 DZ) { return float(Hash3(X + DX, Y + DY, Z + DZ)) / 4294967295.f; };
		const float X00 = FMath::Lerp(V(0, 0, 0), V(1, 0, 0), U.X);
		const float X10 = FMath::Lerp(V(0, 1, 0), V(1, 1, 0), U.X);
		const float X01 = FMath::Lerp(V(0, 0, 1), V(1, 0, 1), U.X);
		const float X11 = FMath::Lerp(V(0, 1, 1), V(1, 1, 1), U.X);
		return FMath::Lerp(FMath::Lerp(X00, X10, U.Y), FMath::Lerp(X01, X11, U.Y), U.Z);
	}

	float Fractal3(FVector3f P, int32 Octaves)
	{
		float Sum = 0.f, Amplitude = 0.5f, Total = 0.f;
		for (int32 Octave = 0; Octave < Octaves; ++Octave)
		{
			Sum += ValueNoise3(P) * Amplitude;
			Total += Amplitude;
			P *= 2.03f;
			Amplitude *= 0.5f;
		}
		return Sum / Total;
	}

	// --- Primitives -----------------------------------------------------------------------------------------

	/** Unit icosphere (radius 1) subdivided Subdivisions times. */
	void MakeIcosphere(int32 Subdivisions, TArray<FVector3f>& OutPositions, TArray<int32>& OutTriangles)
	{
		const float T = (1.f + FMath::Sqrt(5.f)) * 0.5f;
		OutPositions = {
			{ -1, T, 0 }, { 1, T, 0 }, { -1, -T, 0 }, { 1, -T, 0 }, { 0, -1, T }, { 0, 1, T },
			{ 0, -1, -T }, { 0, 1, -T }, { T, 0, -1 }, { T, 0, 1 }, { -T, 0, -1 }, { -T, 0, 1 } };
		for (FVector3f& P : OutPositions)
		{
			P.Normalize();
		}
		OutTriangles = { 0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
			3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1 };
		for (int32 Level = 0; Level < Subdivisions; ++Level)
		{
			TMap<uint64, int32> Midpoints;
			auto Midpoint = [&](int32 A, int32 B)
			{
				const uint64 Key = (uint64(FMath::Min(A, B)) << 32) | uint64(FMath::Max(A, B));
				if (const int32* Found = Midpoints.Find(Key))
				{
					return *Found;
				}
				const int32 Index = OutPositions.Add(((OutPositions[A] + OutPositions[B]) * 0.5f).GetSafeNormal());
				Midpoints.Add(Key, Index);
				return Index;
			};
			TArray<int32> Next;
			Next.Reserve(OutTriangles.Num() * 4);
			for (int32 I = 0; I < OutTriangles.Num(); I += 3)
			{
				const int32 A = OutTriangles[I], B = OutTriangles[I + 1], C = OutTriangles[I + 2];
				const int32 AB = Midpoint(A, B), BC = Midpoint(B, C), CA = Midpoint(C, A);
				Next.Append({ A, AB, CA, B, BC, AB, C, CA, BC, AB, BC, CA });
			}
			OutTriangles = MoveTemp(Next);
		}
	}

	/** Smooth normals of an indexed triangle list. */
	TArray<FVector3f> SmoothNormals(const TArray<FVector3f>& Positions, const TArray<int32>& Triangles)
	{
		TArray<FVector3f> Normals;
		Normals.Init(FVector3f::ZeroVector, Positions.Num());
		for (int32 I = 0; I < Triangles.Num(); I += 3)
		{
			// The icosphere's triangles wind counter-clockwise seen from outside: this cross product points out.
			const FVector3f& A = Positions[Triangles[I]];
			const FVector3f Face = FVector3f::CrossProduct(Positions[Triangles[I + 1]] - A, Positions[Triangles[I + 2]] - A);
			Normals[Triangles[I]] += Face;
			Normals[Triangles[I + 1]] += Face;
			Normals[Triangles[I + 2]] += Face;
		}
		for (FVector3f& N : Normals)
		{
			N = N.GetSafeNormal();
		}
		return Normals;
	}

	/** Frustum-shaped tube from A to B (open ends), Sides around. */
	void AddTube(FOceanMeshData& Data, int32 Section, const FVector3f& A, const FVector3f& B, float RadiusA, float RadiusB,
		int32 Sides, const FLinearColor& ColorA, const FLinearColor& ColorB)
	{
		const FVector3f Axis = (B - A).GetSafeNormal();
		const FVector3f Helper = FMath::Abs(Axis.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector;
		const FVector3f SideX = FVector3f::CrossProduct(Axis, Helper).GetSafeNormal();
		const FVector3f SideY = FVector3f::CrossProduct(Axis, SideX);
		const int32 First = Data.Vertices.Num();
		for (int32 Ring = 0; Ring < 2; ++Ring)
		{
			for (int32 Side = 0; Side < Sides; ++Side)
			{
				const float Angle = 2.f * UE_PI * Side / Sides;
				const FVector3f Out = SideX * FMath::Cos(Angle) + SideY * FMath::Sin(Angle);
				Data.AddVertex((Ring == 0 ? A : B) + Out * (Ring == 0 ? RadiusA : RadiusB), Out,
					FVector2f(float(Side) / Sides, float(Ring)), Ring == 0 ? ColorA : ColorB);
			}
		}
		for (int32 Side = 0; Side < Sides; ++Side)
		{
			const int32 Next = (Side + 1) % Sides;
			const int32 A0 = First + Side, A1 = First + Next, B0 = First + Sides + Side, B1 = First + Sides + Next;
			Data.AddTriangle(Section, A0, B0, A1);
			Data.AddTriangle(Section, A1, B0, B1);
		}
	}

	/**
	 * A continuous tube along a curve (Curve(T), Radius(T), T in 0-1), for a procedural bark material. Consecutive
	 * rings share their vertices and a frame carried along the curve, so the tube bends without cracks (separate
	 * segments, each ring square to its own segment, open slits on the outside of a bend). U goes once around (0-1,
	 * with a duplicated seam column so it does not wrap back), V is the distance along the curve in metres.
	 */
	template <typename CurveType, typename RadiusType>
	void AddBarkTube(FOceanMeshData& Data, int32 Section, const CurveType& Curve, const RadiusType& Radius, int32 Rings, int32 Sides,
		const FLinearColor& Color)
	{
		const int32 First = Data.Vertices.Num();
		const int32 Row = Sides + 1;
		FVector3f SideX = FVector3f::ZeroVector;
		FVector3f Previous = Curve(0.f);
		float Distance = 0.f;
		for (int32 Ring = 0; Ring <= Rings; ++Ring)
		{
			const float T = float(Ring) / Rings;
			const FVector3f Point = Curve(T);
			Distance += FVector3f::Distance(Point, Previous);
			Previous = Point;
			const FVector3f Tangent = (Curve(FMath::Min(T + 0.01f, 1.f)) - Curve(FMath::Max(T - 0.01f, 0.f))).GetSafeNormal();
			if (Ring == 0)
			{
				const FVector3f Helper = FMath::Abs(Tangent.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector;
				SideX = FVector3f::CrossProduct(Tangent, Helper).GetSafeNormal();
			}
			else
			{
				SideX = (SideX - Tangent * FVector3f::DotProduct(SideX, Tangent)).GetSafeNormal(); // carried along the curve
			}
			const FVector3f SideY = FVector3f::CrossProduct(Tangent, SideX);
			const float R = Radius(T);
			for (int32 Side = 0; Side <= Sides; ++Side)
			{
				const float Angle = 2.f * UE_PI * Side / Sides;
				const FVector3f Out = SideX * FMath::Cos(Angle) + SideY * FMath::Sin(Angle);
				Data.AddVertex(Point + Out * R, Out, FVector2f(float(Side) / Sides, Distance * 0.01f), Color);
			}
			if (Ring > 0)
			{
				const int32 Below = First + (Ring - 1) * Row, Above = First + Ring * Row;
				for (int32 Side = 0; Side < Sides; ++Side)
				{
					Data.AddTriangle(Section, Below + Side, Above + Side, Below + Side + 1);
					Data.AddTriangle(Section, Below + Side + 1, Above + Side, Above + Side + 1);
				}
			}
		}
	}

	/** A lumpy ball of leaves, flattened underneath: the leaf mass at a branch tip, seen from a distance. */
	void AddLeafCluster(FOceanMeshData& Data, const FVector3f& Center, float Radius, FRandomStream& Random,
		const FLinearColor& Green, const FLinearColor& Accent, float AccentAmount, int32 Section = SectionFoliage)
	{
		static TArray<FVector3f> Sphere;
		static TArray<int32> SphereTriangles;
		if (Sphere.Num() == 0)
		{
			MakeIcosphere(1, Sphere, SphereTriangles);
		}
		const FVector3f Offset(Random.FRandRange(0.f, 100.f), Random.FRandRange(0.f, 100.f), Random.FRandRange(0.f, 100.f));
		const int32 First = Data.Vertices.Num();
		for (const FVector3f& Direction : Sphere)
		{
			const float Lump = 0.75f + 0.5f * ValueNoise3(Direction * 1.7f + Offset);
			FVector3f Position = Direction * Radius * Lump;
			Position.Z *= Direction.Z < 0.f ? 0.45f : 0.8f;
			// Outward normals, slightly up: foliage is lit like a volume, not like its facets.
			const FVector3f Normal = (Direction + FVector3f(0.f, 0.f, 0.3f)).GetSafeNormal();
			const float Shade = FMath::Lerp(0.75f, 1.1f, 0.5f + 0.5f * Direction.Z) * Random.FRandRange(0.9f, 1.1f);
			const FLinearColor Color = FMath::Lerp(Green, Accent, AccentAmount * ValueNoise3(Direction * 2.3f + Offset)) * Shade;
			Data.AddVertex(Center + Position, Normal, FVector2f(Direction.X, Direction.Y), Color);
		}
		for (int32 I = 0; I < SphereTriangles.Num(); I += 3)
		{
			Data.AddTriangle(Section, First + SphereTriangles[I], First + SphereTriangles[I + 2], First + SphereTriangles[I + 1]);
		}
	}

	/**
	 * A leaf mass drawn with cards: quads showing a cluster of real leaves (Infinigen leaves rendered from above,
	 * Scripts/infinigen/render_leaf_cluster.py; the whole image on each card), scattered through a flattened ball,
	 * mostly towards its surface, and facing mostly up and out as leaves do. Each card is built twice, front and back
	 * with opposite windings, both with normals bent outwards from the mass: it is lit as one soft volume rather than
	 * as flat cards (a two-sided material would flip the back face's normal). Inner cards are darker (self-shadowing).
	 * The vertex colour multiplies the rendered leaves (0.5 = as rendered).
	 */
	void AddLeafCards(FOceanMeshData& Data, int32 Section, const FVector3f& Center, float Radius, float CardSize,
		FRandomStream& Random, const FLinearColor& Tint)
	{
		// Enough cards to cover about half the mass's surface.
		const int32 Cards = FMath::Clamp(FMath::RoundToInt(12.8f * FMath::Square(Radius / CardSize)), 4, 28);
		const FVector3f Below = Center - FVector3f(0.f, 0.f, 0.3f * Radius); // normals radiate from a little below
		for (int32 Card = 0; Card < Cards; ++Card)
		{
			const FVector3f Direction(Random.GetUnitVector());
			const float Depth = Random.FRandRange(0.55f, 1.f);
			const FVector3f Position = Center + FVector3f(Direction.X, Direction.Y, Direction.Z * 0.7f) * (Radius * Depth);
			const FVector3f Normal = (Direction * 0.5f + FVector3f::UpVector * 0.8f + FVector3f(Random.GetUnitVector()) * 0.3f).GetSafeNormal();
			const FVector3f Tangent = FVector3f::CrossProduct(Normal, FVector3f(Random.GetUnitVector())).GetSafeNormal();
			const FVector3f Bitangent = FVector3f::CrossProduct(Normal, Tangent);
			const float Half = 0.5f * CardSize * Random.FRandRange(0.8f, 1.2f);
			const bool bMirror = Random.FRand() < 0.5f;
			const FLinearColor Color = Tint * Random.FRandRange(0.88f, 1.12f) * FMath::Lerp(0.6f, 1.f, (Depth - 0.55f) / 0.45f);
			const FVector3f Corners[4] = {
				Position - Tangent * Half - Bitangent * Half, Position + Tangent * Half - Bitangent * Half,
				Position + Tangent * Half + Bitangent * Half, Position - Tangent * Half + Bitangent * Half };
			const FVector2f UVs[4] = { FVector2f(0.f, 1.f), FVector2f(1.f, 1.f), FVector2f(1.f, 0.f), FVector2f(0.f, 0.f) };
			for (int32 Face = 0; Face < 2; ++Face)
			{
				const int32 First = Data.Vertices.Num();
				for (int32 Corner = 0; Corner < 4; ++Corner)
				{
					const FVector3f Radial = (Corners[Corner] - Below).GetSafeNormal();
					const FVector2f UV = bMirror ? FVector2f(1.f - UVs[Corner].X, UVs[Corner].Y) : UVs[Corner];
					Data.AddVertex(Corners[Corner], (Normal * 0.35f + Radial * 0.65f).GetSafeNormal(), UV, Color);
				}
				if (Face == 0)
				{
					Data.AddTriangle(Section, First, First + 1, First + 2);
					Data.AddTriangle(Section, First, First + 2, First + 3);
				}
				else
				{
					Data.AddTriangle(Section, First, First + 2, First + 1);
					Data.AddTriangle(Section, First, First + 3, First + 2);
				}
			}
		}
	}

	/**
	 * A leaf mass of real leaves (the near level of detail, instead of cards showing a rendered cluster): each leaf a
	 * small blade folded along its midrib (5 vertices, 4 triangles), on a short petiole pointing out of the mass,
	 * mostly facing up and out, bent a little by its own weight. Leaves fill a flattened ball, mostly towards its
	 * surface; their normals lean outwards from the mass so it is lit as one soft volume. Width = blade width over length;
	 * RedChance of them turning red (sea grape, tropical almond).
	 */
	void AddLeafSpray(FOceanMeshData& Data, int32 Section, const FVector3f& Center, float Radius, float LeafSize, float Width,
		const FLinearColor& Green, const FLinearColor& Accent, float AccentAmount, float RedChance, FRandomStream& Random)
	{
		// Leaves come in rosettes at the twig tips (6-10 radiating from one point, in all directions but mostly out and
		// up): clumps and gaps, as on a real crown, rather than an even confetti.
		const int32 Leaves = FMath::Clamp(FMath::RoundToInt(36.f * FMath::Square(Radius / LeafSize)), 24, 1600);
		const FVector3f Below = Center - FVector3f(0.f, 0.f, 0.35f * Radius);
		FVector3f Direction = FVector3f::UpVector, TwigPoint = Center;
		float Depth = 1.f;
		float TwigTone = 0.f;
		int32 LeftInTwig = 0;
		for (int32 Leaf = 0; Leaf < Leaves; ++Leaf)
		{
			if (LeftInTwig-- <= 0)
			{
				LeftInTwig = Random.RandRange(5, 9);
				Direction = FVector3f(Random.GetUnitVector());
				Depth = FMath::Sqrt(Random.FRandRange(0.2f, 1.f));
				TwigPoint = Center + FVector3f(Direction.X, Direction.Y, Direction.Z * 0.7f) * (Radius * Depth);
				TwigTone = Random.FRandRange(-1.f, 1.f);
			}
			const FVector3f Spoke(Random.GetUnitVector());
			const FVector3f Base = TwigPoint + Spoke * (0.08f * LeafSize);
			// Blade facing: up and out, with scatter; axis: out from the twig, drooping a little.
			const FVector3f Face = (Direction * 0.5f + FVector3f::UpVector * 0.7f + Spoke * 0.5f).GetSafeNormal();
			FVector3f Axis = (FVector3f(Spoke.X, Spoke.Y, 0.f).GetSafeNormal() * 0.8f + Direction * 0.5f
				- FVector3f::UpVector * 0.25f);
			Axis = (Axis - Face * FVector3f::DotProduct(Axis, Face)).GetSafeNormal();
			if (Axis.IsNearlyZero())
			{
				continue;
			}
			const FVector3f Side = FVector3f::CrossProduct(Face, Axis);
			const float Length = LeafSize * Random.FRandRange(0.7f, 1.25f);
			const float Half = 0.5f * Width * Length;
			const float Fold = 0.25f * Half; // the edges sit below the midrib: a V cross-section
			const float Sag = 0.12f * Length;  // the tip bends down
			const FVector3f Root = Base; // the blade material draws the stalk
			FLinearColor Color = FMath::Lerp(Green, Accent, AccentAmount * Random.FRand()) * Random.FRandRange(0.8f, 1.2f);
			if (Random.FRand() < RedChance)
			{
				Color = FMath::Lerp(FLinearColor(0.32f, 0.07f, 0.03f), FLinearColor(0.38f, 0.2f, 0.03f), Random.FRand());
			}
			// Inner leaves are darker (shaded by the mass).
			Color *= FMath::Lerp(0.55f, 1.f, Depth);
			// Alpha = ambient occlusion (M_LeafBlade): leaves deep in the mass and on its underside see little sky, and must not
			// mirror it either (unoccluded, the shaded leaves inside the bushes shone white at grazing angles).
			const float Height = FMath::Clamp(0.5f + 0.5f * (Base.Z - Center.Z) / FMath::Max(Radius, 1.f), 0.f, 1.f);
			// Each twig its own shade (young growth yellower, older leaves bluer), a little less saturated than a pure green,
			// and the sunlit leaves on top of the mass paler: a crown of many greens rather than one flat colour.
			const FLinearColor Shift = TwigTone > 0.f ? FMath::Lerp(FLinearColor::White, FLinearColor(1.25f, 1.1f, 0.55f), TwigTone)
				: FMath::Lerp(FLinearColor::White, FLinearColor(0.82f, 0.96f, 1.2f), -TwigTone);
			Color = Color * Shift;
			const float Luma = 0.3f * Color.R + 0.59f * Color.G + 0.11f * Color.B;
			Color = FMath::Lerp(FLinearColor(Luma, Luma, Luma), Color, 0.8f) * FMath::Lerp(0.92f, 1.15f, Height * Depth);
			Color.A = FMath::Lerp(0.15f, 1.f, Depth * Depth) * FMath::Lerp(0.45f, 1.f, Height);
			const FVector3f Radial = (Base - Below).GetSafeNormal();
			auto Normal = [&](const FVector3f& Blade) { return (Blade * 0.45f + Radial * 0.55f).GetSafeNormal(); };
			// A small curved quad (3 x 3 vertices, 8 triangles) holding the blade: M_LeafBlade cuts the leaf's outline (a stalk,
			// an elliptic blade pointed at its tip) and draws its veins from the UVs (U across, V = 1 at the stalk, 0 at the tip),
			// sharp at any distance. Its edges sit below the midrib (a shallow V) and it curves down more towards its tip.
			const FVector3f LeftNormal = Normal((Face - Side * 0.5f).GetSafeNormal());
			const FVector3f RightNormal = Normal((Face + Side * 0.5f).GetSafeNormal());
			// The outline is modelled (opaque, Nanite's fast path: a masked material made it rasterise every leaf in its
			// programmable path and hid nothing behind the crowns), the same as the material drew it: a short stalk, then an
			// elliptic blade rounded at its base, pointed at its tip. Stations along the leaf (U = 0 at the stalk), each a
			// left margin, the midrib and a right margin; the UVs keep the material's convention (U across, scaled to the
			// blade's width there, so its veins follow; V = 1 at the stalk, 0 at the tip).
			static const float Stations[] = { 0.f, 0.07f, 0.16f, 0.28f, 0.42f, 0.56f, 0.7f, 0.83f, 0.93f, 1.f };
			const int32 First = Data.Vertices.Num();
			for (const float U : Stations)
			{
				const float S = FMath::Clamp((U - 0.07f) / 0.93f, 0.f, 1.f);
				const float Blade = U > 0.07f ? FMath::Pow(FMath::Sin(UE_PI * FMath::Pow(S, 0.8f)), 0.6f) : 0.f;
				const float W = FMath::Max(Blade, U < 0.1f ? 0.06f : 0.f);
				const FVector3f C = Root + Axis * (U * Length) + Face * (0.03f * Length * FMath::Sin(UE_PI * U)) - FVector3f::UpVector * (Sag * U * U);
				Data.AddVertex(C - Side * (Half * W) - Face * (Fold * W), LeftNormal, FVector2f(0.5f - 0.5f * W, 1.f - U), Color);
				Data.AddVertex(C, Normal(Face), FVector2f(0.5f, 1.f - U), Color);
				Data.AddVertex(C + Side * (Half * W) - Face * (Fold * W), RightNormal, FVector2f(0.5f + 0.5f * W, 1.f - U), Color);
			}
			for (int32 Row = 0; Row + 1 < UE_ARRAY_COUNT(Stations); ++Row)
			{
				const bool bTip = Row + 2 == UE_ARRAY_COUNT(Stations); // the last station is a point: one triangle per side
				for (int32 Column = 0; Column < 2; ++Column)
				{
					const int32 A = First + Row * 3 + Column;
					if (bTip)
					{
						Data.AddTriangle(Section, A, First + (Row + 1) * 3 + 1, A + 1);
					}
					else
					{
						Data.AddTriangle(Section, A, A + 3, A + 1);
						Data.AddTriangle(Section, A + 1, A + 3, A + 4);
					}
				}
			}
		}
	}

	// --- L-system ---------------------------------------------------------------------------------------------

	struct FLSystemRule
	{
		TCHAR Symbol;
		float Probability;
		FString Replacement;
	};

	FString RewriteLSystem(const FString& Axiom, const TArray<FLSystemRule>& Rules, int32 Iterations, FRandomStream& Random)
	{
		FString Current = Axiom;
		for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
		{
			FString Next;
			Next.Reserve(Current.Len() * 4);
			for (const TCHAR Symbol : Current)
			{
				float Roll = Random.FRand();
				bool bReplaced = false;
				for (const FLSystemRule& Rule : Rules)
				{
					if (Rule.Symbol != Symbol)
					{
						continue;
					}
					if (Roll < Rule.Probability)
					{
						Next += Rule.Replacement;
						bReplaced = true;
						break;
					}
					Roll -= Rule.Probability;
				}
				if (!bReplaced)
				{
					Next.AppendChar(Symbol);
				}
			}
			Current = MoveTemp(Next);
		}
		return Current;
	}

	struct FTurtleParams
	{
		float Length = 150.f;       // cm, first F
		float Radius = 15.f;        // cm, first F
		float LengthScale = 0.78f;  // '
		float RadiusScale = 0.68f;  // !
		float BranchAngle = 32.f;   // &, degrees
		float RollAngle = 23.f;     // /, degrees
		float Jitter = 0.3f;        // relative angle noise
		float Phototropism = 0.12f; // bend towards the sky per segment
		float LeafRadius = 120.f;   // cm
		FLinearColor Bark = FLinearColor(0.075f, 0.065f, 0.052f); // grey-brown tropical bark (0.2 read as pale birch)
		FLinearColor Leaf = FLinearColor(0.06f, 0.14f, 0.03f);
		FLinearColor LeafAccent = FLinearColor(0.16f, 0.2f, 0.05f);
		float AccentAmount = 0.5f;
		// No branch grows straight: each F grows in a few steps, its heading wandering by up to Wobble degrees per step
		// (a random walk, pulled back up by phototropism); the whole plant leans by up to Lean degrees; the trunk's
		// foot flares out by RootFlare.
		float Wobble = 6.f;
		float Lean = 0.f;
		float RootFlare = 0.f;
		// Leaf masses as textured cards (AddLeafCards) rather than lumpy balls.
		bool bLeafCards = false;
		float CardSize = 140.f;    // cm
		FLinearColor CardTint = FLinearColor(0.5f, 0.5f, 0.5f);
		// Real leaves instead (AddLeafSpray, the near level of detail), in section SectionLeaves: Leaf / LeafAccent colours.
		bool b3DLeaves = false;
		float LeafSize = 22.f;     // cm, blade length
		float LeafWidth = 0.55f;   // blade width over length
		float RedChance = 0.f;
	};

	/**
	 * Turtle interpretation: F = grow a branch segment, & = pitch away from the parent axis, / = roll,
	 * ! = thinner, ' = shorter, [ ] = push / pop, L = leaf cluster. Remaining apices (A) become leaves.
	 */
	void InterpretLSystem(FOceanMeshData& Data, const FString& Program, const FTurtleParams& Params, FRandomStream& Random)
	{
		struct FState
		{
			FVector3f Position = FVector3f::ZeroVector;
			FQuat4f Orientation = FQuat4f::Identity;
			float Length = 0.f;
			float Radius = 0.f;
		};
		FState State;
		State.Length = Params.Length;
		State.Radius = Params.Radius;
		if (Params.Lean > 0.f)
		{
			const float Azimuth = Random.FRandRange(0.f, 2.f * UE_PI);
			State.Orientation = FQuat4f(FVector3f(FMath::Cos(Azimuth), FMath::Sin(Azimuth), 0.f),
				FMath::DegreesToRadians(Params.Lean * Random.FRandRange(0.4f, 1.f)));
		}
		bool bFoot = true; // the first segment drawn is the trunk's foot
		TArray<FState> Stack;
		auto Jittered =[&](float Degrees) { return FMath::DegreesToRadians(Degrees * (1.f + Random.FRandRange(-Params.Jitter, Params.Jitter))); };

		for (const TCHAR Symbol : Program)
		{
			switch (Symbol)
			{
			case TEXT('F'):
			{
				// Grown in a few steps (more on thick limbs), the heading wandering at each step and bending towards the
				// light. Each tube starts half a radius back along its own axis, so that consecutive tubes overlap at
				// the bends instead of opening a slit on the outer side.
				const int32 Steps = State.Radius > 8.f ? 4 : (State.Radius > 3.f ? 3 : 2);
				const float StepRadiusScale = FMath::Pow(0.85f, 1.f / Steps);
				for (int32 Step = 0; Step < Steps; ++Step)
				{
					const FVector3f Wander = FVector3f::CrossProduct(State.Orientation.RotateVector(FVector3f::UpVector), FVector3f(Random.GetUnitVector()));
					if (Wander.SizeSquared() > 1e-6f)
					{
						State.Orientation = FQuat4f(Wander.GetSafeNormal(), FMath::DegreesToRadians(Params.Wobble * Random.FRandRange(0.2f, 1.f))) * State.Orientation;
					}
					const FVector3f Heading = State.Orientation.RotateVector(FVector3f::UpVector);
					const FVector3f End = State.Position + Heading * (State.Length / Steps);
					const float EndRadius = State.Radius * StepRadiusScale;
					float StartRadius = State.Radius;
					FVector3f Start = State.Position - Heading * (State.Radius * 0.5f);
					if (bFoot)
					{
						// The trunk's foot: flared, and sunk a little into the ground.
						StartRadius *= 1.f + Params.RootFlare;
						Start = State.Position - FVector3f(0.f, 0.f, 30.f);
						bFoot = false;
					}
					AddTube(Data, SectionBark, Start, End, StartRadius, EndRadius, State.Radius > 8.f ? 8 : 5, Params.Bark, Params.Bark * 1.1f);
					State.Position = End;
					State.Radius = EndRadius;
					// Phototropism: branches bend towards the light.
					const FVector3f Axis = FVector3f::CrossProduct(Heading, FVector3f::UpVector);
					if (Axis.SizeSquared() > 1e-6f)
					{
						State.Orientation = FQuat4f(Axis.GetSafeNormal(), Params.Phototropism / Steps * Axis.Size()) * State.Orientation;
					}
				}
				break;
			}
			case TEXT('&'):
				State.Orientation = State.Orientation * FQuat4f(FVector3f::ForwardVector, Jittered(Params.BranchAngle));
				break;
			case TEXT('/'):
				State.Orientation = State.Orientation * FQuat4f(FVector3f::UpVector, Jittered(Params.RollAngle));
				break;
			case TEXT('!'):
				State.Radius *= Params.RadiusScale;
				break;
			case TEXT('\''):
				State.Length *= Params.LengthScale;
				break;
			case TEXT('['):
				Stack.Push(State);
				break;
			case TEXT(']'):
				if (Stack.Num() > 0)
				{
					State = Stack.Pop();
				}
				break;
			case TEXT('L'):
			case TEXT('A'):
			{
					const float Radius = Params.LeafRadius * Random.FRandRange(0.75f, 1.2f);
				// Its own stream: the branches that follow grow the same whichever way the leaves are drawn.
				FRandomStream LeafRandom(int32(Random.GetUnsignedInt() & 0x7fffffff));
				const FVector3f LeafCenter = State.Position + FVector3f(0.f, 0.f, Radius * 0.3f);
				if (Params.b3DLeaves)
				{
					AddLeafSpray(Data, SectionLeaves, LeafCenter, Radius, Params.LeafSize, Params.LeafWidth, Params.Leaf, Params.LeafAccent,
						Params.AccentAmount, Params.RedChance, LeafRandom);
				}
				else if (Params.bLeafCards)
				{
					AddLeafCards(Data, SectionFoliage, LeafCenter, Radius, Params.CardSize, LeafRandom, Params.CardTint);
				}
				else
				{
					AddLeafCluster(Data, LeafCenter, Radius, LeafRandom, Params.Leaf, Params.LeafAccent, Params.AccentAmount);
				}
				break;
			}
			default:
				break;
			}
		}
	}
}

namespace OceanFlora
{
	/** A point of a palm frond's rachis and its frame: along the rachis, across the frond, up from its upper face. */
	struct FPalmRachisPoint { FVector3f Position, Along, Side, Up; };

	/**
	 * One coconut frond along its rachis (At(S), S = 0 at the base, 1 at the tip): the rachis and its leaflets, at the
	 * detail of MakeCoconutPalm's levels. bLying: fallen on the ground, the leaflets spread nearly flat on either side.
	 */
	void AddPalmFrond(FOceanMeshData& Data, TFunctionRef<FPalmRachisPoint(float)> At, int32 Detail, float Length, float Age, bool bDead,
		float Shade, int32 LeafSeed, bool bLying)
	{
		// The frond as it grows. A tapering rachis, bare over its first fifth (the petiole), then
		// leaflets on both sides, ~85 a side (Cocos nucifera carries 200-250): narrow strips (2-5 cm wide, up to ~a
		// fifth of the frond long, shortest at the base and the tip), pointing forward at 50-60 degrees from the
		// rachis, hanging below it in a V (the frond's cross-section) and curling down under their own weight, more
		// on old fronds. Each folded along its midrib into a shallow inverted V (reduplicate leaflets), so they catch
		// the light on one half and not the other. Some torn short, a few missing, some with dry brown tips.
		// Middle level: a third as many leaflets, as wide as the ones they replace, flat, in two segments; far level: a
		// dozen broad strips a side (every level has the same sections and colours: no jump from one to the next).
		// Colours are linear albedo (the mesh is built with this section as its colour section): a deep green that
		// yellows with age; the rachis yellow-green.
		FRandomStream Leaf(LeafSeed);
		const bool bNear = Detail == 0;
		const FLinearColor Blade = (bDead ? FLinearColor(0.17f, 0.11f, 0.05f)
			: FMath::Lerp(FLinearColor(0.045f, 0.09f, 0.018f), FLinearColor(0.14f, 0.125f, 0.03f), FMath::Pow(Age, 3.f))) * Shade;
		const FLinearColor Dry(0.2f, 0.14f, 0.06f);
		const FLinearColor Stalk = bDead ? FLinearColor(0.2f, 0.14f, 0.07f) : FMath::Lerp(FLinearColor(0.15f, 0.16f, 0.05f), FLinearColor(0.24f, 0.2f, 0.06f), Age);
		auto RachisRadius = [](float S) { return FMath::Lerp(4.5f, 0.6f, FMath::Pow(S, 0.8f)); };
		// Wind data (Shaders/Flora.ush, FloraWind): the frond bends as a beam, more towards its tip (metres along the
		// rachis), and each leaflet flaps as a whole about where it joins the rachis (metres along the leaflet), each
		// with its own phase. The rachis tube's V is already metres along it.
		FRandomStream Phases(LeafSeed * 3 + 7);
		const float FrondPhase = Phases.FRand();
		const int32 RachisFirst = Data.Vertices.Num();
		AddBarkTube(Data, SectionFoliage, [&](float T) { return At(T).Position; }, RachisRadius, bNear ? 24 : (Detail == 1 ? 10 : 5), bNear ? 6 : (Detail == 1 ? 4 : 3), Stalk);
		for (int32 Index = RachisFirst; Index < Data.Vertices.Num(); ++Index)
		{
			Data.Vertices[Index].Wind = FVector4f(Data.Vertices[Index].UV.Y, 0.f, FrondPhase, 0.f);
		}

		const int32 PerSide = bNear ? 85 : (Detail == 1 ? 30 : 12);
		const float MaxLeaflet = 0.2f * Length;
		const float WidthScale = bNear ? 1.f : (Detail == 1 ? 2.4f : 5.5f);
		const int32 Segments = bNear ? 4 : 2;
		const float Fold = bNear ? 0.22f : 0.f;
		for (int32 SideSign = -1; SideSign <= 1; SideSign += 2)
		{
			for (int32 K = 0; K < PerSide; ++K)
			{
				const float T = (K + 0.5f + 0.3f * (Leaf.FRand() - 0.5f)) / PerSide;
				const float Torn = Leaf.FRand();
				const float Brown = Leaf.FRand() < 0.25f + 0.5f * Age ? Leaf.FRandRange(0.3f, 0.9f) : 0.f;
				const float Jitter = Leaf.FRandRange(-1.f, 1.f);
				const float JitterDroop = Leaf.FRandRange(-1.f, 1.f);
				// Each leaflet a slightly different green: some yellower, some bluer, lighter or darker.
				const FLinearColor LeafletBlade = Blade * FMath::Lerp(FLinearColor(0.9f, 1.f, 1.12f), FLinearColor(1.15f, 1.06f, 0.8f), Leaf.FRand())
					* Leaf.FRandRange(0.85f, 1.12f);
				if (Torn < 0.03f)
				{
					continue; // torn off
				}
				const float S = 0.2f + 0.79f * T;
				const FPalmRachisPoint R = At(S);
				const float Profile = FMath::Max(0.22f, FMath::Sin(UE_PI * FMath::Pow(FMath::Clamp(0.12f + 0.88f * T, 0.f, 1.f), 0.75f)));
				const float L = MaxLeaflet * Profile * (Torn < 0.09f ? FMath::Lerp(0.4f, 0.7f, Torn / 0.09f) : 1.f);
				const float W = (2.f + 2.6f * Profile) * WidthScale;
				// Pointing forward (more so towards the tip), hanging below the rachis in the frond's V.
				const float Forward = FMath::DegreesToRadians(32.f + 18.f * T + 6.f * Jitter);
				const float Hang = bLying ? FMath::DegreesToRadians(3.f + 3.f * JitterDroop) // spread flat on the sand
					: FMath::DegreesToRadians((bDead ? 55.f : FMath::Lerp(20.f, 42.f, Age)) + 8.f * JitterDroop);
				FVector3f Direction = (R.Side * float(SideSign) * FMath::Cos(Forward) + R.Along * FMath::Sin(Forward)).GetSafeNormal();
				Direction = (Direction * FMath::Cos(Hang) - R.Up * FMath::Sin(Hang)).GetSafeNormal();
				const FVector3f Across = (R.Along - Direction * FVector3f::DotProduct(R.Along, Direction)).GetSafeNormal();
				FVector3f Normal = FVector3f::CrossProduct(Across, Direction).GetSafeNormal();
				if (FVector3f::DotProduct(Normal, R.Up) < 0.f)
				{
					Normal = -Normal;
				}
				const float Curl = bLying ? 0.f : (bDead ? 1.3f : FMath::Lerp(0.35f, 1.1f, Age)) * (L / 90.f);
				FVector3f Point = R.Position + R.Side * float(SideSign) * RachisRadius(S);
				int32 Previous[3] = { INDEX_NONE, INDEX_NONE, INDEX_NONE };
				const float LeafletPhase = Phases.FRand();
				for (int32 J = 0; J <= Segments; ++J)
				{
					const float U = float(J) / Segments;
					Data.CurrentWind = FVector4f(S * Length * 0.01f, U * L * 0.01f, FrondPhase, LeafletPhase);
					// Narrow at the base, widest at a tenth of its length, tapering to a point.
					const float Width = W * (U < 0.1f ? FMath::Lerp(0.35f, 1.f, U / 0.1f) : 1.f - 0.95f * FMath::Pow((U - 0.1f) / 0.9f, 2.2f));
					const FLinearColor Color = FMath::Lerp(LeafletBlade, Dry, Brown * FMath::SmoothStep(0.65f, 1.f, U));
					const FVector3f Half = Across * (0.5f * Width);
					const FVector3f Drop = Normal * (Fold * Width);
					if (Fold > 0.f)
					{
						const int32 Row[3] = {
							Data.AddVertex(Point - Half - Drop, (Normal - Across * 0.45f).GetSafeNormal(), FVector2f(0.f, U), Color),
							Data.AddVertex(Point, Normal, FVector2f(0.5f, U), Color),
							Data.AddVertex(Point + Half - Drop, (Normal + Across * 0.45f).GetSafeNormal(), FVector2f(1.f, U), Color) };
						if (J > 0)
						{
							for (int32 Column = 0; Column < 2; ++Column)
							{
								Data.AddTriangle(SectionFoliage, Previous[Column], Row[Column], Previous[Column + 1]);
								Data.AddTriangle(SectionFoliage, Previous[Column + 1], Row[Column], Row[Column + 1]);
							}
						}
						FMemory::Memcpy(Previous, Row, sizeof(Row));
					}
					else
					{
						const int32 Row[2] = {
							Data.AddVertex(Point - Half, Normal, FVector2f(0.f, U), Color),
							Data.AddVertex(Point + Half, Normal, FVector2f(1.f, U), Color) };
						if (J > 0)
						{
							Data.AddTriangle(SectionFoliage, Previous[0], Row[0], Previous[1]);
							Data.AddTriangle(SectionFoliage, Previous[1], Row[0], Row[1]);
						}
						Previous[0] = Row[0];
						Previous[1] = Row[1];
					}
					// Curling down under its weight along its length.
					const FVector3f Bent = (Direction - FVector3f::UpVector * (Curl * (U + 0.5f / Segments))).GetSafeNormal();
					Point += Bent * (L / Segments);
				}
			}
		}
		Data.CurrentWind = FVector4f(0.f, 0.f, 0.f, 0.f);
	}

	FOceanMeshData MakeCoconutPalm(int32 Seed, int32 Detail)
	{
		FRandomStream Random(Seed);
		FOceanMeshData Data(3); // bark, leaflets and rachises, coconuts

		// Trunk: leans (towards +X; instances are rotated to lean seawards) then curves back up, slimmer at the top,
		// with the ring scars of fallen fronds.
		// Coconut palms lean out towards the light over the beach, some strongly, the bend mostly near the foot (Bend);
		// the trunk also snakes a little from side to side, never a straight pole.
		const float Height = Random.FRandRange(750.f, 1150.f);
		const float Lean = Random.FRandRange(0.15f, 0.6f);
		const float Bend = Random.FRandRange(1.6f, 3.f);
		const float Sway = Random.FRandRange(-1.f, 1.f) * 0.05f * Height;
		const float Waves = Random.FRandRange(1.f, 2.f);
		const float Phase = Random.FRandRange(0.f, 2.f * UE_PI);
		auto TrunkPoint = [&](float T)
		{
			const float Out = Lean * Height * (1.f - FMath::Pow(1.f - T, Bend)) * 0.8f
				+ 0.02f * Height * (FMath::Sin(2.f * UE_PI * T + Phase) - FMath::Sin(Phase));
			const float Side = Sway * FMath::Sin(UE_PI * T * Waves);
			return FVector3f(Out, Side, Height * T - 20.f); // base slightly sunk in the sand
		};
		// The palm's own details, from their own random stream so that the crown stays the same.
		FRandomStream Extra(Seed * 53 + 1);
		// Bark: M_PalmBark draws the ring scars, fissures and lichen procedurally from the bark coordinates (V = metres along
		// the trunk); the vertex colour stays neutral. Slimmer at the top, a swollen foot (the bole), and a diameter that
		// varies a little along the trunk with the good and lean years.
		const FLinearColor TrunkColor(0.27f, 0.23f, 0.18f);
		const float Girth1 = Extra.FRandRange(0.f, 2.f * UE_PI), Girth2 = Extra.FRandRange(0.f, 2.f * UE_PI);
		auto TrunkRadius = [&](float T)
		{
			const float Metres = T * Height * 0.01f;
			return FMath::Lerp(21.f, 13.5f, T) * (1.f + 0.45f * FMath::Exp(-T * Height / 60.f))
				* (1.f + 0.045f * FMath::Sin(1.3f * Metres + Girth1) + 0.025f * FMath::Sin(3.7f * Metres + Girth2));
		};
		AddBarkTube(Data, SectionBark, TrunkPoint, TrunkRadius, Detail == 0 ? 56 : (Detail == 1 ? 24 : 12), Detail == 0 ? 16 : (Detail == 1 ? 12 : 8),
			TrunkColor);
		const FVector3f Top = TrunkPoint(1.f);

		// The root mass at the foot: a dense collar of short dark roots springing from the bole and diving straight into the
		// sand (a coconut palm has no taproot), the older ones greyed.
		if (Detail < 2)
		{
			const int32 Roots = Detail == 0 ? 64 : 20;
			for (int32 Root = 0; Root < Roots; ++Root)
			{
				const float Angle = 2.f * UE_PI * (Root + Extra.FRand()) / Roots;
				const FVector3f Out(FMath::Cos(Angle), FMath::Sin(Angle), 0.f);
				const float Above = Extra.FRandRange(0.f, 14.f); // cm above the sand where it leaves the trunk
				const float T0 = (Above + 20.f) / Height;
				const FVector3f Start = TrunkPoint(T0) + Out * (TrunkRadius(T0) * 0.92f);
				const float Length = Extra.FRandRange(9.f, 22.f); // short: they dive into the sand close to the trunk
				const float Dip = FMath::DegreesToRadians(Extra.FRandRange(45.f, 75.f));
				const float Radius = Extra.FRandRange(1.3f, 2.2f) * (Detail == 0 ? 1.f : 1.4f);
				const FLinearColor Color = FMath::Lerp(FLinearColor(0.09f, 0.065f, 0.045f), FLinearColor(0.12f, 0.11f, 0.09f), Extra.FRand()); // dark brown, greyed with age (orange read as straw)
				AddBarkTube(Data, SectionNuts,
					[&](float T) { return Start + Out * (Length * T * FMath::Cos(Dip)) - FVector3f(0.f, 0.f, Length * T * FMath::Sin(Dip) * (0.6f + 0.4f * T)); },
					[&](float T) { return Radius * (1.f - 0.4f * T); }, 3, Detail == 0 ? 5 : 3, Color);
			}
		}

		// The fibrous sheath at the crown's base, where the fronds spring from: a brown mat wrapping the top of the trunk.
		const FVector3f TopDirection = (Top - TrunkPoint(0.97f)).GetSafeNormal();
		AddBarkTube(Data, SectionNuts, [&](float T) { return Top + TopDirection * FMath::Lerp(-75.f, 15.f, T); },
			[&](float T) { return TrunkRadius(1.f) * (1.05f + 0.45f * FMath::Sin(UE_PI * FMath::Min(T * 1.15f, 1.f))); },
			Detail == 0 ? 6 : 3, Detail == 0 ? 14 : 8, FLinearColor(0.16f, 0.1f, 0.05f));

		// Coconuts: one to three bunches hanging below the crown on their stalks, nuts of one bunch of one ripeness: green,
		// yellow-orange or brown.
		const int32 Bunches = Extra.RandRange(1, 3);
		for (int32 Bunch = 0; Bunch < Bunches; ++Bunch)
		{
			const float Angle = Extra.FRandRange(0.f, 2.f * UE_PI);
			const FVector3f Out(FMath::Cos(Angle), FMath::Sin(Angle), 0.f);
			const FVector3f Center = Top + Out * 30.f - FVector3f(0.f, 0.f, Extra.FRandRange(30.f, 50.f));
			const float Ripeness = Extra.FRand();
			const FLinearColor Green = Ripeness < 0.5f ? FLinearColor(0.09f, 0.13f, 0.03f)
				: (Ripeness < 0.8f ? FLinearColor(0.3f, 0.19f, 0.04f) : FLinearColor(0.17f, 0.1f, 0.045f));
			AddBarkTube(Data, SectionNuts, [&](float T) { return FMath::Lerp(Top + Out * 10.f - FVector3f(0.f, 0.f, 15.f), Center, T); },
				[](float T) { return 2.f; }, 2, 4, FLinearColor(0.3f, 0.25f, 0.08f));
			const int32 Nuts = Extra.RandRange(4, 10);
			for (int32 Nut = 0; Nut < Nuts; ++Nut)
			{
				const FVector3f Offset = FVector3f(Extra.GetUnitVector()) * FVector3f(16.f, 16.f, 10.f) * Extra.FRandRange(0.4f, 1.f);
				FRandomStream NutRandom(Seed * 31 + Bunch * 17 + Nut);
				AddLeafCluster(Data, Center + Offset, Extra.FRandRange(9.f, 12.f), NutRandom, Green, Green * 1.25f, 0.4f, SectionNuts);
			}
		}

		// Fronds: an arching rachis with leaflets hanging on both sides. The crown is a sphere, not a flat star: in a
		// spiral (golden angle) the young fronds rise almost straight up in its middle, the mature ones arch out, the old
		// ones droop below the horizontal and yellow, the dead ones hang along the trunk; each twisted about its rachis.
		const int32 Fronds = Random.RandRange(22, 30);
		const float Spiral = Random.FRandRange(0.f, 2.f * UE_PI);
		for (int32 Frond = 0; Frond < Fronds; ++Frond)
		{
			const float Azimuth = Spiral + Frond * 2.39996f + Random.FRandRange(-0.15f, 0.15f);
			const float Age = FMath::Clamp((Frond + Random.FRand()) / Fronds, 0.f, 1.f); // 0 = young, upright; 1 = old
			const bool bDead = Age > 0.9f && Random.FRand() < 0.7f;
			const float Length = Random.FRandRange(380.f, 500.f) * FMath::Lerp(0.6f, 1.f, FMath::SmoothStep(0.f, 0.35f, Age));
			const float StartPitch = FMath::DegreesToRadians(bDead ? -70.f : FMath::Lerp(78.f, -22.f, Age));
			const float Droop = FMath::DegreesToRadians(bDead ? 25.f : FMath::Lerp(35.f, 110.f, Age));
			const float Roll = FMath::DegreesToRadians(Random.FRandRange(-28.f, 28.f));
			const FVector3f Outward(FMath::Cos(Azimuth), FMath::Sin(Azimuth), 0.f);
			const float Shade = Random.FRandRange(0.85f, 1.15f);

			// The rachis, finely sampled once for every level of detail (they then share the same crown): position and
			// frame (along, across the frond, up from its upper face).
			constexpr int32 Fine = 48;
			FPalmRachisPoint Rachis[Fine + 1];
			// Older fronds spring from lower on the crown.
			FVector3f Position = Top + Outward * 8.f - FVector3f(0.f, 0.f, 35.f * Age);
			for (int32 Step = 0; Step <= Fine; ++Step)
			{
				const float S = float(Step) / Fine;
				const float Pitch = StartPitch - Droop * FMath::Pow(S, 1.5f);
				const FVector3f Along = (Outward * FMath::Cos(Pitch) + FVector3f(0.f, 0.f, FMath::Sin(Pitch))).GetSafeNormal();
				const FVector3f Flat = FVector3f::CrossProduct(Along, FVector3f::UpVector).GetSafeNormal();
				const FVector3f FlatUp = FVector3f::CrossProduct(Flat, Along).GetSafeNormal();
				const float Twist = Roll * (0.4f + 0.6f * S);
				const FVector3f Side = Flat * FMath::Cos(Twist) + FlatUp * FMath::Sin(Twist);
				Rachis[Step] = { Position, Along, Side, FVector3f::CrossProduct(Side, Along).GetSafeNormal() };
				Position += Along * (Length / Fine);
			}
			auto At = [&Rachis](float S)
			{
				const float X = FMath::Clamp(S, 0.f, 1.f) * Fine;
				const int32 I = FMath::Min(int32(X), Fine - 1);
				const float F = X - I;
				const FPalmRachisPoint& A = Rachis[I];
				const FPalmRachisPoint& B = Rachis[I + 1];
				return FPalmRachisPoint{ FMath::Lerp(A.Position, B.Position, F), FMath::Lerp(A.Along, B.Along, F).GetSafeNormal(),
					FMath::Lerp(A.Side, B.Side, F).GetSafeNormal(), FMath::Lerp(A.Up, B.Up, F).GetSafeNormal() };
			};

			AddPalmFrond(Data, At, Detail, Length, Age, bDead, Shade, Seed * 7919 + Frond * 131, false);
		}
		return Data;
	}

	FOceanMeshData MakeBroadleafTree(int32 Seed, bool b3DLeaves, float LeafScale)
	{
		FRandomStream Random(Seed);
		FOceanMeshData Data(3);
		// The broadleaf trees behind Caribbean beaches (tropical almond, seaside mahoe, sea grape trees) fork low into
		// a few crooked limbs that spread wide before turning up: a broad, umbrella-like crown on a short, leaning,
		// thick trunk. Apices branch in two or three, with leaves along the upper limbs too, so the crown is one dense,
		// lumpy mass rather than a few balls on sticks.
		const TArray<FLSystemRule> Rules = {
			{ TEXT('A'), 0.6f, TEXT("F!'[&FLA]/////[&FLA]///////[&FLA]") },
			{ TEXT('A'), 0.3f, TEXT("F!'[&FLA]////////[&FLA]") },
			{ TEXT('A'), 0.1f, TEXT("F!'L") },
		};
		const FString Program = RewriteLSystem(Random.FRand() < 0.5f ? TEXT("FA") : TEXT("F/[&A]////////[&A]"), Rules, 3, Random);
		FTurtleParams Params;
		Params.Length = Random.FRandRange(150.f, 210.f);
		Params.Radius = Random.FRandRange(18.f, 26.f);
		Params.BranchAngle = Random.FRandRange(42.f, 58.f);
		Params.Phototropism = 0.07f; // limbs spread before they turn up
		// Trunks lean, wander and flare at the foot; limbs twist on their way out.
		Params.Lean = 16.f;
		Params.Wobble = 11.f;
		Params.RootFlare = 0.8f;
		Params.LeafRadius = Random.FRandRange(130.f, 170.f);
		Params.Leaf = FLinearColor(0.03f, 0.08f, 0.018f);
		Params.LeafAccent = FLinearColor(0.07f, 0.12f, 0.025f);
		// Leaf cards (M_LeafCards_Broadleaf): leaves ~30 cm, like the tropical almond's; each tree its own shade.
		Params.bLeafCards = true;
		Params.CardSize = Random.FRandRange(125.f, 150.f);
		Params.CardTint = FLinearColor(0.5f * Random.FRandRange(0.9f, 1.05f), 0.5f * Random.FRandRange(0.92f, 1.08f), 0.5f * Random.FRandRange(0.85f, 1.f));
		// Near: real leaves ~25 cm, each tree its own green (from the card tint), a few turning yellow.
		Params.b3DLeaves = b3DLeaves;
		Params.LeafSize = Random.FRandRange(20.f, 28.f) * LeafScale;
		Params.LeafWidth = 0.5f;
		Params.Leaf = FLinearColor(0.05f, 0.12f, 0.025f) * (Params.CardTint.G / 0.5f);
		Params.LeafAccent = FLinearColor(0.14f, 0.16f, 0.03f);
		Params.AccentAmount = 0.3f;
		InterpretLSystem(Data, Program, Params, Random);
		return Data;
	}

	FOceanMeshData MakeTropicalAlmond(int32 Seed, bool b3DLeaves, float LeafScale)
	{
		// Terminalia catappa, the tropical almond (amandier-pays) of every Caribbean beach: a straight trunk with tiers of
		// near-horizontal limbs (a pagoda), big leaves in rosettes at the twig tips, some of them turned red.
		FRandomStream Random(Seed);
		FOceanMeshData Data(3);
		const TArray<FLSystemRule> Rules = {
			{ TEXT('A'), 0.8f, TEXT("FF'![&&B]///[&&B]///[&&B]///[&&B]A") },
			{ TEXT('A'), 0.2f, TEXT("FF'![&&B]////[&&B]////[&&B]L") },
			{ TEXT('B'), 0.7f, TEXT("F'L[&FL]////[&FL]") },
			{ TEXT('B'), 0.3f, TEXT("F'[&FL]FL") },
		};
		const FString Program = RewriteLSystem(TEXT("FA"), Rules, 3, Random);
		FTurtleParams Params;
		Params.Length = Random.FRandRange(110.f, 150.f);
		Params.Radius = Random.FRandRange(16.f, 22.f);
		Params.LengthScale = 0.86f;
		Params.BranchAngle = Random.FRandRange(38.f, 44.f); // twice that from the trunk: near-horizontal tiers
		Params.RollAngle = 22.f;
		Params.Phototropism = 0.025f;
		Params.Lean = 6.f;
		Params.Wobble = 6.f;
		Params.RootFlare = 0.6f;
		Params.LeafRadius = Random.FRandRange(80.f, 110.f);
		Params.bLeafCards = true;
		Params.CardSize = Random.FRandRange(110.f, 135.f);
		Params.CardTint = FLinearColor(0.52f * Random.FRandRange(0.95f, 1.1f), 0.48f * Random.FRandRange(0.95f, 1.05f), 0.42f);
		Params.b3DLeaves = b3DLeaves;
		Params.LeafSize = Random.FRandRange(26.f, 32.f) * LeafScale;
		Params.LeafWidth = 0.55f;
		Params.Leaf = FLinearColor(0.06f, 0.13f, 0.025f);
		Params.LeafAccent = FLinearColor(0.16f, 0.15f, 0.03f);
		Params.AccentAmount = 0.35f;
		Params.RedChance = Random.FRandRange(0.06f, 0.2f);
		InterpretLSystem(Data, Program, Params, Random);
		return Data;
	}

	FOceanMeshData MakeSeaGrape(int32 Seed, bool b3DLeaves, float LeafScale)
	{
		FRandomStream Random(Seed);
		FOceanMeshData Data(3);
		// Coccoloba uvifera: a sprawling shrub of many low, spreading stems with big round leaves,
		// some of them turning red.
		const TArray<FLSystemRule> Rules = {
			{ TEXT('A'), 0.7f, TEXT("'[&FLA]///////[&FLA]//////[&FAL]") },
			{ TEXT('A'), 0.3f, TEXT("'[&FL]////[&FLA]") },
		};
		const FString Program = RewriteLSystem(TEXT("A"), Rules, 2, Random);
		FTurtleParams Params;
		Params.Length = Random.FRandRange(70.f, 100.f);
		Params.Radius = 5.f;
		Params.LengthScale = 0.85f;
		Params.BranchAngle = 55.f;
		Params.RollAngle = 40.f;
		Params.Phototropism = 0.05f;
		Params.Wobble = 14.f; // the sea grape's stems twist and sprawl
		Params.LeafRadius = Random.FRandRange(55.f, 80.f);
		Params.Bark = FLinearColor(0.07f, 0.06f, 0.05f);
		Params.Leaf = FLinearColor(0.08f, 0.15f, 0.04f);
		Params.LeafAccent = FLinearColor(0.24f, 0.11f, 0.05f); // red-veined old leaves
		Params.AccentAmount = 0.35f;
		// Leaf cards (M_LeafCards_SeaGrape): round leaves ~20 cm along zigzag stems, a few turning red.
		Params.bLeafCards = true;
		Params.CardSize = Random.FRandRange(95.f, 115.f);
		Params.CardTint = FLinearColor(0.4f * Random.FRandRange(0.92f, 1.05f), 0.44f * Random.FRandRange(0.95f, 1.05f), 0.48f); // less yellow than the render
		// Near: the sea grape's big round leathery leaves, ~18 cm, a few red.
		Params.b3DLeaves = b3DLeaves;
		Params.LeafSize = 17.f * LeafScale;
		Params.LeafWidth = 0.95f;
		Params.RedChance = 0.08f;
		InterpretLSystem(Data, Program, Params, Random);
		return Data;
	}

	FOceanMeshData MakeCoastalBush(int32 Seed, bool b3DLeaves, float LeafScale)
	{
		FRandomStream Random(Seed);
		FOceanMeshData Data(3);
		// The evergreen scrub of the coastal forest edge (seaside mahoe, bay cedar, young tropical almonds): three
		// stems from the ground, each forking into leafy masses, so the bush is a mound of foliage from knee height
		// up, ~2-2.5 m tall.
		const TArray<FLSystemRule> Rules = {
			{ TEXT('A'), 0.6f, TEXT("F'[&FLA]////[&FLA]////[&LA]") },
			{ TEXT('A'), 0.4f, TEXT("F'[&FLA]//////[&FL]") },
		};
		const FString Program = RewriteLSystem(TEXT("[&A]/////[&A]/////[&A]"), Rules, 2, Random);
		FTurtleParams Params;
		Params.Length = Random.FRandRange(55.f, 75.f);
		Params.Radius = 4.f;
		Params.LengthScale = 0.85f;
		Params.BranchAngle = 35.f;
		Params.RollAngle = 24.f;
		Params.Phototropism = 0.1f;
		Params.Wobble = 11.f;
		Params.LeafRadius = Random.FRandRange(65.f, 90.f);
		Params.Bark = FLinearColor(0.065f, 0.058f, 0.048f);
		Params.bLeafCards = true;
		Params.CardSize = Random.FRandRange(85.f, 105.f);
		Params.CardTint = FLinearColor(0.5f * Random.FRandRange(0.85f, 1.f), 0.5f * Random.FRandRange(0.9f, 1.05f), 0.5f * Random.FRandRange(0.85f, 1.f));
		Params.b3DLeaves = b3DLeaves;
		Params.LeafSize = Random.FRandRange(9.f, 14.f) * LeafScale;
		Params.LeafWidth = 0.45f;
		Params.Leaf = FLinearColor(0.05f, 0.11f, 0.03f) * (Params.CardTint.G / 0.5f);
		Params.LeafAccent = FLinearColor(0.1f, 0.14f, 0.04f);
		InterpretLSystem(Data, Program, Params, Random);
		return Data;
	}

	FOceanMeshData MakeFallenFrond(int32 Seed, int32 Detail)
	{
		// A dead coconut frond lying under its palm, built like the fronds of MakeCoconutPalm (AddPalmFrond, same levels of
		// detail): the rachis laid along +X from its base, gently bowed sideways, a little off the ground at its thick base,
		// the leaflets spread flat on the sand on either side. Straw-brown to grey-brown, in section 1 (vertex colours).
		FRandomStream Random(Seed);
		FOceanMeshData Data(2);
		const float Length = Random.FRandRange(350.f, 470.f);
		const float Bow = Random.FRandRange(-0.3f, 0.3f);
		const float Shade = Random.FRandRange(0.8f, 1.25f);
		constexpr int32 Fine = 24;
		FPalmRachisPoint Rachis[Fine + 1];
		FVector3f Position = FVector3f::ZeroVector;
		for (int32 Step = 0; Step <= Fine; ++Step)
		{
			const float S = float(Step) / Fine;
			const float Angle = Bow * S;
			const FVector3f Along(FMath::Cos(Angle), FMath::Sin(Angle), 0.f);
			const FVector3f Side(Along.Y, -Along.X, 0.f); // so that Up = Side x Along points up
			Rachis[Step] = { Position + FVector3f(0.f, 0.f, FMath::Lerp(12.f, 2.f, S)), Along, Side, FVector3f::UpVector };
			Position += Along * (Length / Fine);
		}
		auto At = [&Rachis](float S)
		{
			const float X = FMath::Clamp(S, 0.f, 1.f) * Fine;
			const int32 I = FMath::Min(int32(X), Fine - 1);
			const float F = X - I;
			return FPalmRachisPoint{ FMath::Lerp(Rachis[I].Position, Rachis[I + 1].Position, F), FMath::Lerp(Rachis[I].Along, Rachis[I + 1].Along, F).GetSafeNormal(),
				FMath::Lerp(Rachis[I].Side, Rachis[I + 1].Side, F).GetSafeNormal(), FVector3f::UpVector };
		};
		AddPalmFrond(Data, At, Detail, Length, 1.f, true, Shade, Seed * 31 + 7, true);
		return Data;
	}

	FOceanMeshData MakeCoconut(int32 Seed)
	{
		// A fallen coconut: an ovoid husk ~28 cm long with a rounded triangular cross-section, resting on its side.
		// Fresh green, ripening yellow-brown or dry brown (M_Bark, vertex colour).
		FRandomStream Random(Seed);
		FOceanMeshData Data(1);
		static TArray<FVector3f> Sphere;
		static TArray<int32> SphereTriangles;
		if (Sphere.Num() == 0)
		{
			MakeIcosphere(2, Sphere, SphereTriangles);
		}
		const FLinearColor Husks[3] = { FLinearColor(0.11f, 0.15f, 0.035f), FLinearColor(0.32f, 0.21f, 0.06f), FLinearColor(0.15f, 0.09f, 0.045f) };
		const FLinearColor Husk = Husks[Random.RandRange(0, 2)] * Random.FRandRange(0.85f, 1.15f);
		const float Radius = Random.FRandRange(11.f, 13.f);
		for (const FVector3f& Direction : Sphere)
		{
			const float Ridge = 1.f + 0.08f * FMath::Cos(3.f * FMath::Atan2(Direction.Z, Direction.Y));
			const FVector3f Position(Direction.X * Radius * 1.2f, Direction.Y * Radius * Ridge, Direction.Z * Radius * Ridge * 0.92f + Radius * 0.85f);
			Data.AddVertex(Position, Direction, FVector2f(Direction.X, Direction.Y), Husk * FMath::Lerp(0.8f, 1.05f, 0.5f + 0.5f * Direction.Z));
		}
		for (int32 I = 0; I < SphereTriangles.Num(); I += 3)
		{
			Data.AddTriangle(0, SphereTriangles[I], SphereTriangles[I + 2], SphereTriangles[I + 1]);
		}
		return Data;
	}

	FOceanMeshData MakeGrassTuft(int32 Seed, float SpanCm, float BaseV)
	{
		// A tuft of grass: three crossed cards showing a tuft of Infinigen grass seen from the side
		// (Scripts/infinigen/render_grass_tuft.py, the whole image on each card: SpanCm square, the tuft's base at
		// BaseV), leaning a little, each card its own shade (M_GrassTuft multiplies the render by the vertex colour).
		FRandomStream Random(Seed);
		FOceanMeshData Data(1);
		const float Below = (1.f - BaseV) * SpanCm; // the image below the tuft's base goes underground
		const FVector3f Lean(Random.FRandRange(-0.12f, 0.12f) * SpanCm, Random.FRandRange(-0.12f, 0.12f) * SpanCm, 0.f);
		const float Start = Random.FRandRange(0.f, UE_PI);
		for (int32 Card = 0; Card < 3; ++Card)
		{
			const float Angle = Start + Card * UE_PI / 3.f;
			const FVector3f Across(FMath::Cos(Angle) * SpanCm * 0.5f, FMath::Sin(Angle) * SpanCm * 0.5f, 0.f);
			const FVector3f Normal = FVector3f(-Across.Y, Across.X, 0.f).GetSafeNormal() * 0.3f + FVector3f::UpVector * 0.7f;
			const FLinearColor Color = FLinearColor(0.6f, 0.6f, 0.6f) * Random.FRandRange(0.85f, 1.15f);
			const FVector3f Bottom(0.f, 0.f, -Below);
			const FVector3f Top = FVector3f(0.f, 0.f, SpanCm - Below) + Lean;
			const int32 First = Data.Vertices.Num();
			Data.AddVertex(Bottom - Across, Normal.GetSafeNormal(), FVector2f(0.f, 1.f), Color);
			Data.AddVertex(Bottom + Across, Normal.GetSafeNormal(), FVector2f(1.f, 1.f), Color);
			Data.AddVertex(Top + Across, Normal.GetSafeNormal(), FVector2f(1.f, 0.f), Color);
			Data.AddVertex(Top - Across, Normal.GetSafeNormal(), FVector2f(0.f, 0.f), Color);
			Data.AddTriangle(0, First, First + 1, First + 2);
			Data.AddTriangle(0, First, First + 2, First + 3);
		}
		return Data;
	}

	FOceanMeshData MakeRock(int32 Seed)
	{
		FRandomStream Random(Seed);
		FOceanMeshData Data(1);
		TArray<FVector3f> Directions;
		TArray<int32> Triangles;
		MakeIcosphere(3, Directions, Triangles);

		const FVector3f Offset(Random.FRandRange(0.f, 50.f), Random.FRandRange(0.f, 50.f), Random.FRandRange(0.f, 50.f));
		const FVector3f Scale(1.f, Random.FRandRange(0.7f, 1.1f), Random.FRandRange(0.5f, 0.8f));
		TArray<FVector3f> Positions;
		TArray<float> Relief;
		Positions.Reserve(Directions.Num());
		for (const FVector3f& Direction : Directions)
		{
			// Large lumps, angular ridges (folded noise: sharp creases instead of round bulges), then the sharp
			// pits of eroded coral limestone ("ironshore").
			const float Lumps = Fractal3(Direction * 1.3f + Offset, 4) - 0.5f;
			const float Ridges = 0.5f - FMath::Abs(Fractal3(Direction * 2.4f + Offset * 1.7f, 3) - 0.5f) * 2.f;
			const float Pits = Fractal3(Direction * 5.f + Offset * 2.f, 3) - 0.5f;
			const float Radius = 1.f + 0.6f * Lumps + 0.3f * Ridges + 0.15f * Pits;
			FVector3f Position = Direction * Radius * Scale * 100.f;
			Position.Z = FMath::Max(Position.Z, -30.f); // flat base, sits on the ground
			Positions.Add(Position);
			Relief.Add(Radius);
		}
		const TArray<FVector3f> Normals = SmoothNormals(Positions, Triangles);
		for (int32 Index = 0; Index < Positions.Num(); ++Index)
		{
			const FVector3f& Position = Positions[Index];
			// Grey limestone, darker in the hollows, a little algae at the foot.
			const float Tone = FMath::Clamp(0.6f + 0.9f * (Relief[Index] - 1.f) + 0.3f * Fractal3(Position * 0.03f + Offset, 3), 0.35f, 1.2f);
			FLinearColor Color = FLinearColor(0.32f, 0.3f, 0.26f) * Tone;
			Color = FMath::Lerp(Color, FLinearColor(0.12f, 0.14f, 0.08f), FMath::Clamp((-Position.Z) / 40.f, 0.f, 1.f) * 0.6f);
			Data.AddVertex(Position, Normals[Index], FVector2f(Position.X * 0.01f, Position.Y * 0.01f), Color);
		}
		for (int32 I = 0; I < Triangles.Num(); I += 3)
		{
			Data.AddTriangle(0, Triangles[I], Triangles[I + 2], Triangles[I + 1]);
		}
		return Data;
	}

	// --- More Caribbean plants, all in 3D (no flat cards) ------------------------------------------------------

	/**
	 * A leaf blade along an arching curve from Base, heading Out (horizontal) at StartPitch and bending down by Droop
	 * towards its tip; Roll turns its plane about the midrib. Three vertices across, the edges raised by Fold x the
	 * half width (channelled, > 0) or lowered (< 0). Shape: 0 = tapering strap (agave, bromeliad), 1 = paddle on a
	 * petiole (heliconia), 2 = narrow lance (pinna). Returns the tip.
	 */
	FVector3f AddBlade(FOceanMeshData& Data, int32 Section, const FVector3f& Base, const FVector3f& Out, float Length,
		float StartPitch, float Droop, float Width, float Fold, float Roll, int32 Shape, int32 Steps,
		const FLinearColor& Color, const FLinearColor& EdgeColor)
	{
		FVector3f Position = Base;
		int32 Previous[3] = { INDEX_NONE, INDEX_NONE, INDEX_NONE };
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			const float S = float(Step) / Steps;
			const float Pitch = StartPitch - Droop * S * S;
			const FVector3f Direction = (Out * FMath::Cos(Pitch) + FVector3f(0.f, 0.f, FMath::Sin(Pitch))).GetSafeNormal();
			FVector3f Side = FVector3f::CrossProduct(Direction, FVector3f::UpVector).GetSafeNormal();
			if (Side.IsNearlyZero())
			{
				Side = FVector3f::CrossProduct(Direction, FVector3f::ForwardVector).GetSafeNormal();
			}
			FVector3f Up = FVector3f::CrossProduct(Side, Direction).GetSafeNormal();
			const float R = Roll * S;
			const FVector3f RolledSide = Side * FMath::Cos(R) + Up * FMath::Sin(R);
			Up = FVector3f::CrossProduct(RolledSide, Direction).GetSafeNormal();
			Side = RolledSide;
			float W = 0.f;
			switch (Shape)
			{
			case 0: W = Width * FMath::Pow(1.f - S, 0.8f); break;
			case 1: W = S < 0.25f ? 0.06f * Width : Width * FMath::Pow(FMath::Max(FMath::Sin(UE_PI * (S - 0.25f) / 0.75f), 0.f), 0.6f); break;
			default: W = Width * FMath::Pow(FMath::Max(FMath::Sin(UE_PI * FMath::Min(S * 1.1f, 1.f)), 0.f), 0.8f); break; // Max: sin(pi) < 0 by a hair, and Pow of it is NaN
			}
			W *= 0.5f;
			const FVector3f Left = Position - Side * W + Up * (Fold * W);
			const FVector3f Right = Position + Side * W + Up * (Fold * W);
			const int32 Row[3] = {
				Data.AddVertex(Left, (Up - Side * 0.3f * FMath::Sign(Fold + 0.001f)).GetSafeNormal(), FVector2f(0.f, S), EdgeColor),
				Data.AddVertex(Position, Up, FVector2f(0.5f, S), Color),
				Data.AddVertex(Right, (Up + Side * 0.3f * FMath::Sign(Fold + 0.001f)).GetSafeNormal(), FVector2f(1.f, S), EdgeColor) };
			if (Step > 0)
			{
				for (int32 Column = 0; Column < 2; ++Column)
				{
					Data.AddTriangle(Section, Previous[Column], Previous[Column + 1], Row[Column]);
					Data.AddTriangle(Section, Previous[Column + 1], Row[Column + 1], Row[Column]);
				}
			}
			FMemory::Memcpy(Previous, Row, sizeof(Row));
			if (Step < Steps)
			{
				Position += Direction * (Length / Steps);
			}
		}
		return Position;
	}

	/** A ribbed column (cactus) along a curve: Ribs ridges of relative depth RibDepth, rounded top. */
	template <typename CurveType, typename RadiusType>
	void AddRibbedColumn(FOceanMeshData& Data, int32 Section, const CurveType& Curve, const RadiusType& Radius, int32 Ribs,
		float RibDepth, int32 Rings, int32 Sides, const FLinearColor& Color, const FLinearColor& RibColor)
	{
		const int32 First = Data.Vertices.Num();
		const int32 Row = Sides + 1;
		FVector3f SideX = FVector3f::ZeroVector;
		for (int32 Ring = 0; Ring <= Rings; ++Ring)
		{
			const float T = float(Ring) / Rings;
			const FVector3f Point = Curve(T);
			const FVector3f Tangent = (Curve(FMath::Min(T + 0.01f, 1.f)) - Curve(FMath::Max(T - 0.01f, 0.f))).GetSafeNormal();
			if (Ring == 0)
			{
				const FVector3f Helper = FMath::Abs(Tangent.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector;
				SideX = FVector3f::CrossProduct(Tangent, Helper).GetSafeNormal();
			}
			else
			{
				SideX = (SideX - Tangent * FVector3f::DotProduct(SideX, Tangent)).GetSafeNormal();
			}
			const FVector3f SideY = FVector3f::CrossProduct(Tangent, SideX);
			// Rounded top: the last tenth closes in.
			const float Close = T > 0.9f ? FMath::Sqrt(FMath::Max(1.f - FMath::Square((T - 0.9f) / 0.1f), 0.f)) : 1.f;
			const float R = Radius(T) * FMath::Max(Close, 0.05f);
			for (int32 Side = 0; Side <= Sides; ++Side)
			{
				const float Angle = 2.f * UE_PI * Side / Sides;
				const float Rib = FMath::Pow(FMath::Abs(FMath::Cos(0.5f * Ribs * Angle)), 0.6f); // 1 on a ridge, 0 in a groove
				const FVector3f Out = SideX * FMath::Cos(Angle) + SideY * FMath::Sin(Angle);
				const float Radial = R * (1.f - RibDepth + RibDepth * Rib);
				Data.AddVertex(Point + Out * Radial, (Out + Tangent * (1.f - Close) * 2.f).GetSafeNormal(),
					FVector2f(float(Side) / Sides, T), FMath::Lerp(Color, RibColor, Rib));
			}
			if (Ring > 0)
			{
				const int32 Below = First + (Ring - 1) * Row, Above = First + Ring * Row;
				for (int32 Side = 0; Side < Sides; ++Side)
				{
					Data.AddTriangle(Section, Below + Side, Above + Side, Below + Side + 1);
					Data.AddTriangle(Section, Below + Side + 1, Above + Side, Above + Side + 1);
				}
			}
		}
	}

	FOceanMeshData MakeAgave(int32 Seed)
	{
		// Agave (A. karatto, A. antillarum): the century plant of Caribbean limestone coasts. A rosette of thick, rigid,
		// channelled grey-green leaves, the inner ones erect, the outer ones spreading and recurved, the oldest dry. Now
		// and then the tall flowering pole with its candelabra of yellow flower clusters.
		FRandomStream Random(Seed);
		FOceanMeshData Data(2);
		const int32 Leaves = Random.RandRange(24, 36);
		const float Size = Random.FRandRange(60.f, 110.f);
		const float Spiral = Random.FRandRange(0.f, 2.f * UE_PI);
		const FLinearColor Blue(0.2f, 0.3f, 0.22f), Edge(0.17f, 0.21f, 0.12f), Dry(0.3f, 0.23f, 0.13f);
		for (int32 Leaf = 0; Leaf < Leaves; ++Leaf)
		{
			const float Age = (Leaf + Random.FRand()) / Leaves; // 0 = inner, young
			const float Azimuth = Spiral + Leaf * 2.39996f;
			const FVector3f Out(FMath::Cos(Azimuth), FMath::Sin(Azimuth), 0.f);
			const bool bDead = Age > 0.9f && Random.FRand() < 0.6f;
			const float Length = Size * FMath::Lerp(0.55f, 1.f, FMath::Sqrt(Age)) * Random.FRandRange(0.9f, 1.1f);
			const float Pitch = FMath::DegreesToRadians(bDead ? -5.f : FMath::Lerp(80.f, 12.f, Age));
			const float Droop = FMath::DegreesToRadians(bDead ? 50.f : FMath::Lerp(5.f, 40.f, Age * Age));
			const FLinearColor Color = (bDead ? Dry : Blue) * Random.FRandRange(0.85f, 1.15f);
			const FVector3f Base = Out * (3.f + 6.f * Age) + FVector3f(0.f, 0.f, 4.f + 8.f * (1.f - Age));
			AddBlade(Data, 1, Base, Out, Length, Pitch, Droop, 0.17f * Length, 0.45f, FMath::DegreesToRadians(Random.FRandRange(-10.f, 10.f)),
				0, 6, Color, bDead ? Dry * 0.8f : Edge);
		}
		if (Random.FRand() < 0.3f)
		{
			const float PoleHeight = Random.FRandRange(300.f, 600.f);
			const float Lean = Random.FRandRange(-0.08f, 0.08f);
			auto Pole = [&](float T) { return FVector3f(Lean * PoleHeight * T * T, 0.f, 10.f + PoleHeight * T); };
			AddBarkTube(Data, 0, Pole, [](float T) { return FMath::Lerp(6.f, 2.f, T); }, 10, 6, FLinearColor(0.2f, 0.2f, 0.1f));
			const int32 Branches = Random.RandRange(6, 10);
			for (int32 Branch = 0; Branch < Branches; ++Branch)
			{
				const float T = FMath::Lerp(0.6f, 0.95f, float(Branch) / Branches);
				const float Azimuth = Branch * 2.39996f;
				const FVector3f Start = Pole(T);
				const FVector3f End = Start + FVector3f(FMath::Cos(Azimuth), FMath::Sin(Azimuth), 0.35f) * FMath::Lerp(55.f, 20.f, T);
				AddTube(Data, 0, Start, End, 1.6f, 1.f, 5, FLinearColor(0.2f, 0.2f, 0.1f), FLinearColor(0.2f, 0.2f, 0.1f));
				FRandomStream Flower(Seed * 13 + Branch);
				AddLeafCluster(Data, End + FVector3f(0.f, 0.f, 6.f), 11.f, Flower, FLinearColor(0.55f, 0.45f, 0.05f), FLinearColor(0.35f, 0.33f, 0.08f), 0.5f, 1);
			}
		}
		return Data;
	}

	FOceanMeshData MakeTurksCap(int32 Seed)
	{
		// Turk's cap cactus (Melocactus intortus), on the limestone of Pointe des Chateaux and the Grenadines: a glaucous
		// green globe of a dozen deep ribs, each lined with areoles of stout reddish spines radiating against the body,
		// crowned with a cephalium of red-brown wool and cream bristles from which small pink club-shaped fruits poke out.
		// One section (vertex colour, linear albedo).
		FRandomStream Random(Seed);
		FOceanMeshData Data(1);
		const float R = Random.FRandRange(12.f, 19.f);
		const float Height = R * Random.FRandRange(1.4f, 2.f);
		const int32 Ribs = Random.RandRange(11, 15);
		const FLinearColor Body(0.05f, 0.085f, 0.06f), RibEdge(0.085f, 0.12f, 0.08f);
		auto Axis = [&](float T) { return FVector3f(0.f, 0.f, -3.f + Height * T); };
		auto Radius = [&](float T) { return R * FMath::Max(FMath::Pow(FMath::Sin(UE_PI * FMath::Lerp(0.12f, 0.93f, T)), 0.6f), 0.3f); };
		AddRibbedColumn(Data, 0, Axis, Radius, Ribs, 0.3f, 14, 4 * Ribs, Body, RibEdge);

		// Areoles along every rib's crest, each a star of spines lying close to the body: one central, six radials.
		const float Spacing = 0.22f * R;
		for (int32 Rib = 0; Rib < Ribs; ++Rib)
		{
			const float Angle = 2.f * UE_PI * Rib / Ribs; // a crest of AddRibbedColumn (cos(Ribs / 2 x angle) = +-1)
			// AddRibbedColumn's frame on a vertical axis: angle 0 along +Y, then towards -X.
			const FVector3f Out(-FMath::Sin(Angle), FMath::Cos(Angle), 0.f);
			for (float T = 0.14f; T < 0.88f; T += Spacing / Height)
			{
				const float Jitter = Random.FRandRange(-0.015f, 0.015f);
				const FVector3f Areole = Axis(T + Jitter) + Out * Radius(T + Jitter);
				const FVector3f Up = (Axis(T + Jitter + 0.02f) + Out * Radius(T + Jitter + 0.02f) - Areole).GetSafeNormal();
				const FVector3f Side = FVector3f::CrossProduct(Up, Out).GetSafeNormal();
				for (int32 Spine = 0; Spine < 7; ++Spine)
				{
					const float A = Spine == 0 ? 0.f : 2.f * UE_PI * (Spine - 1) / 6.f + Random.FRandRange(-0.25f, 0.25f);
					const FVector3f Along = Spine == 0 ? (Out * 0.8f - Up * 0.6f).GetSafeNormal()
						: (Out * 0.3f + Up * FMath::Cos(A) * 0.95f + Side * FMath::Sin(A) * 0.95f).GetSafeNormal();
					const float Length = (Spine == 0 ? 2.6f : 1.7f) * Random.FRandRange(0.8f, 1.2f) * R / 15.f;
					const FVector3f Across = FVector3f::CrossProduct(Along, Out).GetSafeNormal() * 0.14f;
					const FLinearColor Root(0.17f, 0.07f, 0.045f), Tip(0.3f, 0.26f, 0.21f);
					const int32 First = Data.Vertices.Num();
					Data.AddVertex(Areole - Across, Out, FVector2f(0.f, 1.f), Root);
					Data.AddVertex(Areole + Across, Out, FVector2f(1.f, 1.f), Root);
					Data.AddVertex(Areole + Along * Length, Out, FVector2f(0.5f, 0.f), Tip);
					Data.AddTriangle(0, First, First + 1, First + 2);
				}
			}
		}

		// The cephalium: a cylinder of wool, finely ribbed by its bristles, red-brown and cream.
		const float CapHeight = R * Random.FRandRange(0.4f, 1.1f);
		const float CapBase = 0.93f * Height - 3.f - 0.15f * R;
		AddRibbedColumn(Data, 0, [&](float T) { return FVector3f(0.f, 0.f, CapBase + CapHeight * T); },
			[&](float T) { return 0.5f * R * (1.f - 0.15f * T); }, 30, 0.1f, 6, 60, FLinearColor(0.2f, 0.06f, 0.045f), FLinearColor(0.42f, 0.33f, 0.26f));
		// Fruits: small pink-magenta clubs poking out of its top.
		const int32 Fruits = Random.RandRange(2, 7);
		for (int32 Fruit = 0; Fruit < Fruits; ++Fruit)
		{
			const float A = Random.FRandRange(0.f, 2.f * UE_PI);
			const float D = Random.FRandRange(0.1f, 0.35f) * R;
			const FVector3f Base(FMath::Cos(A) * D, FMath::Sin(A) * D, CapBase + CapHeight - 0.6f);
			const FVector3f Lean = FVector3f(FMath::Cos(A) * 0.3f, FMath::Sin(A) * 0.3f, 1.f).GetSafeNormal();
			const float Length = Random.FRandRange(1.4f, 2.2f);
			AddBarkTube(Data, 0, [&](float T) { return Base + Lean * (Length * T); },
				[&](float T) { return 0.35f + 0.25f * FMath::Sin(UE_PI * FMath::Min(T * 1.2f, 1.f)); }, 3, 5, FLinearColor(0.55f, 0.05f, 0.2f));
		}
		return Data;
	}

	FOceanMeshData MakeColumnCactus(int32 Seed)
	{
		// Dildo cactus (Pilosocereus royenii): a clump of ribbed, glaucous columns 2-4 m tall, curving up from the base,
		// on dry limestone and in the coastal scrub. Bark section (vertex colour).
		FRandomStream Random(Seed);
		FOceanMeshData Data(1);
		const int32 Stems = Random.RandRange(3, 7);
		const FLinearColor Glaucous(0.035f, 0.07f, 0.05f), Rib(0.07f, 0.11f, 0.08f);
		for (int32 Stem = 0; Stem < Stems; ++Stem)
		{
			const float Azimuth = Random.FRandRange(0.f, 2.f * UE_PI);
			const FVector3f Out(FMath::Cos(Azimuth), FMath::Sin(Azimuth), 0.f);
			const float Height = Stem == 0 ? Random.FRandRange(250.f, 420.f) : Random.FRandRange(120.f, 330.f);
			const float Spread = Stem == 0 ? 0.f : Random.FRandRange(25.f, 70.f);
			const float Radius = Random.FRandRange(6.5f, 9.f);
			AddRibbedColumn(Data, 0, [&](float T)
				{
					// Out from the base, then up.
					return Out * (Spread * FMath::Sqrt(T) + 8.f * (Stem > 0 ? 1.f : 0.f)) + FVector3f(0.f, 0.f, Height * T - 5.f);
				},
				[&](float T) { return Radius * (1.f - 0.15f * T); }, Random.RandRange(8, 11), 0.22f, 14, 30, Glaucous, Rib);
		}
		return Data;
	}

	FOceanMeshData MakeBromeliad(int32 Seed)
	{
		// Ground and rock bromeliads of the Antillean forests (Hohenbergia, Guzmania): a funnel rosette of arching strap
		// leaves, some with a red heart.
		FRandomStream Random(Seed);
		FOceanMeshData Data(2);
		const int32 Leaves = Random.RandRange(14, 22);
		const float Size = Random.FRandRange(35.f, 70.f);
		const bool bRed = Random.FRand() < 0.45f;
		const FLinearColor Green(0.09f, 0.18f, 0.05f), Heart(0.36f, 0.06f, 0.04f);
		const float Spiral = Random.FRandRange(0.f, 2.f * UE_PI);
		for (int32 Leaf = 0; Leaf < Leaves; ++Leaf)
		{
			const float Age = (Leaf + Random.FRand()) / Leaves;
			const float Azimuth = Spiral + Leaf * 2.39996f;
			const FVector3f Out(FMath::Cos(Azimuth), FMath::Sin(Azimuth), 0.f);
			const float Length = Size * FMath::Lerp(0.6f, 1.f, Age) * Random.FRandRange(0.9f, 1.1f);
			// Red-hearted ones turn red from the centre out (Guzmania): the inner leaves red, the next ones orange then green;
			// the others a glossy green, some yellower, the outer leaves with dry tips.
			const FLinearColor Leafy = Green * FMath::Lerp(FLinearColor(0.9f, 1.f, 1.1f), FLinearColor(1.2f, 1.05f, 0.75f), Random.FRand()) * Random.FRandRange(0.85f, 1.15f);
			const FLinearColor Color = bRed ? FMath::Lerp(Heart, Leafy, FMath::SmoothStep(0.2f, 0.55f, Age)) : Leafy;
			AddBlade(Data, 1, Out * 2.f + FVector3f(0.f, 0.f, 3.f), Out, Length, FMath::DegreesToRadians(FMath::Lerp(72.f, 30.f, Age)),
				FMath::DegreesToRadians(FMath::Lerp(40.f, 95.f, Age)), Random.FRandRange(4.5f, 7.f), 0.35f,
				FMath::DegreesToRadians(Random.FRandRange(-20.f, 20.f)), 0, 6, Color, Color * 0.8f);
		}
		// Some flower: a spike of red bracts rising from the heart, tipped yellow.
		if (bRed && Random.FRand() < 0.6f)
		{
			const float SpikeHeight = Size * Random.FRandRange(0.35f, 0.55f);
			const int32 Bracts = Random.RandRange(8, 12);
			for (int32 Bract = 0; Bract < Bracts; ++Bract)
			{
				const float T = (Bract + 0.5f) / Bracts;
				const float Azimuth = Bract * 2.39996f;
				const FVector3f Out(FMath::Cos(Azimuth), FMath::Sin(Azimuth), 0.f);
				const FLinearColor Bright = FMath::Lerp(FLinearColor(0.5f, 0.04f, 0.03f), FLinearColor(0.6f, 0.4f, 0.03f), FMath::SmoothStep(0.7f, 1.f, T));
				AddBlade(Data, 1, FVector3f(0.f, 0.f, 4.f + SpikeHeight * T), Out, Size * 0.18f * (1.2f - 0.6f * T),
					FMath::DegreesToRadians(65.f), FMath::DegreesToRadians(20.f), 3.5f * (1.2f - 0.5f * T), 0.4f, 0.f, 0, 3, Bright, Bright * 0.8f);
			}
			AddBarkTube(Data, 1, [&](float T) { return FVector3f(0.f, 0.f, 2.f + SpikeHeight * T); }, [](float T) { return 1.2f * (1.f - 0.6f * T); }, 4, 5,
				FLinearColor(0.42f, 0.05f, 0.03f));
		}
		return Data;
	}

	FOceanMeshData MakeFern(int32 Seed)
	{
		// A forest fern (Nephrolepis, Thelypteris): a crown of arching fronds, each a rachis with narrow pinnae on both
		// sides, longest mid-frond; the outer fronds paler and drooping.
		FRandomStream Random(Seed);
		FOceanMeshData Data(2);
		const int32 Fronds = Random.RandRange(8, 14);
		const float Size = Random.FRandRange(60.f, 140.f);
		const float Spiral = Random.FRandRange(0.f, 2.f * UE_PI);
		for (int32 Frond = 0; Frond < Fronds; ++Frond)
		{
			const float Age = (Frond + Random.FRand()) / Fronds;
			const float Azimuth = Spiral + Frond * 2.39996f;
			const FVector3f Out(FMath::Cos(Azimuth), FMath::Sin(Azimuth), 0.f);
			const float Length = Size * FMath::Lerp(0.6f, 1.f, Age) * Random.FRandRange(0.85f, 1.1f);
			const float StartPitch = FMath::DegreesToRadians(FMath::Lerp(72.f, 32.f, Age));
			const float Droop = FMath::DegreesToRadians(FMath::Lerp(70.f, 115.f, Age));
			const FLinearColor Color = FMath::Lerp(FLinearColor(0.06f, 0.15f, 0.03f), FLinearColor(0.13f, 0.17f, 0.04f), Age * Age) * Random.FRandRange(0.85f, 1.15f);
			const int32 Steps = 20;
			FVector3f Position = Out * 2.f;
			for (int32 Step = 0; Step < Steps; ++Step)
			{
				const float S = (Step + 0.5f) / Steps;
				const float Pitch = StartPitch - Droop * S * S;
				const FVector3f Direction = (Out * FMath::Cos(Pitch) + FVector3f(0.f, 0.f, FMath::Sin(Pitch))).GetSafeNormal();
				const FVector3f Side = FVector3f::CrossProduct(Direction, FVector3f::UpVector).GetSafeNormal();
				Position += Direction * (Length / Steps);
				if (S < 0.12f)
				{
					continue; // the bare stipe
				}
				const float PinnaLength = 0.3f * Length * FMath::Pow(FMath::Max(FMath::Sin(UE_PI * FMath::Min((S - 0.12f) / 0.9f, 1.f)), 0.f), 0.7f);
				for (int32 Sign = -1; Sign <= 1; Sign += 2)
				{
					// Pinnae lean forward along the rachis and droop a little.
					const FVector3f PinnaOut = (Side * float(Sign) + Direction * 0.45f).GetSafeNormal();
					AddBlade(Data, 1, Position, FVector3f(PinnaOut.X, PinnaOut.Y, 0.f).GetSafeNormal(), PinnaLength,
						FMath::Asin(FMath::Clamp(PinnaOut.Z, -1.f, 1.f)) - 0.15f, 0.35f, 0.22f * PinnaLength, 0.15f, 0.f, 2, 2, Color, Color * 0.85f);
				}
			}
		}
		return Data;
	}

	FOceanMeshData MakeHeliconia(int32 Seed)
	{
		// Heliconia caribaea, the balisier of the Lesser Antilles' forests: a clump of pseudostems with huge paddle leaves
		// on long petioles, two ranks to a stem, and on some the zigzag of red and yellow bracts.
		FRandomStream Random(Seed);
		FOceanMeshData Data(2);
		const int32 Stems = Random.RandRange(4, 8);
		const FLinearColor Stem(0.1f, 0.17f, 0.06f), Leaf(0.07f, 0.17f, 0.035f), Rib(0.12f, 0.2f, 0.06f);
		for (int32 Index = 0; Index < Stems; ++Index)
		{
			const FVector3f Root(Random.FRandRange(-30.f, 30.f), Random.FRandRange(-30.f, 30.f), -5.f);
			const float Height = Random.FRandRange(110.f, 230.f);
			const FVector3f Lean(Random.FRandRange(-0.12f, 0.12f), Random.FRandRange(-0.12f, 0.12f), 1.f);
			const FVector3f Top = Root + Lean.GetSafeNormal() * Height;
			AddTube(Data, 0, Root, Top, 2.6f, 2.f, 6, Stem, Stem);
			const float Rank = Random.FRandRange(0.f, 2.f * UE_PI);
			const int32 Leaves = Random.RandRange(2, 4);
			for (int32 L = 0; L < Leaves; ++L)
			{
				const float Azimuth = Rank + (L % 2) * UE_PI + Random.FRandRange(-0.3f, 0.3f);
				const FVector3f Out(FMath::Cos(Azimuth), FMath::Sin(Azimuth), 0.f);
				const FVector3f Base = FMath::Lerp(Root, Top, FMath::Lerp(0.55f, 1.f, float(L) / Leaves));
				AddBlade(Data, 1, Base, Out, Random.FRandRange(120.f, 190.f), FMath::DegreesToRadians(Random.FRandRange(50.f, 72.f)),
					FMath::DegreesToRadians(Random.FRandRange(35.f, 70.f)), Random.FRandRange(26.f, 36.f), -0.15f,
					FMath::DegreesToRadians(Random.FRandRange(-25.f, 25.f)), 1, 10, Leaf * Random.FRandRange(0.85f, 1.15f), Rib);
			}
			if (Random.FRand() < 0.4f)
			{
				// The inflorescence: boat-shaped bracts alternating along a zigzag axis, red with yellow lips.
				FVector3f Bract = Top;
				for (int32 B = 0; B < 7; ++B)
				{
					const FVector3f Next = Bract + FVector3f((B % 2 ? 6.f : -6.f), 0.f, 9.f);
					FRandomStream BractRandom(Seed * 17 + Index * 7 + B);
					AddLeafCluster(Data, Next, 6.f, BractRandom, FLinearColor(0.45f, 0.04f, 0.03f), FLinearColor(0.5f, 0.35f, 0.03f), 0.35f, 1);
					Bract = Next;
				}
			}
		}
		return Data;
	}

	FOceanMeshData MakeBeachCreeper(int32 Seed)
	{
		// Beach morning glory (Ipomoea pes-caprae), the creeper of the upper beach: runners radiating over the sand with
		// leathery leaves folded along the midrib, standing up on their petioles, and pink-purple trumpets here and there.
		FRandomStream Random(Seed);
		FOceanMeshData Data(2);
		const int32 Runners = Random.RandRange(5, 9);
		const FLinearColor Vine(0.2f, 0.12f, 0.08f), Leaf(0.07f, 0.15f, 0.035f);
		for (int32 Runner = 0; Runner < Runners; ++Runner)
		{
			const float Azimuth = Random.FRandRange(0.f, 2.f * UE_PI);
			const float Length = Random.FRandRange(140.f, 340.f);
			const float Wiggle = Random.FRandRange(0.15f, 0.4f);
			const float Phase = Random.FRandRange(0.f, 6.f);
			auto Curve = [&](float T)
			{
				const float A = Azimuth + Wiggle * FMath::Sin(Phase + 7.f * T);
				return FVector3f(FMath::Cos(A), FMath::Sin(A), 0.f) * (Length * T) + FVector3f(0.f, 0.f, 1.f);
			};
			AddBarkTube(Data, 0, Curve, [](float) { return 0.6f; }, 16, 4, Vine);
			for (float T = 0.05f; T < 0.98f; T += Random.FRandRange(0.04f, 0.08f))
			{
				const FVector3f At = Curve(T);
				const FVector3f Side(Random.GetUnitVector());
				const FVector3f Face = (FVector3f::UpVector * 0.9f + FVector3f(Side.X, Side.Y, 0.f) * 0.5f).GetSafeNormal();
				FVector3f Axis = FVector3f(Side.X, Side.Y, 0.f).GetSafeNormal();
				Axis = (Axis - Face * FVector3f::DotProduct(Axis, Face)).GetSafeNormal();
				const FVector3f Across = FVector3f::CrossProduct(Face, Axis);
				const float Size = Random.FRandRange(6.f, 10.f) * FMath::Lerp(1.f, 0.6f, T);
				const FVector3f Root = At + FVector3f(0.f, 0.f, Random.FRandRange(4.f, 12.f));
				AddTube(Data, 0, At, Root, 0.25f, 0.2f, 3, Vine, Vine);
				// A round, notched leaf folded up along its midrib.
				const FLinearColor Color = Leaf * Random.FRandRange(0.85f, 1.2f);
				const int32 First = Data.Vertices.Num();
				Data.AddVertex(Root, Face, FVector2f(0.5f, 1.f), Color);
				Data.AddVertex(Root + Axis * (0.45f * Size) - Across * (0.55f * Size) + Face * (0.25f * Size), Face, FVector2f(0.f, 0.5f), Color);
				Data.AddVertex(Root + Axis * (0.5f * Size), Face, FVector2f(0.5f, 0.5f), Color * 1.1f);
				Data.AddVertex(Root + Axis * (0.45f * Size) + Across * (0.55f * Size) + Face * (0.25f * Size), Face, FVector2f(1.f, 0.5f), Color);
				Data.AddVertex(Root + Axis * (0.95f * Size) - Across * (0.15f * Size) + Face * (0.1f * Size), Face, FVector2f(0.3f, 0.f), Color);
				Data.AddVertex(Root + Axis * (0.95f * Size) + Across * (0.15f * Size) + Face * (0.1f * Size), Face, FVector2f(0.7f, 0.f), Color);
				Data.AddTriangle(1, First, First + 2, First + 1);
				Data.AddTriangle(1, First, First + 3, First + 2);
				Data.AddTriangle(1, First + 1, First + 2, First + 4);
				Data.AddTriangle(1, First + 2, First + 3, First + 5);
				Data.AddTriangle(1, First + 2, First + 5, First + 4);
				if (Random.FRand() < 0.07f)
				{
					FRandomStream Flower(Seed * 29 + Runner * 101 + int32(T * 100.f));
					AddLeafCluster(Data, Root + FVector3f(0.f, 0.f, 4.f), 3.5f, Flower, FLinearColor(0.42f, 0.1f, 0.32f), FLinearColor(0.55f, 0.3f, 0.45f), 0.4f, 1);
				}
			}
		}
		return Data;
	}

	FOceanMeshData MakeCaribbeanRock(ERockKind Kind, int32 Seed)
	{
		FRandomStream Random(Seed * 7919 + int32(Kind) * 104729);
		FOceanMeshData Data(1);
		TArray<FVector3f> Directions;
		TArray<int32> Triangles;
		const bool bSmall = Kind == ERockKind::CoralRubble || Kind == ERockKind::Cobble;
		MakeIcosphere(bSmall ? 3 : 4, Directions, Triangles);
		const FVector3f Offset(Random.FRandRange(0.f, 90.f), Random.FRandRange(0.f, 90.f), Random.FRandRange(0.f, 90.f));
		auto Ridged = [](const FVector3f& P, int32 Octaves) { return 1.f - FMath::Abs(2.f * Fractal3(P, Octaves) - 1.f); };

		// Proportions (semi-axes, the longest 1 = 100 cm) and squareness (superellipsoid exponent: 2 = ellipsoid,
		// higher = a block with rounded edges) per kind.
		FVector3f Axes(1.f, Random.FRandRange(0.6f, 0.95f), Random.FRandRange(0.45f, 0.8f));
		float Square = 2.f;
		FLinearColor Light, Dark;
		switch (Kind)
		{
		case ERockKind::Limestone:   // a broken block of bedded limestone: angular, layered, pitted by dissolution
			Square = Random.FRandRange(3.5f, 6.f);
			Light = FLinearColor(0.42f, 0.39f, 0.33f); Dark = FLinearColor(0.2f, 0.19f, 0.17f);
			break;
		case ERockKind::Ironshore:   // low, wide, black-grey and razor-jagged (biokarst of the spray zone)
			Axes = FVector3f(1.f, Random.FRandRange(0.6f, 1.f), Random.FRandRange(0.22f, 0.38f));
			Square = Random.FRandRange(2.5f, 3.5f);
			Light = FLinearColor(0.36f, 0.34f, 0.3f); Dark = FLinearColor(0.1f, 0.1f, 0.09f);
			break;
		case ERockKind::Beachrock:   // a cemented sand slab, tabular, cut by joints into blocks
			Axes = FVector3f(1.f, Random.FRandRange(0.5f, 0.8f), Random.FRandRange(0.1f, 0.16f));
			Square = Random.FRandRange(4.f, 7.f);
			Light = FLinearColor(0.6f, 0.52f, 0.4f); Dark = FLinearColor(0.3f, 0.27f, 0.2f);
			break;
		case ERockKind::Volcanic:    // a rounded andesite boulder of the Lesser Antilles, dark and smooth
			Square = Random.FRandRange(2.f, 2.6f);
			Light = FLinearColor(0.2f, 0.19f, 0.18f); Dark = FLinearColor(0.07f, 0.07f, 0.065f);
			break;
		case ERockKind::CoralRubble: // a bleached lump of dead coral, knobbly
			Axes = FVector3f(1.f, Random.FRandRange(0.6f, 0.9f), Random.FRandRange(0.4f, 0.7f));
			Light = FLinearColor(0.66f, 0.62f, 0.55f); Dark = FLinearColor(0.38f, 0.35f, 0.31f);
			break;
		case ERockKind::Cobble:      // a water-worn pebble
		default:
			Axes = FVector3f(1.f, Random.FRandRange(0.65f, 0.9f), Random.FRandRange(0.35f, 0.55f));
			Light = FMath::Lerp(FLinearColor(0.5f, 0.47f, 0.42f), FLinearColor(0.22f, 0.21f, 0.2f), Random.FRand());
			Dark = Light * 0.55f;
			break;
		}

		// Fracture planes: a broken block is bounded by a few flat faces at random angles.
		TArray<FVector4f> Planes;
		if (Kind == ERockKind::Limestone || Kind == ERockKind::Beachrock)
		{
			const int32 Count = Random.RandRange(4, 7);
			for (int32 I = 0; I < Count; ++I)
			{
				FVector3f Normal = FVector3f(Random.GetUnitVector());
				Normal.Z *= Kind == ERockKind::Beachrock ? 0.15f : 0.6f; // mostly steep faces
				Normal.Normalize();
				Planes.Add(FVector4f(Normal.X, Normal.Y, Normal.Z, Random.FRandRange(0.72f, 0.95f)));
			}
		}
		const float BedPhase = Random.FRand();
		const float BedThickness = Random.FRandRange(0.18f, 0.32f) * Axes.Z * 2.f;
		const float JointAngle = Random.FRand() * 2.f * UE_PI;
		const FVector3f JointA(FMath::Cos(JointAngle), FMath::Sin(JointAngle), 0.f);
		const FVector3f JointB(-JointA.Y, JointA.X, 0.f);

		TArray<FVector3f> Positions;
		TArray<float> Weathering;
		Positions.Reserve(Directions.Num());
		Weathering.Reserve(Directions.Num());
		for (const FVector3f& D : Directions)
		{
			// Superellipsoid radius along D.
			const float P = Square;
			const float Sum = FMath::Pow(FMath::Abs(D.X / Axes.X), P) + FMath::Pow(FMath::Abs(D.Y / Axes.Y), P) + FMath::Pow(FMath::Abs(D.Z / Axes.Z), P);
			float R = FMath::Pow(Sum, -1.f / P);
			// Cut by the fracture planes (distance along D to the plane at Plane.W of the radius).
			for (const FVector4f& Plane : Planes)
			{
				const float Facing = D.X * Plane.X + D.Y * Plane.Y + D.Z * Plane.Z;
				if (Facing > 0.05f)
				{
					R = FMath::Min(R, Plane.W / Facing); // the flat fracture face
				}
			}
			const FVector3f S = D * R;
			float Hollow = 0.f; // how sunken this point is (darker)
			switch (Kind)
			{
			case ERockKind::Limestone:
			{
				// Bedding: notches along the layer boundaries; dissolution pits; rills (karren) running down the top.
				const float Layer = FMath::Frac(S.Z / BedThickness + BedPhase + 0.08f * Fractal3(S * 3.f + Offset, 2));
				const float Notch = FMath::Exp(-FMath::Square((Layer - 0.5f) / 0.07f));
				const float Pits = FMath::SmoothStep(0.62f, 0.8f, Fractal3(S * 6.f + Offset * 1.3f, 3));
				const float Rills = Ridged(FVector3f(S.X * 9.f, S.Y * 9.f, S.Z * 1.5f) + Offset, 2) * FMath::Max(D.Z, 0.f);
				const float Lumps = Fractal3(S * 2.2f + Offset, 4) - 0.5f;
				R += 0.18f * Lumps - 0.07f * Notch * (1.f - FMath::Abs(D.Z)) - 0.1f * Pits - 0.04f * Rills;
				Hollow = 0.8f * Notch * (1.f - FMath::Abs(D.Z)) + Pits;
				break;
			}
			case ERockKind::Ironshore:
			{
				const float Jag = Ridged(S * 7.f + Offset, 4);
				const float Basins = FMath::SmoothStep(0.55f, 0.75f, Fractal3(S * 3.f + Offset * 0.7f, 3));
				const float Top = FMath::SmoothStep(-0.2f, 0.6f, D.Z);
				R += 0.2f * (Fractal3(S * 1.6f + Offset, 3) - 0.5f) + Top * (0.2f * Jag - 0.14f * Basins);
				Hollow = Top * (Basins + 0.6f * (1.f - Jag));
				break;
			}
			case ERockKind::Beachrock:
			{
				// Joints: two sets of straight cracks across the slab; the slab's top worn smooth, its edges crumbly.
				const float JA = FMath::Abs(FMath::Frac(FVector3f::DotProduct(S, JointA) * 2.2f + BedPhase) - 0.5f);
				const float JB = FMath::Abs(FMath::Frac(FVector3f::DotProduct(S, JointB) * 1.6f + BedPhase * 1.7f) - 0.5f);
				const float Joint = FMath::Max(FMath::SmoothStep(0.44f, 0.5f, JA), FMath::SmoothStep(0.45f, 0.5f, JB));
				const float Crumble = (1.f - FMath::Abs(D.Z)) * (Fractal3(S * 8.f + Offset, 3) - 0.5f);
				R += 0.03f * (Fractal3(S * 3.f + Offset, 3) - 0.5f) - 0.035f * Joint * FMath::Max(D.Z, 0.f) + 0.06f * Crumble;
				Hollow = Joint * FMath::Max(D.Z, 0.f) + FMath::Max(-Crumble, 0.f) * 3.f;
				break;
			}
			case ERockKind::Volcanic:
			{
				const float Vesicles = FMath::SmoothStep(0.7f, 0.85f, Fractal3(S * 14.f + Offset, 2));
				R += 0.22f * (Fractal3(S * 1.5f + Offset, 4) - 0.5f) + 0.05f * (Fractal3(S * 6.f + Offset, 3) - 0.5f) - 0.02f * Vesicles;
				Hollow = Vesicles;
				break;
			}
			case ERockKind::CoralRubble:
			{
				const float Knobs = FMath::Abs(Fractal3(S * 6.f + Offset, 3) - 0.5f) * 2.f;
				R += 0.25f * (Fractal3(S * 2.f + Offset, 3) - 0.5f) + 0.12f * Knobs;
				Hollow = 1.f - Knobs;
				break;
			}
			default:
				R += 0.06f * (Fractal3(S * 2.f + Offset, 3) - 0.5f);
				break;
			}
			FVector3f Position = D * R * 100.f;
			// A flat base, a little below the ground (sinks in when placed).
			Position.Z = FMath::Max(Position.Z, -Axes.Z * 65.f);
			Positions.Add(Position);
			Weathering.Add(Hollow);
		}
		const TArray<FVector3f> Normals = SmoothNormals(Positions, Triangles);
		for (int32 Index = 0; Index < Positions.Num(); ++Index)
		{
			const FVector3f& Position = Positions[Index];
			// Lighter weathered rinds on the tops, darker in the hollows and on the undersides; mottled.
			const float Up = Normals[Index].Z;
			const float Tone = FMath::Clamp(0.55f + 0.3f * Up - 0.45f * Weathering[Index] + 0.35f * (Fractal3(Position * 0.02f + Offset, 3) - 0.5f), 0.f, 1.f);
			FLinearColor Color = FMath::Lerp(Dark, Light, Tone);
			// Orange-brown iron stains here and there on the limestones.
			if (Kind == ERockKind::Limestone || Kind == ERockKind::Beachrock)
			{
				const float Stain = FMath::SmoothStep(0.62f, 0.78f, Fractal3(Position * 0.012f + Offset * 2.f, 3));
				Color = FMath::Lerp(Color, Color * FLinearColor(1.15f, 0.85f, 0.6f), Stain * 0.7f);
			}
			Data.AddVertex(Position, Normals[Index], FVector2f(Position.X * 0.01f, Position.Y * 0.01f), Color);
		}
		for (int32 I = 0; I < Triangles.Num(); I += 3)
		{
			Data.AddTriangle(0, Triangles[I], Triangles[I + 2], Triangles[I + 1]);
		}
		return Data;
	}

	void MergeSection(FOceanMeshData& Data, int32 From, int32 To)
	{
		if (Data.Sections.IsValidIndex(From) && Data.Sections.IsValidIndex(To) && From != To)
		{
			Data.Sections[To].Append(Data.Sections[From]);
			Data.Sections[From].Reset();
		}
	}

	UStaticMesh* BuildStaticMesh(UObject* Outer, const FOceanMeshData& Data, const TArray<UMaterialInterface*>& Materials, int32 ColorSection)
	{
		return BuildStaticMeshLods(Outer, { &Data }, Materials, { 1.f }, { ColorSection });
	}

	namespace
	{
		/**
		 * ColorSection: the section whose vertex colours must show. Measured on these runtime-built meshes (UE 5.8):
		 * only the second polygon group showed its vertex colours, the others drew white whatever their material
		 * (a copy of the working foliage material included). So that section gets a duplicate of one of its own
		 * triangles as a first group, and comes second. -1: none (textured cards were tuned on the white colours).
		 */
		void FillDescription(FMeshDescription& Description, const FOceanMeshData& Data, int32 ColorSection)
		{
			FStaticMeshAttributes Attributes(Description);
			Attributes.Register();
			TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
			TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
			TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
			UVs.SetNumChannels(3); // 1 and 2: wind data (FOceanMeshData::FVertex::Wind)
			TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
			TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
			Description.ReserveNewVertices(Data.Vertices.Num());
			Description.ReserveNewVertexInstances(Data.Vertices.Num());
			TArray<FVertexInstanceID> Instances;
			Instances.Reserve(Data.Vertices.Num());
			for (const FOceanMeshData::FVertex& Vertex : Data.Vertices)
			{
				const FVertexID VertexID = Description.CreateVertex();
				Positions[VertexID] = Vertex.Position;
				const FVertexInstanceID Instance = Description.CreateVertexInstance(VertexID);
				Normals[Instance] = Vertex.Normal;
				const FVector3f Helper = FMath::Abs(Vertex.Normal.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector;
				Tangents[Instance] = FVector3f::CrossProduct(Helper, Vertex.Normal).GetSafeNormal();
				BinormalSigns[Instance] = 1.f;
				UVs.Set(Instance, 0, Vertex.UV);
				UVs.Set(Instance, 1, FVector2f(Vertex.Wind.X, Vertex.Wind.Y));
				UVs.Set(Instance, 2, FVector2f(Vertex.Wind.Z, Vertex.Wind.W));
				// The mesh build stores the colours sRGB-encoded (FLinearColor::ToFColor(true)) and the material's Vertex
				// Color reads those bytes as they are: decoded here, the material gets the linear albedo it was given
				// (encoded, a 0.2 albedo drew as 0.48: the stones and coconuts white, every plant pale). Only for the
				// meshes whose colour is their albedo (those with a ColorSection): the textured cards' colours are tints
				// tuned on screen as they draw.
				auto Decode = [bAlbedo = Data.Sections.IsValidIndex(ColorSection)](float C)
				{
					C = FMath::Clamp(C, 0.f, 1.f);
					return !bAlbedo ? C : C <= 0.04045f ? C / 12.92f : FMath::Pow((C + 0.055f) / 1.055f, 2.4f);
				};
				// Alpha passes through: an occlusion term for the materials that read it (M_LeafBlade).
				Colors[Instance] = FVector4f(Decode(Vertex.Color.R), Decode(Vertex.Color.G), Decode(Vertex.Color.B), FMath::Clamp(Vertex.Color.A, 0.f, 1.f));
				Instances.Add(Instance);
			}
			// One polygon group per section, in order, even when empty: the runtime build gives each group the material of
			// its index (its slot name aside), so a skipped, emptied or reordered group shifted every material after it (trunks
			// drawn with the vertex-colour material, 3D leaves with the leaf-card texture: the "chewed" leaves).
			for (int32 Section = 0; Section < Data.Sections.Num(); ++Section)
			{
				const TArray<int32>& Indices = Data.Sections[Section];
				const FPolygonGroupID Group = Description.CreatePolygonGroup();
				SlotNames[Group] = FName(*FString::Printf(TEXT("Slot%d"), Section));
				if (Indices.Num() < 3 && Instances.Num() > 0)
				{
					// The build drops empty groups (and the rest move down an index): a speck of a triangle keeps this one.
					FVertexInstanceID Speck[3];
					for (int32 Corner = 0; Corner < 3; ++Corner)
					{
						const FVertexID VertexID = Description.CreateVertex();
						Positions[VertexID] = Data.Vertices[0].Position + FVector3f(Corner == 1 ? 0.01f : 0.f, Corner == 2 ? 0.01f : 0.f, 0.f);
						Speck[Corner] = Description.CreateVertexInstance(VertexID);
						Normals[Speck[Corner]] = FVector3f::UpVector;
						Tangents[Speck[Corner]] = FVector3f::ForwardVector;
						BinormalSigns[Speck[Corner]] = 1.f;
						UVs.Set(Speck[Corner], 0, FVector2f::ZeroVector);
						Colors[Speck[Corner]] = FVector4f(0.f, 0.f, 0.f, 1.f);
					}
					Description.CreateTriangle(Group, { Speck[0], Speck[1], Speck[2] });
					continue;
				}
				for (int32 I = 0; I + 2 < Indices.Num(); I += 3)
				{
					Description.CreateTriangle(Group, { Instances[Indices[I]], Instances[Indices[I + 1]], Instances[Indices[I + 2]] });
				}
			}
		}
	}

	UStaticMesh* BuildStaticMeshLods(UObject* Outer, const TArray<const FOceanMeshData*>& Lods, const TArray<UMaterialInterface*>& Materials,
		const TArray<float>& ScreenSizes, const TArray<int32>& ColorSections)
	{
		TArray<FMeshDescription> Descriptions;
		Descriptions.SetNum(Lods.Num());
		TArray<const FMeshDescription*> Pointers;
		int32 Slots = 0;
		for (int32 Lod = 0; Lod < Lods.Num(); ++Lod)
		{
			FillDescription(Descriptions[Lod], *Lods[Lod], ColorSections.IsValidIndex(Lod) ? ColorSections[Lod] : -1);
			Pointers.Add(&Descriptions[Lod]);
			Slots = FMath::Max(Slots, Lods[Lod]->Sections.Num());
		}
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, NAME_None, RF_Transient);
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			const FName SlotName(*FString::Printf(TEXT("Slot%d"), Slot));
			Mesh->GetStaticMaterials().Add(FStaticMaterial(Materials.IsValidIndex(Slot) ? Materials[Slot] : nullptr, SlotName, SlotName));
		}
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bFastBuild = true; // the runtime path: works in the editor and in a cooked game
		Params.bBuildSimpleCollision = false;
		Params.bCommitMeshDescription = false;
		Params.bMarkPackageDirty = false;
		Mesh->BuildFromMeshDescriptions(Pointers, Params);
		// Screen size (share of the screen height) below which each level is replaced by the next.
		if (FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
		{
			for (int32 Lod = 1; Lod < Lods.Num() && Lod < MAX_STATIC_MESH_LODS; ++Lod)
			{
				RenderData->ScreenSize[Lod].Default = ScreenSizes.IsValidIndex(Lod) ? ScreenSizes[Lod] : FMath::Pow(0.5f, float(Lod));
			}
		}
		return Mesh;
	}
}

#if WITH_EDITOR
UStaticMesh* OceanFlora::BakeNaniteMesh(const FString& PackagePath, const FOceanMeshData& Data, const TArray<UMaterialInterface*>& Materials,
	int32 ColorSection, bool bFoliage)
{
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
	UPackage* Package = CreatePackage(*PackagePath);
	Package->FullyLoad();
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	for (int32 Slot = 0; Slot < Data.Sections.Num(); ++Slot)
	{
		const FName SlotName(*FString::Printf(TEXT("Slot%d"), Slot));
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Materials.IsValidIndex(Slot) ? Materials[Slot] : nullptr, SlotName, SlotName));
	}
	FMeshDescription Description;
	FillDescription(Description, Data, ColorSection);
	FStaticMeshSourceModel& Source = Mesh->AddSourceModel();
	// The generators give normals and tangents; the speck triangles that keep empty sections must survive.
	Source.BuildSettings.bRecomputeNormals = false;
	Source.BuildSettings.bRecomputeTangents = false;
	Source.BuildSettings.bRemoveDegenerates = false;
	Source.BuildSettings.bGenerateLightmapUVs = false;
	Mesh->CreateMeshDescription(0, MoveTemp(Description));
	Mesh->CommitMeshDescription(0);
	FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
	Nanite.bEnabled = true;
	Nanite.ShapePreservation = bFoliage ? ENaniteShapePreservation::Voxelize : ENaniteShapePreservation::None;
	Mesh->SetNaniteSettings(Nanite);
	Mesh->Build(true);
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Mesh);
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	UPackage::SavePackage(Package, Mesh, *FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension()), Args);
	// Saved: its source geometry (up to a gigabyte for a bush) is no longer needed in this session; kept, a whole bake ran
	// out of memory.
	Mesh->ClearMeshDescriptions();
	return Mesh;
}
#endif
