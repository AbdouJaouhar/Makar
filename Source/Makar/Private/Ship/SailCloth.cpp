#include "SailCloth.h"

#include "Async/ParallelFor.h"

namespace
{
	FVector3f ClothClosest(const FVector3f& P, const FVector3f& A, const FVector3f& B)
	{
		const FVector3f AB = B - A;
		const float Length2 = AB.SizeSquared();
		const float T = Length2 > 1e-8f ? FMath::Clamp(FVector3f::DotProduct(P - A, AB) / Length2, 0.f, 1.f) : 0.f;
		return A + AB * T;
	}
}

namespace
{
	/** Points over a triangle no more than Step apart. */
	template <typename FVisit>
	void SampleTriangle(const FVector3f& A, const FVector3f& B, const FVector3f& C, float Step, FVisit&& Visit)
	{
		const int32 NU = FMath::Clamp(FMath::CeilToInt((B - A).Size() / Step), 1, 4000);
		const int32 NV = FMath::Clamp(FMath::CeilToInt((C - A).Size() / Step), 1, 4000);
		for (int32 I = 0; I <= NU; ++I)
		{
			const float S = float(I) / NU;
			const int32 JMax = FMath::Min(NV, FMath::CeilToInt((1.f - S) * NV));
			for (int32 J = 0; J <= JMax; ++J)
			{
				Visit(A + (B - A) * S + (C - A) * FMath::Min(float(J) / NV, 1.f - S));
			}
		}
	}
}

void FShipVoxels::Reset(const FBox3f& Bounds, float InCell)
{
	Cell = InCell;
	Origin = Bounds.Min;
	const FVector3f Extent = Bounds.GetSize() / (Cell * 8.f);
	Bricks = FIntVector(FMath::Clamp(FMath::CeilToInt(Extent.X) + 1, 1, 400), FMath::Clamp(FMath::CeilToInt(Extent.Y) + 1, 1, 400), FMath::Clamp(FMath::CeilToInt(Extent.Z) + 1, 1, 400));
	Touched.SetNumZeroed(Bricks.X * Bricks.Y * Bricks.Z);
	Top.Init(-1, Bricks.X * Bricks.Y * Bricks.Z);
	Where.Reset();
	Cells.Reset();
}

void FShipVoxels::Touch(const FVector3f& A, const FVector3f& B, const FVector3f& C)
{
	const float Size = Cell * 8.f;
	SampleTriangle(A, B, C, Size * 0.5f, [&](const FVector3f& P)
	{
		const FVector3f Q = (P - Origin) / Size;
		const int32 X = FMath::FloorToInt(Q.X), Y = FMath::FloorToInt(Q.Y), Z = FMath::FloorToInt(Q.Z);
		if (X >= 0 && Y >= 0 && Z >= 0 && X < Bricks.X && Y < Bricks.Y && Z < Bricks.Z)
		{
			Touched[(Z * Bricks.Y + Y) * Bricks.X + X] = 1; // the same value from any thread
		}
	});
}

void FShipVoxels::Allocate()
{
	// A brick wherever something is drawn and round it (the grown cells may spill into the next brick).
	for (int32 Z = 0; Z < Bricks.Z; ++Z)
	{
		for (int32 Y = 0; Y < Bricks.Y; ++Y)
		{
			for (int32 X = 0; X < Bricks.X; ++X)
			{
				bool bNear = false;
				for (int32 DZ = -1; DZ <= 1 && !bNear; ++DZ)
				{
					for (int32 DY = -1; DY <= 1 && !bNear; ++DY)
					{
						for (int32 DX = -1; DX <= 1 && !bNear; ++DX)
						{
							const int32 I = X + DX, J = Y + DY, K = Z + DZ;
							bNear = I >= 0 && J >= 0 && K >= 0 && I < Bricks.X && J < Bricks.Y && K < Bricks.Z && Touched[(K * Bricks.Y + J) * Bricks.X + I];
						}
					}
				}
				if (bNear)
				{
					Top[(Z * Bricks.Y + Y) * Bricks.X + X] = Where.Num();
					Where.Add(FIntVector(X, Y, Z));
				}
			}
		}
	}
	Cells.SetNumZeroed(Where.Num() * 512);
	Touched.Empty();
}

void FShipVoxels::Fill(const FVector3f& A, const FVector3f& B, const FVector3f& C, uint8 Kind)
{
	// Samples 0.8 cell apart: a cell skipped at a corner is filled by Grow.
	SampleTriangle(A, B, C, Cell * 0.8f, [&](const FVector3f& P)
	{
		const FVector3f Q = (P - Origin) / Cell;
		const int32 X = FMath::FloorToInt(Q.X), Y = FMath::FloorToInt(Q.Y), Z = FMath::FloorToInt(Q.Z);
		if (X < 0 || Y < 0 || Z < 0)
		{
			return;
		}
		const int32 BX = X >> 3, BY = Y >> 3, BZ = Z >> 3;
		if (BX >= Bricks.X || BY >= Bricks.Y || BZ >= Bricks.Z)
		{
			return;
		}
		const int32 Brick = Top[(BZ * Bricks.Y + BY) * Bricks.X + BX];
		if (Brick >= 0)
		{
			Cells[Brick * 512 + ((Z & 7) * 8 + (Y & 7)) * 8 + (X & 7)] |= uint8(1 | Kind); // a kind lost to a race only blurs the diagnostics
		}
	});
}

void FShipVoxels::Grow()
{
	ParallelFor(Where.Num(), [this](int32 Brick)
	{
		const FIntVector Base = Where[Brick] * 8;
		for (int32 Z = 0; Z < 8; ++Z)
		{
			for (int32 Y = 0; Y < 8; ++Y)
			{
				for (int32 X = 0; X < 8; ++X)
				{
					uint8& C = Cells[Brick * 512 + (Z * 8 + Y) * 8 + X];
					if (C & 1)
					{
						continue;
					}
					const int32 I = Base.X + X, J = Base.Y + Y, K = Base.Z + Z;
					// Only the drawn bit is read: no cell grows from a grown one.
					if ((GetCell(I - 1, J, K) | GetCell(I + 1, J, K) | GetCell(I, J - 1, K) | GetCell(I, J + 1, K) | GetCell(I, J, K - 1) | GetCell(I, J, K + 1)) & 1)
					{
						C |= 2;
					}
				}
			}
		}
	});
}

bool FShipVoxels::NearestFree(const FVector3f& P, int32 Radius, FVector3f& Out) const
{
	const FVector3f Q = (P - Origin) / Cell;
	const int32 X = FMath::FloorToInt(Q.X), Y = FMath::FloorToInt(Q.Y), Z = FMath::FloorToInt(Q.Z);
	float Best = 1e9f;
	for (int32 DZ = -Radius; DZ <= Radius; ++DZ)
	{
		for (int32 DY = -Radius; DY <= Radius; ++DY)
		{
			for (int32 DX = -Radius; DX <= Radius; ++DX)
			{
				if (GetCell(X + DX, Y + DY, Z + DZ) != 0)
				{
					continue;
				}
				const FVector3f Centre = Origin + FVector3f(X + DX + 0.5f, Y + DY + 0.5f, Z + DZ + 0.5f) * Cell;
				const float D = (Centre - P).SizeSquared();
				if (D < Best)
				{
					Best = D;
					Out = Centre;
				}
			}
		}
	}
	return Best < 1e9f;
}

namespace
{
	constexpr float SailFar = 1e6f;

	float CapsuleDistance(const FVector3f& P, const FShipCapsule& C, FVector3f& Out)
	{
		const FVector3f D = P - ClothClosest(P, C.A, C.B);
		const float L = D.Size();
		Out = L > 1e-5f ? D / L : FVector3f::UpVector;
		return L - C.Radius;
	}

	float BlockDistance(const FVector3f& P, const FShipBlock& K, FVector3f& Out)
	{
		const FVector3f D = P - K.O;
		const FVector3f Q(FVector3f::DotProduct(D, K.X), FVector3f::DotProduct(D, K.Y), FVector3f::DotProduct(D, K.Z));
		const FVector3f E(FMath::Abs(Q.X) - K.Half.X, FMath::Abs(Q.Y) - K.Half.Y, FMath::Abs(Q.Z) - K.Half.Z);
		const FVector3f Outside(FMath::Max(E.X, 0.f) * FMath::Sign(Q.X), FMath::Max(E.Y, 0.f) * FMath::Sign(Q.Y), FMath::Max(E.Z, 0.f) * FMath::Sign(Q.Z));
		const float L = Outside.Size();
		if (L > 1e-6f)
		{
			Out = (K.X * Outside.X + K.Y * Outside.Y + K.Z * Outside.Z) / L;
			return L;
		}
		const int32 Axis = E.X > E.Y ? (E.X > E.Z ? 0 : 2) : (E.Y > E.Z ? 1 : 2);
		const FVector3f Axes[3] = { K.X, K.Y, K.Z };
		Out = Axes[Axis] * (Q[Axis] >= 0.f ? 1.f : -1.f);
		return E[Axis];
	}

