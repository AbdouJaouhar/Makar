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
		// Leaf masses as textured cards (AddLeafCards) rather than lumpy balls.
		bool bLeafCards = false;
		float CardSize = 140.f;    // cm
		FLinearColor CardTint = FLinearColor(0.5f, 0.5f, 0.5f);
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
		TArray<FState> Stack;
		auto Jittered = [&](float Degrees) { return FMath::DegreesToRadians(Degrees * (1.f + Random.FRandRange(-Params.Jitter, Params.Jitter))); };

		for (const TCHAR Symbol : Program)
		{
			switch (Symbol)
			{
			case TEXT('F'):
			{
				const FVector3f Heading = State.Orientation.RotateVector(FVector3f::UpVector);
				const FVector3f End = State.Position + Heading * State.Length;
				const float EndRadius = State.Radius * 0.85f;
				AddTube(Data, SectionBark, State.Position, End, State.Radius, EndRadius, State.Radius > 8.f ? 7 : 5, Params.Bark, Params.Bark * 1.1f);
				State.Position = End;
				State.Radius = EndRadius;
				// Phototropism: branches bend towards the light.
				const FVector3f Axis = FVector3f::CrossProduct(Heading, FVector3f::UpVector);
				if (Axis.SizeSquared() > 1e-6f)
				{
					State.Orientation = FQuat4f(Axis.GetSafeNormal(), Params.Phototropism * Axis.Size()) * State.Orientation;
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
				if (Params.bLeafCards)
				{
					AddLeafCards(Data, SectionFoliage, State.Position + FVector3f(0.f, 0.f, Radius * 0.3f), Radius, Params.CardSize, Random, Params.CardTint);
				}
				else
				{
					AddLeafCluster(Data, State.Position + FVector3f(0.f, 0.f, Radius * 0.3f), Radius, Random, Params.Leaf, Params.LeafAccent, Params.AccentAmount);
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
		const float Height = Random.FRandRange(750.f, 1150.f);
		const float Lean = Random.FRandRange(0.12f, 0.4f);
		auto TrunkPoint = [&](float T)
		{
			const float Out = Lean * Height * (1.f - (1.f - T) * (1.f - T)) * 0.8f;
			return FVector3f(Out, 0.f, Height * T - 20.f); // base slightly sunk in the sand
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

		// Fronds: an arching rachis with leaflets hanging on both sides; older, lower fronds droop and yellow.
		const int32 Fronds = Random.RandRange(14, 18);
		for (int32 Frond = 0; Frond < Fronds; ++Frond)
		{
			const float Azimuth = 2.f * UE_PI * Frond / Fronds + Random.FRandRange(-0.2f, 0.2f);
			const float Age = Random.FRand(); // 0 = young, upright; 1 = old, drooping
			// The oldest fronds die back: brown, dry, hanging straight down along the trunk.
			const bool bDead = Age > 0.88f;
			const float Length = Random.FRandRange(380.f, 500.f);
			const float StartPitch = FMath::DegreesToRadians(bDead ? -60.f : FMath::Lerp(55.f, -5.f, Age));
			const float Droop = FMath::DegreesToRadians(bDead ? 40.f : FMath::Lerp(55.f, 95.f, Age));
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
			FVector3f Position = Top;
			int32 Previous[3] = { INDEX_NONE, INDEX_NONE, INDEX_NONE };
			for (int32 Step = 0; Step <= Steps; ++Step)
			{
				const float S = float(Step) / Steps;
				const float Pitch = StartPitch - Droop * FMath::Pow(S, 1.5f);
				const FVector3f Direction = (Outward * FMath::Cos(Pitch) + FVector3f(0.f, 0.f, FMath::Sin(Pitch))).GetSafeNormal();
				const FVector3f Side = FVector3f::CrossProduct(Direction, FVector3f::UpVector).GetSafeNormal();
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

	FOceanMeshData MakeBroadleafTree(int32 Seed)
	{
		FRandomStream Random(Seed);
		FOceanMeshData Data(2);
		// Trunk, then apices branching in two or three, with leaves along the upper branches too, so the
		// crown is one dense, lumpy mass rather than a few balls on sticks.
		const TArray<FLSystemRule> Rules = {
			{ TEXT('A'), 0.6f, TEXT("F!'[&FLA]/////[&FLA]///////[&FLA]") },
			{ TEXT('A'), 0.3f, TEXT("F!'[&FLA]////////[&FLA]") },
			{ TEXT('A'), 0.1f, TEXT("F!'L") },
		};
		const FString Program = RewriteLSystem(TEXT("FFA"), Rules, 3, Random);
		FTurtleParams Params;
		Params.Length = Random.FRandRange(150.f, 210.f);
		Params.Radius = Random.FRandRange(14.f, 20.f);
		Params.BranchAngle = Random.FRandRange(30.f, 42.f);
		Params.LeafRadius = Random.FRandRange(130.f, 170.f);
		Params.Leaf = FLinearColor(0.03f, 0.08f, 0.018f);
		Params.LeafAccent = FLinearColor(0.07f, 0.12f, 0.025f);
		// Leaf cards (M_LeafCards_Broadleaf): leaves ~30 cm, like the tropical almond's; each tree its own shade.
		Params.bLeafCards = true;
		Params.CardSize = Random.FRandRange(125.f, 150.f);
		Params.CardTint = FLinearColor(0.5f * Random.FRandRange(0.9f, 1.05f), 0.5f * Random.FRandRange(0.92f, 1.08f), 0.5f * Random.FRandRange(0.85f, 1.f));
		InterpretLSystem(Data, Program, Params, Random);
		return Data;
	}

	FOceanMeshData MakeSeaGrape(int32 Seed)
	{
		FRandomStream Random(Seed);
		FOceanMeshData Data(2);
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
		Params.LeafRadius = Random.FRandRange(55.f, 80.f);
		Params.Bark = FLinearColor(0.18f, 0.15f, 0.12f);
		Params.Leaf = FLinearColor(0.08f, 0.15f, 0.04f);
		Params.LeafAccent = FLinearColor(0.24f, 0.11f, 0.05f); // red-veined old leaves
		Params.AccentAmount = 0.35f;
		// Leaf cards (M_LeafCards_SeaGrape): round leaves ~20 cm along zigzag stems, a few turning red.
		Params.bLeafCards = true;
		Params.CardSize = Random.FRandRange(95.f, 115.f);
		Params.CardTint = FLinearColor(0.5f * Random.FRandRange(0.92f, 1.05f), 0.5f * Random.FRandRange(0.95f, 1.05f), 0.5f);
		InterpretLSystem(Data, Program, Params, Random);
		return Data;
	}

	FOceanMeshData MakeCoastalBush(int32 Seed)
	{
		FRandomStream Random(Seed);
		FOceanMeshData Data(2);
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
		Params.LeafRadius = Random.FRandRange(65.f, 90.f);
		Params.Bark = FLinearColor(0.17f, 0.15f, 0.12f);
		Params.bLeafCards = true;
		Params.CardSize = Random.FRandRange(85.f, 105.f);
		Params.CardTint = FLinearColor(0.5f * Random.FRandRange(0.85f, 1.f), 0.5f * Random.FRandRange(0.9f, 1.05f), 0.5f * Random.FRandRange(0.85f, 1.f));
		InterpretLSystem(Data, Program, Params, Random);
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

	UStaticMesh* BuildStaticMesh(UObject* Outer, const FOceanMeshData& Data, const TArray<UMaterialInterface*>& Materials)
	{
		FMeshDescription Description;
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

		UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, NAME_None, RF_Transient);
		for (int32 Section = 0; Section < Data.Sections.Num(); ++Section)
		{
			const FName SlotName(*FString::Printf(TEXT("Slot%d"), Section));
			const FPolygonGroupID Group = Description.CreatePolygonGroup();
			SlotNames[Group] = SlotName;
			const TArray<int32>& Indices = Data.Sections[Section];
			for (int32 I = 0; I + 2 < Indices.Num(); I += 3)
			{
				Description.CreateTriangle(Group, { Instances[Indices[I]], Instances[Indices[I + 1]], Instances[Indices[I + 2]] });
			}
			Mesh->GetStaticMaterials().Add(FStaticMaterial(Materials.IsValidIndex(Section) ? Materials[Section] : nullptr, SlotName, SlotName));
		}

		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bFastBuild = true; // the runtime path: works in the editor and in a cooked game
		Params.bBuildSimpleCollision = false;
		Params.bCommitMeshDescription = false;
		Params.bMarkPackageDirty = false;
		Mesh->BuildFromMeshDescriptions({ &Description }, Params);
		return Mesh;
	}
}
