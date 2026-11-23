#include "Ocean/OceanFlora.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

int32 FOceanMeshData::AddVertex(const FVector3f& Position, const FVector3f& Normal, const FVector2f& UV, const FLinearColor& Color)
{
	return Vertices.Add({ Position, Normal.GetSafeNormal(), UV, Color });
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
		int32 LeftInTwig = 0;
		for (int32 Leaf = 0; Leaf < Leaves; ++Leaf)
		{
			if (LeftInTwig-- <= 0)
			{
				LeftInTwig = Random.RandRange(5, 9);
				Direction = FVector3f(Random.GetUnitVector());
				Depth = FMath::Sqrt(Random.FRandRange(0.2f, 1.f));
				TwigPoint = Center + FVector3f(Direction.X, Direction.Y, Direction.Z * 0.7f) * (Radius * Depth);
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
			const FVector3f Root = Base + Axis * (0.12f * Length);
			const FVector3f Mid = Root + Axis * (0.5f * Length) + Face * (0.02f * Length);
			const FVector3f Tip = Root + Axis * Length - FVector3f::UpVector * Sag;
			const FVector3f Left = Root + Axis * (0.45f * Length) - Side * Half - Face * Fold;
			const FVector3f Right = Root + Axis * (0.45f * Length) + Side * Half - Face * Fold;
			FLinearColor Color = FMath::Lerp(Green, Accent, AccentAmount * Random.FRand()) * Random.FRandRange(0.8f, 1.2f);
			if (Random.FRand() < RedChance)
			{
				Color = FMath::Lerp(FLinearColor(0.32f, 0.07f, 0.03f), FLinearColor(0.38f, 0.2f, 0.03f), Random.FRand());
			}
			// Inner leaves are darker (shaded by the mass).
			Color *= FMath::Lerp(0.55f, 1.f, Depth);
			const FVector3f Radial = (Base - Below).GetSafeNormal();
			auto Normal = [&](const FVector3f& Blade) { return (Blade * 0.45f + Radial * 0.55f).GetSafeNormal(); };
			const int32 First = Data.Vertices.Num();
			Data.AddVertex(Root, Normal(Face), FVector2f(0.5f, 1.f), Color * 0.85f);
			Data.AddVertex(Left, Normal((Face - Side * 0.5f).GetSafeNormal()), FVector2f(0.f, 0.5f), Color);
			Data.AddVertex(Mid, Normal(Face), FVector2f(0.5f, 0.5f), Color * 1.05f);
			Data.AddVertex(Right, Normal((Face + Side * 0.5f).GetSafeNormal()), FVector2f(1.f, 0.5f), Color);
			Data.AddVertex(Tip, Normal(Face), FVector2f(0.5f, 0.f), Color);
			Data.AddTriangle(Section, First, First + 2, First + 1);
			Data.AddTriangle(Section, First + 1, First + 2, First + 4);
			Data.AddTriangle(Section, First, First + 3, First + 2);
			Data.AddTriangle(Section, First + 3, First + 4, First + 2);
			// The petiole: a thin sliver from the twig.
			if (Leaf % 3 == 0)
			{
				const FVector3f Twig = Base - Axis * (0.1f * Length) - FVector3f::UpVector * (0.05f * Length);
				const int32 P = Data.Vertices.Num();
				Data.AddVertex(Twig, Normal(Face), FVector2f(0.5f, 1.f), Color * 0.6f);
				Data.AddVertex(Root - Side * 0.04f * Length, Normal(Face), FVector2f(0.4f, 1.f), Color * 0.7f);
				Data.AddVertex(Root + Side * 0.04f * Length, Normal(Face), FVector2f(0.6f, 1.f), Color * 0.7f);
				Data.AddTriangle(Section, P, P + 2, P + 1);
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
		FLinearColor Bark = FLinearColor(0.2f, 0.17f, 0.13f);
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
	FOceanMeshData MakeCoconutPalm(int32 Seed)
	{
		FRandomStream Random(Seed);
		FOceanMeshData Data(3); // bark, frond cards, coconuts

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
		// Bark: M_PalmBark draws the ring scars and fissures procedurally from the bark coordinates (V = metres along
		// the trunk); the vertex colour stays neutral. Slimmer at the top, flared at the foot.
		const FLinearColor TrunkColor(0.27f, 0.23f, 0.18f);
		AddBarkTube(Data, SectionBark, TrunkPoint,
			[](float T) { return FMath::Lerp(22.f, 14.f, T) * (1.f + 0.6f * FMath::Exp(-T * 25.f)); }, 24, 12, TrunkColor);
		const FVector3f Top = TrunkPoint(1.f);

		// Coconuts under the crown.
		const int32 Nuts = Random.RandRange(3, 7);
		for (int32 Nut = 0; Nut < Nuts; ++Nut)
		{
			const float Angle = Random.FRandRange(0.f, 2.f * UE_PI);
			const FVector3f Center = Top + FVector3f(FMath::Cos(Angle) * 18.f, FMath::Sin(Angle) * 18.f, -25.f);
			FRandomStream NutRandom(Seed * 31 + Nut);
			AddLeafCluster(Data, Center, 11.f, NutRandom, FLinearColor(0.16f, 0.15f, 0.04f), FLinearColor(0.25f, 0.17f, 0.06f), 0.6f, SectionNuts);
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
			// Vertex colour = tint over the rendered frond (M_PalmFrond divides by the young green (0.12, 0.2, 0.04)):
			// young fronds a little yellower than the render, old ones yellow, dead ones straw-brown.
			const FLinearColor Green = bDead ? FLinearColor(0.2f, 0.11f, 0.045f)
				: FMath::Lerp(FLinearColor(0.17f, 0.18f, 0.03f), FLinearColor(0.22f, 0.18f, 0.035f), Age * Age);

			// A leaf card along the arching rachis, textured with a frond rendered from Infinigen's coconut palm
			// (Scripts/infinigen/render_frond_atlas.py: base at V = 1, tip at V = 0, the frond spanning
			// U 0.27-0.73 of the square image). Three vertices across: the leaflets droop on both sides of the
			// rachis, more towards the tip, so the card has a V cross-section instead of being a flat ribbon.
			const FLinearColor Color = Green * Random.FRandRange(0.85f, 1.15f);
			const float HalfWidth = 0.24f * Length;
			const int32 Steps = 12;
			// Older fronds spring from lower on the crown shaft.
			FVector3f Position = Top + Outward * 8.f - FVector3f(0.f, 0.f, 35.f * Age);
			int32 Previous[3] = { INDEX_NONE, INDEX_NONE, INDEX_NONE };
			for (int32 Step = 0; Step <= Steps; ++Step)
			{
				const float S = float(Step) / Steps;
				const float Pitch = StartPitch - Droop * FMath::Pow(S, 1.5f);
				const FVector3f Direction = (Outward * FMath::Cos(Pitch) + FVector3f(0.f, 0.f, FMath::Sin(Pitch))).GetSafeNormal();
				const FVector3f Flat = FVector3f::CrossProduct(Direction, FVector3f::UpVector).GetSafeNormal();
				const FVector3f FlatUp = FVector3f::CrossProduct(Flat, Direction).GetSafeNormal();
				const float Twist = Roll * (0.4f + 0.6f * S);
				const FVector3f Side = Flat * FMath::Cos(Twist) + FlatUp * FMath::Sin(Twist);
				const FVector3f Up = FVector3f::CrossProduct(Side, Direction).GetSafeNormal();
				const float Sag = HalfWidth * FMath::Lerp(0.25f, 0.6f, S);
				const float V = FMath::Lerp(0.99f, 0.01f, S);
				const FVector3f Left = Position - Side * HalfWidth - Up * Sag;
				const FVector3f Right = Position + Side * HalfWidth - Up * Sag;
				const FVector3f LeftNormal = (Up + Side * 0.4f).GetSafeNormal();
				const FVector3f RightNormal = (Up - Side * 0.4f).GetSafeNormal();
				const int32 Row[3] = {
					Data.AddVertex(Left, LeftNormal, FVector2f(0.27f, V), Color),
					Data.AddVertex(Position, Up, FVector2f(0.5f, V), Color),
					Data.AddVertex(Right, RightNormal, FVector2f(0.73f, V), Color) };
				if (Step > 0)
				{
					for (int32 Column = 0; Column < 2; ++Column)
					{
						Data.AddTriangle(SectionFoliage, Previous[Column], Row[Column], Previous[Column + 1]);
						Data.AddTriangle(SectionFoliage, Previous[Column + 1], Row[Column], Row[Column + 1]);
					}
				}
				FMemory::Memcpy(Previous, Row, sizeof(Row));
				Position += Direction * (Length / Steps);
			}
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
		Params.Bark = FLinearColor(0.18f, 0.15f, 0.12f);
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
		Params.Bark = FLinearColor(0.17f, 0.15f, 0.12f);
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

	FOceanMeshData MakeFallenFrond(int32 Seed)
	{
		// A dead coconut frond lying under its palm: the frond card (M_PalmFrond, the leaflets cut out by the texture's
		// alpha) laid along +X from its base, gently bowed sideways, the rachis a little off the ground (thicker at the
		// base), the leaflets' tips on the sand. Straw-brown to grey-brown.
		FRandomStream Random(Seed);
		FOceanMeshData Data(1);
		const float Length = Random.FRandRange(350.f, 470.f);
		const float HalfWidth = 0.24f * Length; // as on the palm (the frond spans U 0.27-0.73 of the image)
		const float Bow = Random.FRandRange(-0.3f, 0.3f);
		const FLinearColor Color = FMath::Lerp(FLinearColor(0.2f, 0.11f, 0.045f), FLinearColor(0.24f, 0.18f, 0.09f), Random.FRand());
		const int32 Steps = 10;
		int32 Previous[3] = { INDEX_NONE, INDEX_NONE, INDEX_NONE };
		FVector3f Position = FVector3f::ZeroVector;
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			const float S = float(Step) / Steps;
			const float Angle = Bow * S;
			const FVector3f Direction(FMath::Cos(Angle), FMath::Sin(Angle), 0.f);
			const FVector3f Side(-Direction.Y, Direction.X, 0.f);
			const FVector3f Rachis = Position + FVector3f(0.f, 0.f, FMath::Lerp(14.f, 3.f, S));
			const float V = FMath::Lerp(0.99f, 0.01f, S);
			const int32 Row[3] = {
				Data.AddVertex(Position - Side * HalfWidth + FVector3f(0.f, 0.f, 1.f), FVector3f::UpVector, FVector2f(0.27f, V), Color),
				Data.AddVertex(Rachis, FVector3f::UpVector, FVector2f(0.5f, V), Color),
				Data.AddVertex(Position + Side * HalfWidth + FVector3f(0.f, 0.f, 1.f), FVector3f::UpVector, FVector2f(0.73f, V), Color) };
			if (Step > 0)
			{
				for (int32 Column = 0; Column < 2; ++Column)
				{
					Data.AddTriangle(0, Previous[Column], Row[Column], Previous[Column + 1]);
					Data.AddTriangle(0, Previous[Column + 1], Row[Column], Row[Column + 1]);
				}
			}
			FMemory::Memcpy(Previous, Row, sizeof(Row));
			Position += Direction * (Length / Steps);
		}
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
		const FLinearColor Blue(0.13f, 0.2f, 0.15f), Edge(0.1f, 0.13f, 0.08f), Dry(0.24f, 0.18f, 0.1f);
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
		// Turk's cap cactus (Melocactus intortus), on the limestone of Pointe des Chateaux and the Grenadines: a ribbed
		// green globe crowned with a woolly red cephalium. Bark section (vertex colour).
		FRandomStream Random(Seed);
		FOceanMeshData Data(1);
		const float R = Random.FRandRange(12.f, 19.f);
		const int32 Ribs = Random.RandRange(12, 15);
		const FLinearColor Green(0.04f, 0.09f, 0.04f), Rib(0.08f, 0.14f, 0.07f);
		AddRibbedColumn(Data, 0, [&](float T) { return FVector3f(0.f, 0.f, -3.f + 1.8f * R * T); },
			[&](float T) { return R * FMath::Max(FMath::Sin(UE_PI * FMath::Lerp(0.18f, 0.92f, T)), 0.35f); }, Ribs, 0.14f, 10, 3 * Ribs, Green, Rib);
		const float CapHeight = R * Random.FRandRange(0.5f, 1.3f);
		AddRibbedColumn(Data, 0, [&](float T) { return FVector3f(0.f, 0.f, 1.5f * R + CapHeight * T); },
			[&](float T) { return 0.55f * R; }, 24, 0.08f, 6, 24, FLinearColor(0.35f, 0.08f, 0.07f), FLinearColor(0.5f, 0.3f, 0.26f));
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
			const FLinearColor Color = (bRed && Age < 0.35f ? Heart : Green * Random.FRandRange(0.85f, 1.2f));
			AddBlade(Data, 1, Out * 2.f + FVector3f(0.f, 0.f, 3.f), Out, Length, FMath::DegreesToRadians(FMath::Lerp(72.f, 30.f, Age)),
				FMath::DegreesToRadians(FMath::Lerp(40.f, 95.f, Age)), Random.FRandRange(4.5f, 7.f), 0.35f,
				FMath::DegreesToRadians(Random.FRandRange(-20.f, 20.f)), 0, 6, Color, Color * 0.8f);
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
			Light = FLinearColor(0.62f, 0.58f, 0.5f); Dark = FLinearColor(0.3f, 0.29f, 0.26f);
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
			Light = FLinearColor(0.8f, 0.77f, 0.7f); Dark = FLinearColor(0.5f, 0.47f, 0.42f);
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

	UStaticMesh* BuildStaticMesh(UObject* Outer, const FOceanMeshData& Data, const TArray<UMaterialInterface*>& Materials)
	{
		return BuildStaticMeshLods(Outer, { &Data }, Materials, { 1.f });
	}

	namespace
	{
		void FillDescription(FMeshDescription& Description, const FOceanMeshData& Data)
		{
			FStaticMeshAttributes Attributes(Description);
			Attributes.Register();
			TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
			TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
			TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
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
				Colors[Instance] = FVector4f(Vertex.Color.R, Vertex.Color.G, Vertex.Color.B, 1.f);
				Instances.Add(Instance);
			}
			for (int32 Section = 0; Section < Data.Sections.Num(); ++Section)
			{
				const TArray<int32>& Indices = Data.Sections[Section];
				if (Indices.Num() < 3)
				{
					continue;
				}
				const FPolygonGroupID Group = Description.CreatePolygonGroup();
				SlotNames[Group] = FName(*FString::Printf(TEXT("Slot%d"), Section));
				for (int32 I = 0; I + 2 < Indices.Num(); I += 3)
				{
					Description.CreateTriangle(Group, { Instances[Indices[I]], Instances[Indices[I + 1]], Instances[Indices[I + 2]] });
				}
			}
		}
	}

	UStaticMesh* BuildStaticMeshLods(UObject* Outer, const TArray<const FOceanMeshData*>& Lods, const TArray<UMaterialInterface*>& Materials,
		const TArray<float>& ScreenSizes)
	{
		TArray<FMeshDescription> Descriptions;
		Descriptions.SetNum(Lods.Num());
		TArray<const FMeshDescription*> Pointers;
		int32 Slots = 0;
		for (int32 Lod = 0; Lod < Lods.Num(); ++Lod)
		{
			FillDescription(Descriptions[Lod], *Lods[Lod]);
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