	/** The hull's side as a wall from its rail down, as thick as its timbers and planking. */
	float HullDistance(const FVector3f& P, const FShipHullShape& Hull, FVector3f& Out)
	{
		if (!Hull.IsValid())
		{
			return SailFar;
		}
		const float H = P.Z - Hull.ZOffset;
		const float Rail = Hull.RailAt(P.X);
		const float Half = Hull.HalfAt(P.X, FMath::Min(H, Rail));
		if (Half <= 0.05f)
		{
			return SailFar;
		}
		const float Y = FMath::Abs(P.Y), Side = P.Y >= 0.f ? 1.f : -1.f;
		const float Inner = Half - Hull.Wall;
		const float CY = FMath::Clamp(Y, Inner, Half), CH = FMath::Min(H, Rail);
		const float DY = Y - CY, DH = H - CH;
		const float L = FMath::Sqrt(DY * DY + DH * DH);
		if (L > 1e-6f)
		{
			Out = FVector3f(0.f, Side * DY, DH) / L;
			return L;
		}
		const float ToIn = Y - Inner, ToOut = Half - Y, ToTop = Rail - H;
		if (ToTop <= ToIn && ToTop <= ToOut)
		{
			Out = FVector3f::UpVector;
			return -ToTop;
		}
		Out = FVector3f(0.f, ToOut < ToIn ? Side : -Side, 0.f);
		return -FMath::Min(ToIn, ToOut);
	}

	/** Distance between segment P0-P1 and triangle T (0 if it goes through). */
	float SegmentTriangleDistance(const FVector& P0, const FVector& P1, const FVector& A, const FVector& B, const FVector& C)
	{
		FVector Hit, HitNormal;
		if (FMath::SegmentTriangleIntersection(P0, P1, A, B, C, Hit, HitNormal))
		{
			return 0.f;
		}
		float D = float(FMath::Min(FVector::Dist(P0, FMath::ClosestPointOnTriangleToPoint(P0, A, B, C)), FVector::Dist(P1, FMath::ClosestPointOnTriangleToPoint(P1, A, B, C))));
		const FVector Corners[3] = { A, B, C };
		for (int32 E = 0; E < 3; ++E)
		{
			FVector OnSegment, OnEdge;
			FMath::SegmentDistToSegmentSafe(P0, P1, Corners[E], Corners[(E + 1) % 3], OnSegment, OnEdge);
			D = FMath::Min(D, float(FVector::Dist(OnSegment, OnEdge)));
		}
		return D;
	}
}

float ShipSails::ToHeld(const FShipSail& Sail, const FVector3f& P)
{
	float Best = SailFar;
	for (int32 J = 0; J <= Sail.NV; ++J)
	{
		for (int32 I = 0; I <= Sail.NU; ++I)
		{
			const int32 V = Sail.Index(I, J);
			if (!Sail.Pinned[V])
			{
				continue;
			}
			Best = FMath::Min(Best, (P - Sail.Rest[V]).Size());
			const int32 Next[2][2] = { { I + 1, J }, { I, J + 1 } };
			for (const auto& Nb : Next)
			{
				if (Nb[0] <= Sail.NU && Nb[1] <= Sail.NV && Sail.Pinned[Sail.Index(Nb[0], Nb[1])])
				{
					Best = FMath::Min(Best, (P - ClothClosest(P, Sail.Rest[V], Sail.Rest[Sail.Index(Nb[0], Nb[1])])).Size());
				}
			}
		}
	}
	return Best;
}

ShipSails::FFitReport ShipSails::Fit(TArray<FShipSail>& Sails, const TArray<FShipCapsule>& Colliders, const TArray<FShipBlock>& Blocks, const FShipHullShape& Hull)
{
	const double Start = FPlatformTime::Seconds();
	const int32 N = Sails.Num();
	FFitReport Report;
	float MostSwing = 0.f;

	// The movement as designed: the sail's full swing away from its held edges, fading to nothing over 2.5 m of canvas
	// towards them (the distance across the canvas to the nearest held point).
	for (FShipSail& S : Sails)
	{
		MostSwing = FMath::Max(MostSwing, S.MaxSwing);
		const int32 Count = S.Rest.Num();
		TArray<float> Dist;
		Dist.Init(SailFar, Count);
		for (int32 V = 0; V < Count; ++V)
		{
			Dist[V] = S.Pinned[V] ? 0.f : SailFar;
		}
		for (int32 Sweep = 0; Sweep < S.NU + S.NV + 2; ++Sweep)
		{
			bool bChanged = false;
			for (int32 J = 0; J <= S.NV; ++J)
			{
				for (int32 I = 0; I <= S.NU; ++I)
				{
					const int32 V = S.Index(I, J);
					for (int32 DJ = -1; DJ <= 1; ++DJ)
					{
						for (int32 DI = -1; DI <= 1; ++DI)
						{
							const int32 I1 = I + DI, J1 = J + DJ;
							if ((DI == 0 && DJ == 0) || I1 < 0 || J1 < 0 || I1 > S.NU || J1 > S.NV)
							{
								continue;
							}
							const int32 W = S.Index(I1, J1);
							const float Via = Dist[W] + (S.Rest[V] - S.Rest[W]).Size();
							if (Via < Dist[V] - 1e-4f)
							{
								Dist[V] = Via;
								bChanged = true;
							}
						}
					}
				}
			}
			if (!bChanged)
			{
				break;
			}
		}
		S.Swing.SetNum(Count);
		for (int32 V = 0; V < Count; ++V)
		{
			S.Swing[V] = S.Pinned[V] ? 0.f : S.MaxSwing * FMath::SmoothStep(0.f, 2.5f, FMath::Min(Dist[V], 1000.f));
		}
	}

	// Ropes, spars and blocks in a grid of 1 m cells: a cell lists every one that can matter to a point in it (within
	// the most a sail swings, plus the air kept). Capsules as their index, blocks as -1 - index.
	TArray<FBox3f> Boxes;
	FBox3f All(ForceInit);
	for (const FShipSail& S : Sails)
	{
		FBox3f Box(ForceInit);
		for (const FVector3f& P : S.Rest)
		{
			Box += P;
		}
		Boxes.Add(Box.ExpandBy(MostSwing * 2.f + Keep + 1.f));
		All += Boxes.Last();
	}
	constexpr float GridCell = 1.f;
	const float Reach = MostSwing + Keep + 0.25f;
	const FVector3f GridOrigin = All.Min;
	const FIntVector GridSize(FMath::CeilToInt(All.GetSize().X / GridCell) + 1, FMath::CeilToInt(All.GetSize().Y / GridCell) + 1, FMath::CeilToInt(All.GetSize().Z / GridCell) + 1);
	auto ToCell = [&](const FVector3f& P)
	{
		const FVector3f Q = (P - GridOrigin) / GridCell;
		return FIntVector(FMath::FloorToInt(Q.X), FMath::FloorToInt(Q.Y), FMath::FloorToInt(Q.Z));
	};
	TArray<uint64> Pairs;
	auto AddBox = [&](const FBox3f& Box, int32 Item)
	{
		const FIntVector Lo = ToCell(Box.Min), Hi = ToCell(Box.Max);
		for (int32 Z = FMath::Max(Lo.Z, 0); Z <= FMath::Min(Hi.Z, GridSize.Z - 1); ++Z)
		{
			for (int32 Y = FMath::Max(Lo.Y, 0); Y <= FMath::Min(Hi.Y, GridSize.Y - 1); ++Y)
			{
				for (int32 X = FMath::Max(Lo.X, 0); X <= FMath::Min(Hi.X, GridSize.X - 1); ++X)
				{
					Pairs.Add((uint64((Z * GridSize.Y + Y) * GridSize.X + X) << 32) | uint32(Item));
				}
			}
		}
	};
	for (int32 C = 0; C < Colliders.Num(); ++C)
	{
		const FShipCapsule& Capsule = Colliders[C];
		const int32 Pieces = FMath::Clamp(FMath::CeilToInt((Capsule.B - Capsule.A).Size() / GridCell), 1, 200);
		for (int32 K = 0; K < Pieces; ++K)
		{
			FBox3f Box(ForceInit);
			Box += FMath::Lerp(Capsule.A, Capsule.B, float(K) / Pieces);
			Box += FMath::Lerp(Capsule.A, Capsule.B, float(K + 1) / Pieces);
			AddBox(Box.ExpandBy(Capsule.Radius + Reach), C);
		}
	}
	for (int32 K = 0; K < Blocks.Num(); ++K)
	{
		const FShipBlock& B = Blocks[K];
		const FVector3f Extent = (B.X * B.Half.X).GetAbs() + (B.Y * B.Half.Y).GetAbs() + (B.Z * B.Half.Z).GetAbs();
		AddBox(FBox3f(B.O - Extent, B.O + Extent).ExpandBy(Reach), -1 - K);
	}
	Pairs.Sort();
	TArray<int32> GridStart, GridItems;
	GridStart.Init(0, GridSize.X * GridSize.Y * GridSize.Z + 1);
	{
		uint64 Last = ~uint64(0);
		for (const uint64 Pair : Pairs)
		{
			if (Pair == Last)
			{
				continue;
			}
			Last = Pair;
			++GridStart[int32(Pair >> 32) + 1];
			GridItems.Add(int32(uint32(Pair & 0xffffffffu)));
		}
		for (int32 I = 0; I + 1 < GridStart.Num(); ++I)
		{
			GridStart[I + 1] += GridStart[I];
		}
	}
	auto ObstaclesAt = [&](const FVector3f& P) -> TArrayView<const int32>
	{
		const FIntVector C = ToCell(P);
		if (C.X < 0 || C.Y < 0 || C.Z < 0 || C.X >= GridSize.X || C.Y >= GridSize.Y || C.Z >= GridSize.Z)
		{
			return TArrayView<const int32>();
		}
		const int32 Cell = (C.Z * GridSize.Y + C.Y) * GridSize.X + C.X;
		return TArrayView<const int32>(GridItems.GetData() + GridStart[Cell], GridStart[Cell + 1] - GridStart[Cell]);
	};
	TArray<TArray<int32>> NearSails;
	NearSails.SetNum(N);
	for (int32 S = 0; S < N; ++S)
	{
		for (int32 O = 0; O < N; ++O)
		{
			if (O != S && Boxes[O].Intersect(Boxes[S]))
			{
				NearSails[S].Add(O);
			}
		}
	}
	// The other sails' facets: centre and reach, each row's bounds (rows far off passed over), refreshed as shapes move.
	struct FFacet
	{
		FVector3f Centre;
		float Reach;
	};
	TArray<TArray<FFacet>> Facets;
	TArray<TArray<FBox3f>> Rows;
	Facets.SetNum(N);
	Rows.SetNum(N);
	auto UpdateFacets = [&]()
	{
		ParallelFor(N, [&](int32 S)
		{
			const FShipSail& Spec = Sails[S];
			Facets[S].Reset();
			Rows[S].Init(FBox3f(ForceInit), Spec.NV);
			for (int32 J = 0; J < Spec.NV; ++J)
			{
				for (int32 I = 0; I < Spec.NU; ++I)
				{
					const FVector3f C[4] = { Spec.Rest[Spec.Index(I, J)], Spec.Rest[Spec.Index(I + 1, J)], Spec.Rest[Spec.Index(I + 1, J + 1)], Spec.Rest[Spec.Index(I, J + 1)] };
					const FVector3f Centre = (C[0] + C[1] + C[2] + C[3]) * 0.25f;
					float FacetReach = 0.f;
					for (const FVector3f& P : C)
					{
						FacetReach = FMath::Max(FacetReach, (P - Centre).Size());
						Rows[S][J] += P;
					}
					Facets[S].Add({ Centre, FacetReach });
				}
			}
		});
	};
	// What is near P (a point of sail S): the nearest solid (rope, spar, block, hull) and the nearest other sail, each
	// with the way off it. Only what is within Limit counts.
	struct FNear
	{
		float Solid = SailFar;
		FVector3f SolidDir = FVector3f::UpVector;
		float Sail = SailFar;
		FVector3f SailDir = FVector3f::UpVector;
		int32 Item = 0;     // the solid: capsule index, -1 - block index, or INT_MAX for the hull
		int32 OtherSail = -1;
		float OtherSwing = 0.f; // how much the other sail is meant to move there
	};
	auto Describe = [&](const FNear& Near) -> FString
	{
		if (Near.Sail < Near.Solid)
		{
			return FString::Printf(TEXT("sail %d"), Near.OtherSail);
		}
		if (Near.Item == MAX_int32)
		{
			return TEXT("hull");
		}
		if (Near.Item >= 0)
		{
			const FShipCapsule& C = Colliders[Near.Item];
			return FString::Printf(TEXT("%s r%.2f (%.1f %.1f %.1f)-(%.1f %.1f %.1f)"), C.bSpar ? TEXT("spar") : TEXT("rope"), C.Radius, C.A.X, C.A.Y, C.A.Z, C.B.X, C.B.Y, C.B.Z);
		}
		const FShipBlock& K = Blocks[-1 - Near.Item];
		return FString::Printf(TEXT("block (%.1f %.1f %.1f) half (%.2f %.2f %.2f)"), K.O.X, K.O.Y, K.O.Z, K.Half.X, K.Half.Y, K.Half.Z);
	};
	auto Measure = [&](int32 S, const FVector3f& P, float Limit, bool bCapsules) -> FNear
	{
		FNear Out;
		FVector3f Dir;
		const bool bOnHeld = ToHeld(Sails[S], P) < Bent;
		for (const int32 Item : bOnHeld ? TArrayView<const int32>() : ObstaclesAt(P))
		{
			if (Item >= 0 && !bCapsules)
			{
				continue;
			}
			const float D = Item >= 0 ? CapsuleDistance(P, Colliders[Item], Dir) : BlockDistance(P, Blocks[-1 - Item], Dir);
			if (D < Out.Solid)
			{
				Out.Solid = D;
				Out.SolidDir = Dir;
				Out.Item = Item;
			}
		}
		{
			const float D = HullDistance(P, Hull, Dir);
			if (D < Out.Solid)
			{
				Out.Solid = D;
				Out.SolidDir = Dir;
				Out.Item = MAX_int32;
			}
		}
		for (const int32 O : NearSails[S])
		{
			const FShipSail& Other = Sails[O];
			const float Within = FMath::Min(Limit, Out.Sail);
			for (int32 J = 0; J < Other.NV; ++J)
			{
				if (Rows[O][J].ComputeSquaredDistanceToPoint(P) > Within * Within)
				{
					continue;
				}
				for (int32 I = 0; I < Other.NU; ++I)
				{
					const FFacet& Facet = Facets[O][J * Other.NU + I];
					if ((P - Facet.Centre).Size() - Facet.Reach > FMath::Min(Limit, Out.Sail))
					{
						continue;
					}
					const FVector Q0(Other.Rest[Other.Index(I, J)]), Q1(Other.Rest[Other.Index(I + 1, J)]), Q2(Other.Rest[Other.Index(I + 1, J + 1)]), Q3(Other.Rest[Other.Index(I, J + 1)]);
					for (int32 T = 0; T < 2; ++T)
					{
						const FVector3f Closest(FMath::ClosestPointOnTriangleToPoint(FVector(P), Q0, T == 0 ? Q1 : Q2, T == 0 ? Q2 : Q3));
						const FVector3f Away = P - Closest;
						const float L = Away.Size();
						if (L < Out.Sail && ToHeld(Other, Closest) >= Bent)
						{
							Out.Sail = L;
							Out.SailDir = L > 1e-5f ? Away / L : FVector3f::UpVector;
							Out.OtherSail = O;
							Out.OtherSwing = 0.25f * (Other.Swing[Other.Index(I, J)] + Other.Swing[Other.Index(I + 1, J)] + Other.Swing[Other.Index(I + 1, J + 1)] + Other.Swing[Other.Index(I, J + 1)]);
						}
					}
				}
			}
		}
		return Out;
	};
	// Exactly, the room between a facet's triangles and the ropes and spars near it (less their radius).
	auto CapsuleRoom = [&](int32 S, const FVector3f* Q, FVector3f& OutAway, int32* OutItem = nullptr) -> float
	{
		TArray<int32, TInlineAllocator<64>> Near;
		for (const FVector3f& P : { Q[0], Q[1], Q[2], Q[3], (Q[0] + Q[1] + Q[2] + Q[3]) * 0.25f })
		{
			for (const int32 Item : ObstaclesAt(P))
			{
				if (Item >= 0)
				{
					Near.AddUnique(Item);
				}
			}
		}
		float Room = SailFar;
		const FVector3f Centre = (Q[0] + Q[1] + Q[2] + Q[3]) * 0.25f;
		for (const int32 Item : Near)
		{
			const FShipCapsule& Capsule = Colliders[Item];
			if (!Capsule.bSpar && (ShipSails::ToHeld(Sails[S], Capsule.A) < Bent || ShipSails::ToHeld(Sails[S], Capsule.B) < Bent)
				&& ShipSails::ToHeld(Sails[S], ClothClosest(Centre, Capsule.A, Capsule.B)) < Bent)
			{
				continue; // made fast where the sail is bent on (its yard, lifts, braces, lacing), and met there
			}
			const FVector P0(Capsule.A), P1(Capsule.B);
			const float D = FMath::Min(SegmentTriangleDistance(P0, P1, FVector(Q[0]), FVector(Q[1]), FVector(Q[2])),
				SegmentTriangleDistance(P0, P1, FVector(Q[0]), FVector(Q[2]), FVector(Q[3]))) - Capsule.Radius;
			if (D < Room)
			{
				Room = D;
				OutAway = (Centre - ClothClosest(Centre, Capsule.A, Capsule.B)).GetSafeNormal();
				if (OutItem)
				{
					*OutItem = Item;
				}
			}
		}
		return Room;
	};
	// Each sail bellies one way: the side its facets are moved to when a rope goes right through them.
	TArray<FVector3f> Belly;
	for (const FShipSail& S : Sails)
	{
		FVector3f Mean = FVector3f::ZeroVector, Corners = FVector3f::ZeroVector;
		for (const FVector3f& P : S.Rest)
		{
			Mean += P;
		}
		Mean /= FMath::Max(S.Rest.Num(), 1);
		for (const int32 V : { S.Index(0, 0), S.Index(S.NU, 0), S.Index(S.NU, S.NV), S.Index(0, S.NV) })
		{
			Corners += S.Rest[V] * 0.25f;
		}
		Belly.Add((Mean - Corners).GetSafeNormal());
	}
	auto FacetCorners = [](const FShipSail& Spec, int32 F, int32* C)
	{
		const int32 I = F % Spec.NU, J = F / Spec.NU;
		C[0] = Spec.Index(I, J);
		C[1] = Spec.Index(I + 1, J);
		C[2] = Spec.Index(I + 1, J + 1);
		C[3] = Spec.Index(I, J + 1);
	};

	// 1. The shapes moved clear: a point or a facet nearer than Keep to anything is moved off it, and the move spread
	// over the canvas round it in a smooth bulge (no kink).
	const double PassesStart = FPlatformTime::Seconds();
	int32 Moved = 0, Passes = 0;
	constexpr float SoftStep = 0.1f;   // m a soft move takes in one pass
	constexpr float SoftLimit = 0.6f;  // m a shape may leave its cut for room to move (no more for the air kept)
	// What moved each point last (for the report): a solid's item, MAX_int32 the hull, 1e9 + index another sail.
	TArray<TArray<FVector3f>> Original;
	TArray<TArray<int32>> Cause;
	for (const FShipSail& Spec : Sails)
	{
		Original.Add(Spec.Rest);
		Cause.Emplace_GetRef().Init(MIN_int32, Spec.Rest.Num());
	}
	for (int32 Pass = 0; Pass < 20; ++Pass)
	{
		++Passes;
		UpdateFacets();
		int32 MovedNow = 0;
		for (int32 S = 0; S < N; ++S)
		{
			FShipSail& Spec = Sails[S];
			const int32 Count = Spec.Rest.Num();
			TArray<FVector3f> Delta;
			Delta.Init(FVector3f::ZeroVector, Count);
			ParallelFor(Count, [&](int32 V)
			{
				if (Spec.Pinned[V])
				{
					return;
				}
				const FNear Near = Measure(S, Spec.Rest[V], Keep + MostSwing + 0.5f, true);
				const bool bSolid = Near.Solid <= Near.Sail;
				const float D = bSolid ? Near.Solid : Near.Sail;
				// Hard: the air kept. Soft: room to move as designed, a step at a time, while the shape stays near its cut.
				const float Want = Keep + Spec.Swing[V];
				float Move = 0.f;
				if (D < Keep)
				{
					Move = Keep - D + 0.01f;
				}
				else if (D < Want - 0.01f && (Spec.Rest[V] - Original[S][V]).Size() < SoftLimit)
				{
					Move = FMath::Min(Want - D, SoftStep);
				}
				if (Move > 0.f)
				{
					Delta[V] = (bSolid ? Near.SolidDir : Near.SailDir) * Move;
					Cause[S][V] = bSolid ? Near.Item : 1000000000 + Near.OtherSail;
				}
			});
			TArray<FVector3f> FacetMove;
			FacetMove.Init(FVector3f::ZeroVector, Spec.NU * Spec.NV);
			TArray<int32> FacetCause;
			FacetCause.Init(MIN_int32, Spec.NU * Spec.NV);
			ParallelFor(Spec.NU * Spec.NV, [&](int32 F)
			{
				int32 C[4];
				FacetCorners(Spec, F, C);
				if (Spec.Pinned[C[0]] || Spec.Pinned[C[1]] || Spec.Pinned[C[2]] || Spec.Pinned[C[3]])
				{
					return; // on its spar or stay: what is made fast there touches it by design
				}
				const FVector3f Q[4] = { Spec.Rest[C[0]], Spec.Rest[C[1]], Spec.Rest[C[2]], Spec.Rest[C[3]] };
				const float Want = Keep + FMath::Max(FMath::Max(Spec.Swing[C[0]], Spec.Swing[C[1]]), FMath::Max(Spec.Swing[C[2]], Spec.Swing[C[3]]));
				FVector3f Normal = FVector3f::CrossProduct(Q[2] - Q[0], Q[3] - Q[1]).GetSafeNormal();
				if (FVector3f::DotProduct(Normal, Belly[S]) < 0.f)
				{
					Normal = -Normal;
				}
				float Worst = Want;
				FVector3f Away = Normal;
				FVector3f RopeAway;
				int32 RopeItem = MIN_int32, WorstItem = MIN_int32;
				const float Rope = CapsuleRoom(S, Q, RopeAway, &RopeItem);
				if (Rope <= 0.f)
				{
					Worst = Rope;
					Away = Normal; // right through: to the side it bellies to
					WorstItem = RopeItem;
				}
				else if (Rope < Worst)
				{
					Worst = Rope;
					Away = RopeAway;
					WorstItem = RopeItem;
				}
				for (int32 U = 0; U < 3; ++U)
				{
					for (int32 W = 0; W < 3; ++W)
					{
						const FVector3f P = FMath::Lerp(FMath::Lerp(Q[0], Q[1], (U + 0.5f) / 3.f), FMath::Lerp(Q[3], Q[2], (U + 0.5f) / 3.f), (W + 0.5f) / 3.f);
						const FNear Near = Measure(S, P, Keep + 0.5f, false);
						if (Near.Solid < Worst)
						{
							Worst = Near.Solid;
							Away = Near.SolidDir;
							WorstItem = Near.Item;
						}
						if (Near.Sail < Worst)
						{
							Worst = Near.Sail;
							Away = Near.SailDir;
							WorstItem = 1000000000 + Near.OtherSail;
						}
					}
				}
				const bool bFar = (Q[0] - Original[S][C[0]]).Size() >= SoftLimit || (Q[2] - Original[S][C[2]]).Size() >= SoftLimit;
				const float Move = Worst < Keep ? Keep - Worst + 0.01f : (Worst < Want - 0.01f && !bFar ? FMath::Min(Want - Worst, SoftStep) : 0.f);
				if (Move > 0.f)
				{
					// Through by a rope: to the side the sail bellies to. Otherwise straight away from what it nears.
					FacetMove[F] = Worst <= 0.f && WorstItem == RopeItem ? Normal * Move : Away.GetSafeNormal() * Move;
					FacetCause[F] = WorstItem;
				}
			});
			for (int32 F = 0; F < FacetMove.Num(); ++F)
			{
				if (FacetMove[F].IsZero())
				{
					continue;
				}
				int32 C[4];
				FacetCorners(Spec, F, C);
				for (const int32 V : C)
				{
					if (FacetMove[F].SizeSquared() > Delta[V].SizeSquared())
					{
						Delta[V] = FacetMove[F];
						Cause[S][V] = FacetCause[F];
					}
				}
			}
			// Smoothed like a membrane: each point keeps the move it needs, or takes 0.85 of its neighbours' mean move
			// (the held edges stay put): the canvas bulges smoothly round what it clears, no steps, no kinks.
			{
				const TArray<FVector3f> Needed = Delta;
				for (int32 Iteration = 0; Iteration < 20; ++Iteration)
				{
					TArray<FVector3f> Next = Delta;
					for (int32 J = 0; J <= Spec.NV; ++J)
					{
						for (int32 I = 0; I <= Spec.NU; ++I)
						{
							const int32 V = Spec.Index(I, J);
							if (Spec.Pinned[V])
							{
								continue;
							}
							FVector3f Sum = FVector3f::ZeroVector;
							int32 Around = 0;
							const int32 Nb[4][2] = { { I - 1, J }, { I + 1, J }, { I, J - 1 }, { I, J + 1 } };
							for (const auto& A : Nb)
							{
								if (A[0] < 0 || A[1] < 0 || A[0] > Spec.NU || A[1] > Spec.NV)
								{
									continue;
								}
								const int32 W = Spec.Index(A[0], A[1]);
								Sum += Spec.Pinned[W] ? FVector3f::ZeroVector : Delta[W];
								++Around;
							}
							const FVector3f Mean = Around > 0 ? Sum * (0.92f / Around) : FVector3f::ZeroVector;
							Next[V] = !Needed[V].IsZero() && Needed[V].SizeSquared() >= Mean.SizeSquared() ? Needed[V] : Mean;
						}
					}
					Delta = MoveTemp(Next);
				}
			}
			for (int32 V = 0; V < Count; ++V)
			{
				if (!Spec.Pinned[V] && !Delta[V].IsZero())
				{
					Spec.Rest[V] += Delta[V];
					++MovedNow;
				}
			}
		}
		Moved += MovedNow;
		if (MovedNow == 0)
		{
			break;
		}
	}

	// 2. Each point's room, and its movement bounded by it: the least of its own and of the facets round it (exactly
	// for ropes and spars; 3 x 3 samples for the rest, give or take a quarter of their spacing), less Keep; another
	// sail's gap is shared (each moves at most half of it). Then smoothed: no point moves much more than its neighbour.
	const double RoomsStart = FPlatformTime::Seconds();
	UpdateFacets();
	int32 Points = 0;
	double Meant = 0.0, Cut = 0.0;
	FString PerSail;
	FCriticalSection ReportLock;
	TArray<FString> Examples;
	int32 CutKinds[7] = {};
	TMap<int32, int32> CutItems;
	for (int32 S = 0; S < N; ++S)
	{
		FShipSail& Spec = Sails[S];
		const int32 Count = Spec.Rest.Num();
		TArray<float> FacetRoom;
		FacetRoom.Init(SailFar, Spec.NU * Spec.NV);
		// What limits each facet's and point's room (for the report): kind and item.
		TArray<int32> FacetKind, FacetItem, PointKind, PointItem;
		FacetKind.Init(0, Spec.NU * Spec.NV);
		FacetItem.Init(MIN_int32, Spec.NU * Spec.NV);
		PointKind.Init(0, Count);
		PointItem.Init(MIN_int32, Count);
		TArray<int32> FacetFails;
		FacetFails.Init(0, Spec.NU * Spec.NV);
		ParallelFor(Spec.NU * Spec.NV, [&](int32 F)
		{
			int32 C[4];
			FacetCorners(Spec, F, C);
			if (Spec.Pinned[C[0]] || Spec.Pinned[C[1]] || Spec.Pinned[C[2]] || Spec.Pinned[C[3]])
			{
				return;
			}
			const FVector3f Q[4] = { Spec.Rest[C[0]], Spec.Rest[C[1]], Spec.Rest[C[2]], Spec.Rest[C[3]] };
			const float Spacing = FMath::Max(FMath::Max((Q[1] - Q[0]).Size(), (Q[2] - Q[3]).Size()), FMath::Max((Q[3] - Q[0]).Size(), (Q[2] - Q[1]).Size())) / 3.f;
			FVector3f Away;
			int32 RopeItem = MIN_int32;
			float Room = CapsuleRoom(S, Q, Away, &RopeItem) - Keep;
			FacetKind[F] = 3;
			FacetItem[F] = RopeItem;
			if (Room < -0.005f)
			{
				++FacetFails[F];
				FScopeLock Guard(&ReportLock);
				if (Examples.Num() < 6)
				{
					FNear Fake;
					Fake.Item = RopeItem;
					Examples.Add(FString::Printf(TEXT("sail %d facet %d,%d by %s (%.2f)"), S, F % Spec.NU, F / Spec.NU, *Describe(Fake), Room + Keep));
				}
			}
			for (int32 U = 0; U < 3; ++U)
			{
				for (int32 W = 0; W < 3; ++W)
				{
					const FVector3f P = FMath::Lerp(FMath::Lerp(Q[0], Q[1], (U + 0.5f) / 3.f), FMath::Lerp(Q[3], Q[2], (U + 0.5f) / 3.f), (W + 0.5f) / 3.f);
					const FNear Near = Measure(S, P, MostSwing * 2.f + Keep + 0.5f, false);
					if (Near.Solid < Keep - 0.005f || Near.Sail < Keep - 0.005f)
					{
						++FacetFails[F];
						FScopeLock Guard(&ReportLock);
						if (Examples.Num() < 6)
						{
							Examples.Add(FString::Printf(TEXT("sail %d facet %d,%d near %s (%.2f)"), S, F % Spec.NU, F / Spec.NU, *Describe(Near), FMath::Min(Near.Solid, Near.Sail)));
						}
					}
					if (Near.Solid - Keep - Spacing * 0.25f < Room)
					{
						Room = Near.Solid - Keep - Spacing * 0.25f;
						FacetKind[F] = 4;
						FacetItem[F] = Near.Item;
					}
					if (Near.Sail - Keep - Near.OtherSwing - Spacing * 0.25f < Room)
					{
						Room = Near.Sail - Keep - Near.OtherSwing - Spacing * 0.25f;
						FacetKind[F] = 5;
						FacetItem[F] = 1000000000 + Near.OtherSail;
					}
				}
			}
			FacetRoom[F] = Room;
		});
		Spec.Amplitude.Init(0.f, Count);
		TArray<int32> PointFails;
		PointFails.Init(0, Count);
		ParallelFor(Count, [&](int32 V)
		{
			if (Spec.Pinned[V])
			{
				return;
			}
			const FNear Near = Measure(S, Spec.Rest[V], MostSwing * 2.f + Keep + 0.5f, true);
			PointFails[V] = (Near.Solid < Keep - 0.005f || Near.Sail < Keep - 0.005f) ? 1 : 0;
			if (PointFails[V])
			{
				FScopeLock Guard(&ReportLock);
				if (Examples.Num() < 6)
				{
					Examples.Add(FString::Printf(TEXT("sail %d point %d,%d near %s (%.2f)"), S, V % (Spec.NU + 1), V / (Spec.NU + 1), *Describe(Near), FMath::Min(Near.Solid, Near.Sail)));
				}
			}
			float Room = Near.Solid - Keep;
			PointKind[V] = 1;
			PointItem[V] = Near.Item;
			if (Near.Sail - Keep - Near.OtherSwing < Room)
			{
				Room = Near.Sail - Keep - Near.OtherSwing;
				PointKind[V] = 2;
				PointItem[V] = 1000000000 + Near.OtherSail;
			}
			const int32 I = V % (Spec.NU + 1), J = V / (Spec.NU + 1);
			for (int32 DJ = -1; DJ <= 0; ++DJ)
			{
				for (int32 DI = -1; DI <= 0; ++DI)
				{
					const int32 FI = I + DI, FJ = J + DJ;
					if (FI >= 0 && FJ >= 0 && FI < Spec.NU && FJ < Spec.NV && FacetRoom[FJ * Spec.NU + FI] < Room)
					{
						Room = FacetRoom[FJ * Spec.NU + FI];
						PointKind[V] = FacetKind[FJ * Spec.NU + FI];
						PointItem[V] = FacetItem[FJ * Spec.NU + FI];
					}
				}
			}
			Spec.Amplitude[V] = FMath::Clamp(Room, 0.f, Spec.Swing[V]);
		});
		const TArray<float> Raw = Spec.Amplitude;
		for (int32 Iteration = 0; Iteration < Spec.NU + Spec.NV; ++Iteration)
		{
			bool bChanged = false;
			for (int32 J = 0; J <= Spec.NV; ++J)
			{
				for (int32 I = 0; I <= Spec.NU; ++I)
				{
					const int32 V = Spec.Index(I, J);
					const int32 Around[4][2] = { { I - 1, J }, { I + 1, J }, { I, J - 1 }, { I, J + 1 } };
					for (const auto& A : Around)
					{
						if (A[0] < 0 || A[1] < 0 || A[0] > Spec.NU || A[1] > Spec.NV)
						{
							continue;
						}
						const int32 W = Spec.Index(A[0], A[1]);
						const float Limit = Spec.Amplitude[W] + 0.18f * (Spec.Rest[V] - Spec.Rest[W]).Size();
						if (Spec.Amplitude[V] > Limit + 1e-4f)
						{
							Spec.Amplitude[V] = Limit;
							bChanged = true;
						}
					}
				}
			}
			if (!bChanged)
			{
				break;
			}
		}
		int32 Fails = 0;
		for (const int32 F : FacetFails)
		{
			Fails += F;
		}
		for (const int32 F : PointFails)
		{
			Fails += F;
		}
		Report.ShapeFails += Fails;
		if (Fails > 0)
		{
			Report.FailMasts |= 1u << FMath::Clamp(Spec.Mast, 0, 31);
		}
		double SailMeant = 0.0, SailCut = 0.0;
		for (int32 V = 0; V < Count; ++V)
		{
			if (Spec.Swing[V] > 0.05f)
			{
				++Points;
				SailMeant += 1.0;
				if (Spec.Amplitude[V] < 0.5f * Spec.Swing[V])
				{
					SailCut += 1.0;
					const int32 Kind = Raw[V] >= 0.5f * Spec.Swing[V] ? 6 : PointKind[V];
					++CutKinds[Kind];
					if (Kind != 6)
					{
						CutItems.FindOrAdd(PointItem[V]) += 1;
					}
				}
			}
		}
		Meant += SailMeant;
		Cut += SailCut;
		PerSail += FString::Printf(TEXT(" %d/%.0f%%"), Fails, SailMeant > 0.0 ? 100.0 * SailCut / SailMeant : 0.0);
	}
	Report.Reduced = Meant > 0.0 ? float(Cut / Meant) : 0.f;

	// 3. The wind out of the sails: each point's place with the belly gone (straight between head and foot, the foot
	// straight between its clews, in the sail's own plane), and the share of the way there it may go while it still
	// clears everything by Keep plus its movement (another sail's movement too).
	int32 Slackened = 0, SlackPoints = 0;
	for (int32 S = 0; S < N; ++S)
	{
		FShipSail& Spec = Sails[S];
		const int32 Count = Spec.Rest.Num();
		const FVector3f B = Belly[S];
		Spec.BellyDir = B;
		Spec.Flat = Spec.Rest;
		for (int32 I = 0; I <= Spec.NU; ++I)
		{
			const FVector3f Head = Spec.Rest[Spec.Index(I, 0)];
			const FVector3f Foot = Spec.Rest[Spec.Index(I, Spec.NV)];
			for (int32 J = 0; J <= Spec.NV; ++J)
			{
				const int32 V = Spec.Index(I, J);
				if (Spec.Pinned[V])
				{
					continue;
				}
				// The foot stays where it was fitted (it clears the tops and rails as it is).
				const FVector3f Chord = FMath::Lerp(Head, Foot, float(J) / Spec.NV);
				Spec.Flat[V] = Spec.Rest[V] - B * FVector3f::DotProduct(Spec.Rest[V] - Chord, B);
			}
		}
		Spec.Slack.Init(0.f, Count);
		ParallelFor(Count, [&](int32 V)
		{
			if (Spec.Pinned[V])
			{
				return;
			}
			const FVector3f Way = Spec.Flat[V] - Spec.Rest[V];
			if (Way.Size() < 0.02f)
			{
				Spec.Slack[V] = 1.f;
				return;
			}
			float Allowed = 0.f;
			for (const float K : { 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.f })
			{
				const FNear Near = Measure(S, Spec.Rest[V] + Way * K, MostSwing * 2.f + Keep + 0.5f, true);
				const float Need = Keep + Spec.Amplitude[V];
				if (Near.Solid < Need || Near.Sail < Need + Near.OtherSwing + 0.1f)
				{
					break;
				}
				Allowed = K;
			}
			Spec.Slack[V] = Allowed;
		});
		// Facets: their way to the empty shape checked too (a rope between two points, a block between samples).
		{
			TArray<float> FacetSlack;
			FacetSlack.Init(1.f, Spec.NU * Spec.NV);
			ParallelFor(Spec.NU * Spec.NV, [&](int32 F)
			{
				int32 C[4];
				FacetCorners(Spec, F, C);
				float NeedA = 0.f;
				for (const int32 V : C)
				{
					NeedA = FMath::Max(NeedA, Spec.Amplitude[V]);
				}
				float Allowed = 0.f;
				for (const float K : { 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.f })
				{
					FVector3f Q[4];
					for (int32 Corner = 0; Corner < 4; ++Corner)
					{
						Q[Corner] = FMath::Lerp(Spec.Rest[C[Corner]], Spec.Flat[C[Corner]], K);
					}
					FVector3f Away;
					if (CapsuleRoom(S, Q, Away) < Keep + NeedA)
					{
						break;
					}
					const float Spacing = FMath::Max(FMath::Max((Q[1] - Q[0]).Size(), (Q[2] - Q[3]).Size()), FMath::Max((Q[3] - Q[0]).Size(), (Q[2] - Q[1]).Size())) / 3.f;
					bool bClear = true;
					for (int32 U = 0; U < 3 && bClear; ++U)
					{
						for (int32 W = 0; W < 3 && bClear; ++W)
						{
							const FVector3f P = FMath::Lerp(FMath::Lerp(Q[0], Q[1], (U + 0.5f) / 3.f), FMath::Lerp(Q[3], Q[2], (U + 0.5f) / 3.f), (W + 0.5f) / 3.f);
							const FNear Near = Measure(S, P, MostSwing * 2.f + Keep + 0.5f, false);
							const float Need = Keep + NeedA + Spacing * 0.25f;
							bClear = Near.Solid >= Need && Near.Sail >= Need + Near.OtherSwing + 0.1f;
						}
					}
					if (!bClear)
					{
						break;
					}
					Allowed = K;
				}
				FacetSlack[F] = Allowed;
			});
			for (int32 F = 0; F < FacetSlack.Num(); ++F)
			{
				int32 C[4];
				FacetCorners(Spec, F, C);
				for (const int32 V : C)
				{
					if (!Spec.Pinned[V])
					{
						Spec.Slack[V] = FMath::Min(Spec.Slack[V], FacetSlack[F]);
					}
				}
			}
		}
		// Smooth: no point empties much more than its neighbour (a crease would show).
		for (int32 Iteration = 0; Iteration < Spec.NU + Spec.NV; ++Iteration)
		{
			bool bChanged = false;
			for (int32 J = 0; J <= Spec.NV; ++J)
			{
				for (int32 I = 0; I <= Spec.NU; ++I)
				{
					const int32 V = Spec.Index(I, J);
					const int32 Nb[4][2] = { { I - 1, J }, { I + 1, J }, { I, J - 1 }, { I, J + 1 } };
					for (const auto& A : Nb)
					{
						if (A[0] < 0 || A[1] < 0 || A[0] > Spec.NU || A[1] > Spec.NV)
						{
							continue;
						}
						const int32 W = Spec.Index(A[0], A[1]);
						const float Limit = Spec.Slack[W] + 0.25f;
						if (!Spec.Pinned[W] && Spec.Slack[V] > Limit + 1e-4f)
						{
							Spec.Slack[V] = Limit;
							bChanged = true;
						}
					}
				}
			}
			if (!bChanged)
			{
				break;
			}
		}
		for (int32 V = 0; V < Count; ++V)
		{
			if (!Spec.Pinned[V] && (Spec.Flat[V] - Spec.Rest[V]).Size() >= 0.02f)
			{
				++SlackPoints;
				Slackened += Spec.Slack[V] >= 0.99f ? 1 : 0;
			}
		}
	}
	TArray<FString> Moves;
	for (int32 S = 0; S < N; ++S)
	{
		int32 Most = INDEX_NONE;
		float MostSize = 0.3f;
		for (int32 V = 0; V < Sails[S].Rest.Num(); ++V)
		{
			const float Size = (Sails[S].Rest[V] - Original[S][V]).Size();
			if (Size > MostSize)
			{
				MostSize = Size;
				Most = V;
			}
		}
		if (Most != INDEX_NONE)
		{
			const int32 Why = Cause[S][Most];
			FString What = TEXT("?");
			if (Why >= 1000000000)
			{
				What = FString::Printf(TEXT("sail %d"), Why - 1000000000);
			}
			else if (Why != MIN_int32)
			{
				FNear Fake;
				Fake.Item = Why;
				What = Describe(Fake);
			}
			const FVector3f From = Original[S][Most], To = Sails[S].Rest[Most];
			Moves.Add(FString::Printf(TEXT("sail %d point %d,%d moved %.2f m (%.1f %.1f %.1f)->(%.1f %.1f %.1f), last by %s"), S, Most % (Sails[S].NU + 1), Most / (Sails[S].NU + 1), MostSize, From.X, From.Y, From.Z, To.X, To.Y, To.Z, *What));
		}
	}
	Report.Text = FString::Printf(TEXT("%d of %d bellied points may empty fully; "), Slackened, SlackPoints) + FString::Printf(TEXT("sails fitted in %.0f ms (setup %.0f, passes %.0f, rooms %.0f; %d passes, %d moves): shape fails %d, movement cut by half on %.1f%% of the canvas (by sail, fails/cut:%s)"),
		(FPlatformTime::Seconds() - Start) * 1000.0, (PassesStart - Start) * 1000.0, (RoomsStart - PassesStart) * 1000.0, (FPlatformTime::Seconds() - RoomsStart) * 1000.0, Passes, Moved, Report.ShapeFails, Report.Reduced * 100.f, *PerSail);
	{
		Report.Text += FString::Printf(TEXT("; cut by: point near solid %d, point near sail %d, rope through facet %d, facet near solid %d, facet near sail %d, smoothing %d"),
			CutKinds[1], CutKinds[2], CutKinds[3], CutKinds[4], CutKinds[5], CutKinds[6]);
		CutItems.ValueSort([](int32 A, int32 B) { return A > B; });
		int32 Shown = 0;
		for (const TPair<int32, int32>& Item : CutItems)
		{
			if (Shown++ >= 6)
			{
				break;
			}
			FString What = TEXT("?");
			if (Item.Key >= 1000000000)
			{
				What = FString::Printf(TEXT("sail %d"), Item.Key - 1000000000);
			}
			else if (Item.Key != MIN_int32)
			{
				FNear Fake;
				Fake.Item = Item.Key;
				What = Describe(Fake);
			}
			Report.Text += FString::Printf(TEXT("%s %s x%d"), Shown == 1 ? TEXT("; most:") : TEXT(","), *What, Item.Value);
		}
	}
	if (Moves.Num() > 0)
	{
		Report.Text += TEXT("; moves: ") + FString::Join(Moves, TEXT("; "));
	}
	if (Examples.Num() > 0)
	{
		Report.Text += TEXT("; fails: ") + FString::Join(Examples, TEXT("; "));
		TArray<FString> Who;
		for (int32 S = 0; S < N; ++S)
		{
			FBox3f Box(ForceInit);
			for (const FVector3f& P : Sails[S].Rest)
			{
				Box += P;
			}
			Who.Add(FString::Printf(TEXT("%d: %dx%d (%.1f %.1f %.1f)-(%.1f %.1f %.1f)"), S, Sails[S].NU, Sails[S].NV, Box.Min.X, Box.Min.Y, Box.Min.Z, Box.Max.X, Box.Max.Y, Box.Max.Z));
		}
		Report.Text += TEXT("; sails: ") + FString::Join(Who, TEXT("; "));
	}
	return Report;
}

void FSailAnimation::Init(TArray<FShipSail> InSails, TSharedPtr<const FShipVoxels> InVerify)
{
	Sails.Reset();
	Verify = InVerify;
	int32 Index = 0;
	for (FShipSail& Spec : InSails)
	{
		FSailState& S = Sails.AddDefaulted_GetRef();
		const int32 Count = Spec.Rest.Num();
		if (Spec.Amplitude.Num() != Count)
		{
			Spec.Amplitude.Init(0.f, Count);
		}
		S.X = Spec.Rest;
		S.Normal.Init(FVector3f::UpVector, Count);
		UpdateNormals(Spec, Spec.Rest, S.RestNormal);
		S.Normal = S.RestNormal;
		// Where each point is in the waves: metres along its row from the luff, down its column from the head.
		S.Along.Init(0.f, Count);
		S.Down.Init(0.f, Count);
		for (int32 J = 0; J <= Spec.NV; ++J)
		{
			for (int32 I = 1; I <= Spec.NU; ++I)
			{
				S.Along[Spec.Index(I, J)] = S.Along[Spec.Index(I - 1, J)] + (Spec.Rest[Spec.Index(I, J)] - Spec.Rest[Spec.Index(I - 1, J)]).Size();
			}
		}
		for (int32 I = 0; I <= Spec.NU; ++I)
		{
			for (int32 J = 1; J <= Spec.NV; ++J)
			{
				S.Down[Spec.Index(I, J)] = S.Down[Spec.Index(I, J - 1)] + (Spec.Rest[Spec.Index(I, J)] - Spec.Rest[Spec.Index(I, J - 1)]).Size();
			}
		}
		S.Phase = 1.7f * Index++;
		S.Spec = MoveTemp(Spec);
	}
}

void FSailAnimation::UpdateNormals(const FShipSail& Spec, const TArray<FVector3f>& X, TArray<FVector3f>& OutNormal) const
{
	OutNormal.SetNum(X.Num());
	for (int32 J = 0; J <= Spec.NV; ++J)
	{
		for (int32 I = 0; I <= Spec.NU; ++I)
		{
			const FVector3f DU = X[Spec.Index(FMath::Min(I + 1, Spec.NU), J)] - X[Spec.Index(FMath::Max(I - 1, 0), J)];
			// Triangular sails meet in a point at their head: take the next row's direction there.
			const FVector3f DV = X[Spec.Index(I, FMath::Min(J + 1, Spec.NV))] - X[Spec.Index(I, FMath::Max(J - 1, 0))];
			const FVector3f N = FVector3f::CrossProduct(DU, DV).GetSafeNormal();
			OutNormal[Spec.Index(I, J)] = N;
		}
	}
	// Degenerate points (a head where a row shrinks to a point) take the normal of the point below.
	for (int32 J = Spec.NV; J >= 0; --J)
	{
		for (int32 I = 0; I <= Spec.NU; ++I)
		{
			FVector3f& N = OutNormal[Spec.Index(I, J)];
			if (N.IsNearlyZero() && J < Spec.NV)
			{
				N = OutNormal[Spec.Index(I, J + 1)];
			}
		}
	}
}

void FSailAnimation::Evaluate(float Time, float Strength, const FVector3f& WindDir, float ForcedFill)
{
	Strength = FMath::Clamp(Strength, 0.f, 1.f);
	const FVector3f Wind = WindDir.GetSafeNormal();
	ParallelFor(Sails.Num(), [&](int32 Index)
	{
		FSailState& S = Sails[Index];
		const FShipSail& Spec = S.Spec;
		// How the wind takes it: on its back, it fills; along it, it empties; on its face, it lies flat. The sail
		// follows over about a second.
		float Target = ForcedFill;
		if (Target < 0.f)
		{
			Target = FMath::SmoothStep(-0.05f, 0.35f, FVector3f::DotProduct(Wind, Spec.BellyDir)) * FMath::SmoothStep(0.05f, 0.3f, Strength);
		}
		if (ForcedFill >= 0.f || S.LastTime < 0.f || Time < S.LastTime)
		{
			S.Fill = Target;
		}
		else
		{
			S.Fill = FMath::FInterpTo(S.Fill, Target, FMath::Min(Time - S.LastTime, 0.1f), 1.5f);
		}
		S.LastTime = Time;
		const float Empty = 1.f - S.Fill;
		// A full sail is taut: it ripples; an empty one flogs (its whole movement).
		const float Flog = 0.45f + 0.55f * Empty;
		// Waves weighed 0.5 + 0.3 + 0.2: the sum never exceeds 1, so no point ever goes further than its amplitude.
		constexpr float W1 = UE_TWO_PI * 0.55f, K1 = UE_TWO_PI / 5.f;   // the long flutter running aft through the canvas
		constexpr float W2 = UE_TWO_PI * 1.3f, K2 = UE_TWO_PI / 2.2f;   // the short ripple on top of it
		constexpr float W3 = UE_TWO_PI * 0.12f;                         // the whole sail breathing with the gusts
		const float Breath = FMath::Sin(W3 * Time + S.Phase);
		for (int32 V = 0; V < S.X.Num(); ++V)
		{
			const FVector3f Shape = Spec.Slack.Num() == S.X.Num() ? FMath::Lerp(Spec.Rest[V], Spec.Flat[V], Empty * Spec.Slack[V]) : Spec.Rest[V];
			const float A = Spec.Amplitude[V] * FMath::Max(Strength, 0.35f) * Flog;
			if (A <= 0.f)
			{
				S.X[V] = Shape;
				continue;
			}
			const float F = 0.5f * FMath::Sin(W1 * Time - K1 * S.Along[V] + 0.35f * K1 * S.Down[V] + S.Phase)
				+ 0.3f * FMath::Sin(W2 * Time - K2 * S.Along[V] - 0.6f * K2 * S.Down[V] + 1.3f + S.Phase)
				+ 0.2f * Breath;
			S.X[V] = Shape + S.RestNormal[V] * (A * F);
		}
		UpdateNormals(Spec, S.X, S.Normal);
	});
}

FString FSailAnimation::VerifyCycle(int32 Times, float Seconds, int32& OutInside, int32& OutCrossings, FVector3f* OutFirst)
{
	OutInside = 0;
	OutCrossings = 0;
	int32 Samples = 0;
	FString First;
	double EvaluateSeconds = 0.0;
	for (int32 K = 0; K < Times; ++K)
	{
		const double T0 = FPlatformTime::Seconds();
		Evaluate(Seconds * K / Times, 1.f, FVector3f::ForwardVector, K % 3 == 0 ? 1.f : (K % 3 == 1 ? 0.5f : 0.f));
		EvaluateSeconds += FPlatformTime::Seconds() - T0;
		int32 Count = 0;
		FString Detail;
		OutInside += CountInDrawn(Count, First.IsEmpty() ? &Detail : nullptr, First.IsEmpty() ? OutFirst : nullptr);
		Samples += Count;
		if (First.IsEmpty() && !Detail.IsEmpty())
		{
			First = FString::Printf(TEXT(" first at %.2f s:%s"), Seconds * K / Times, *Detail);
		}
		if (K % 4 == 0)
		{
			OutCrossings += CountSailCrossings();
		}
	}
	Evaluate(0.f, 1.f, FVector3f::ForwardVector, 1.f);
	return FString::Printf(TEXT("%d of %d canvas samples inside anything drawn over %d instants (full, half and empty), %d canvas edges through another sail, %.2f ms a frame to move them%s"),
		OutInside, Samples, Times, OutCrossings, EvaluateSeconds * 1000.0 / FMath::Max(Times, 1), *First);
}

int32 FSailAnimation::CountInDrawn(int32& OutSamples, FString* OutDetail, FVector3f* OutFirst) const
{
	OutSamples = 0;
	int32 Inside = 0;
	if (!CanVerify())
	{
		return 0;
	}
	for (const FSailState& S : Sails)
	{
		const FShipSail& Spec = S.Spec;
		for (int32 J = 0; J < Spec.NV; ++J)
		{
			for (int32 I = 0; I < Spec.NU; ++I)
			{
				const int32 C[4] = { Spec.Index(I, J), Spec.Index(I + 1, J), Spec.Index(I + 1, J + 1), Spec.Index(I, J + 1) };
				// Facets on a spar or stay (held along it) touch it by design.
				if (Spec.Pinned[C[0]] || Spec.Pinned[C[1]] || Spec.Pinned[C[2]] || Spec.Pinned[C[3]])
				{
					continue;
				}
				for (int32 U = 0; U < 4; ++U)
				{
					for (int32 V = 0; V < 4; ++V)
					{
						const float FU = (U + 0.5f) / 4.f, FV = (V + 0.5f) / 4.f;
						++OutSamples;
						const FVector3f P = FMath::Lerp(FMath::Lerp(S.X[C[0]], S.X[C[1]], FU), FMath::Lerp(S.X[C[3]], S.X[C[2]], FU), FV);
						const uint8 Cell = Verify->Get(P);
						if ((Cell & 1) && ShipSails::ToHeld(Spec, P) >= ShipSails::Bent)
						{
							++Inside;
							if (OutFirst && Inside == 1)
							{
								*OutFirst = P;
							}
							if (OutDetail && Inside <= 8)
							{
								static const TCHAR* const Kinds[5] = { TEXT("wood"), TEXT("canvas"), TEXT("rope"), TEXT("iron"), TEXT("glass") };
								FString What;
								for (int32 K = 0; K < 5; ++K)
								{
									if (Cell & (4 << K))
									{
										What += FString(What.IsEmpty() ? TEXT("") : TEXT("+")) + Kinds[K];
									}
								}
								*OutDetail += FString::Printf(TEXT(" [sail %d facet %d,%d at %.1f %.1f %.1f: %s]"), int32(&S - Sails.GetData()), I, J, P.X, P.Y, P.Z, *What);
							}
						}
					}
				}
			}
		}
	}
	return Inside;
}

int32 FSailAnimation::CountSailCrossings() const
{
	TArray<FBox3f> Boxes;
	for (const FSailState& S : Sails)
	{
		FBox3f Box(ForceInit);
		for (const FVector3f& P : S.X)
		{
			Box += P;
		}
		Boxes.Add(Box);
	}
	int32 Crossings = 0;
	for (int32 A = 0; A < Sails.Num(); ++A)
	{
		for (int32 B = 0; B < Sails.Num(); ++B)
		{
			if (A == B || !Boxes[A].Intersect(Boxes[B]))
			{
				continue;
			}
			const FSailState& SA = Sails[A];
			const FSailState& SB = Sails[B];
			const FShipSail& EA = SA.Spec;
			const FShipSail& EB = SB.Spec;
			for (int32 J = 0; J <= EA.NV; ++J)
			{
				for (int32 I = 0; I <= EA.NU; ++I)
				{
					for (int32 Dir = 0; Dir < 2; ++Dir)
					{
						const int32 I1 = I + (Dir == 0 ? 1 : 0), J1 = J + (Dir == 1 ? 1 : 0);
						if (I1 > EA.NU || J1 > EA.NV)
						{
							continue;
						}
						const FVector P0(SA.X[EA.Index(I, J)]), P1(SA.X[EA.Index(I1, J1)]);
						FBox3f Edge(ForceInit);
						Edge += FVector3f(P0);
						Edge += FVector3f(P1);
						if (!Edge.Intersect(Boxes[B]))
						{
							continue;
						}
						for (int32 L = 0; L < EB.NV; ++L)
						{
							for (int32 K = 0; K < EB.NU; ++K)
							{
								const FVector Q0(SB.X[EB.Index(K, L)]), Q1(SB.X[EB.Index(K + 1, L)]), Q2(SB.X[EB.Index(K + 1, L + 1)]), Q3(SB.X[EB.Index(K, L + 1)]);
								FVector Hit, Normal;
								if (FMath::SegmentTriangleIntersection(P0, P1, Q0, Q1, Q2, Hit, Normal) || FMath::SegmentTriangleIntersection(P0, P1, Q0, Q2, Q3, Hit, Normal))
								{
									++Crossings;
								}
							}
						}
					}
				}
			}
		}
	}
	return Crossings;
}

void FSailAnimation::GetTopology(TArray<int32>& OutTriangles, TArray<FVector2D>& OutUV, TArray<FColor>& OutColors) const
{
	OutTriangles.Reset();
	OutUV.Reset();
	OutColors.Reset();
	int32 Offset = 0;
	for (const FSailState& S : Sails)
	{
		const FShipSail& Spec = S.Spec;
		for (int32 V = 0; V < Spec.Rest.Num(); ++V)
		{
			OutUV.Add(FVector2D(Spec.UV[V]));
			const FLinearColor& C = Spec.Color[V];
			auto Byte = [](float X) { return uint8(FMath::Clamp(FMath::RoundToInt(X * 255.f), 0, 255)); };
			OutColors.Add(FColor(Byte(C.R), Byte(C.G), Byte(C.B), 255));
		}
		for (int32 J = 0; J < Spec.NV; ++J)
		{
			for (int32 I = 0; I < Spec.NU; ++I)
			{
				const int32 A = Offset + Spec.Index(I, J), B = Offset + Spec.Index(I + 1, J);
				const int32 C = Offset + Spec.Index(I + 1, J + 1), D = Offset + Spec.Index(I, J + 1);
				OutTriangles.Append({ A, C, B, A, D, C });
			}
		}
		Offset += Spec.Rest.Num();
	}
}

void FSailAnimation::GetSurface(TArray<FVector>& OutPositionsCm, TArray<FVector>& OutNormals) const
{
	OutPositionsCm.Reset();
	OutNormals.Reset();
	for (const FSailState& S : Sails)
	{
		for (int32 V = 0; V < S.X.Num(); ++V)
		{
			OutPositionsCm.Add(FVector(S.X[V] * 100.f));
			OutNormals.Add(FVector(S.Normal[V]));
		}
	}
}
