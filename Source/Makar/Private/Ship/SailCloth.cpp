#include "SailCloth.h"

#include "Async/ParallelFor.h"

namespace
{
	constexpr float AirDensity = 1.2f;        // kg/m3
	constexpr float PressureCoefficient = 1.3f;
	constexpr float CanvasDensity = 0.75f;    // kg/m2: heavy flax sailcloth
	constexpr float SubstepTime = 1.f / 300.f;
	constexpr int32 ClothMaxSubsteps = 10;          // per frame; a slow frame slows the cloth rather than exploding it
	constexpr float Damping = 0.004f;          // per substep

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
	constexpr float SailFitKeep = 0.12f;   // m of air kept between a fitted sail and anything
	constexpr float SailMaxRoom = 0.6f;  // m a point may stray from its fitted shape at most
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
		// Inside the wall: out by the nearest face.
		const float ToIn = Y - Inner, ToOut = Half - Y, ToTop = Rail - H;
		if (ToTop <= ToIn && ToTop <= ToOut)
		{
			Out = FVector3f::UpVector;
			return -ToTop;
		}
		Out = FVector3f(0.f, ToOut < ToIn ? Side : -Side, 0.f);
		return -FMath::Min(ToIn, ToOut);
	}
}

FString ShipSails::Fit(TArray<FShipSail>& Sails, const TArray<FShipCapsule>& Colliders, const TArray<FShipBlock>& Blocks, const FShipHullShape& Hull)
{
	const double Start = FPlatformTime::Seconds();
	const int32 N = Sails.Num();
	// What each sail can reach: the ropes, spars, blocks and other sails near its shape.
	TArray<FBox3f> Boxes;
	for (const FShipSail& S : Sails)
	{
		FBox3f Box(ForceInit);
		for (const FVector3f& P : S.Rest)
		{
			Box += P;
		}
		Boxes.Add(Box.ExpandBy(SailMaxRoom + 1.f));
	}
	TArray<TArray<int32>> NearCapsules, NearSails;
	NearCapsules.SetNum(N);
	NearSails.SetNum(N);
	FBox3f All(ForceInit);
	for (const FBox3f& Box : Boxes)
	{
		All += Box;
	}
	// Ropes, spars and blocks in a grid of 1 m cells: a cell lists every one nearer than the room any point can have
	// (whatever is further does not count). Capsules as their index, blocks as -1 - index.
	constexpr float GridCell = 1.f;
	const float Reach = SailMaxRoom + SailFitKeep + 0.05f;
	const FVector3f GridOrigin = All.Min;
	const FIntVector GridSize(FMath::CeilToInt(All.GetSize().X / GridCell) + 1, FMath::CeilToInt(All.GetSize().Y / GridCell) + 1, FMath::CeilToInt(All.GetSize().Z / GridCell) + 1);
	TArray<uint64> Pairs;
	auto ToCell = [&](const FVector3f& P)
	{
		const FVector3f Q = (P - GridOrigin) / GridCell;
		return FIntVector(FMath::FloorToInt(Q.X), FMath::FloorToInt(Q.Y), FMath::FloorToInt(Q.Z));
	};
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
		// Along the capsule in short pieces (a long stay's box would fill half the grid).
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
	for (int32 S = 0; S < N; ++S)
	{
		for (int32 C = 0; C < Colliders.Num(); ++C)
		{
			FBox3f Box(ForceInit);
			Box += Colliders[C].A;
			Box += Colliders[C].B;
			if (Box.ExpandBy(Colliders[C].Radius).Intersect(Boxes[S]))
			{
				NearCapsules[S].Add(C);
			}
		}
		for (int32 O = 0; O < N; ++O)
		{
			if (O != S && Boxes[O].Intersect(Boxes[S]))
			{
				NearSails[S].Add(O);
			}
		}
	}
	// The other sails' facets: centre and reach, refreshed as the shapes are moved.
	struct FFacet
	{
		FVector3f Centre;
		float Reach;
	};
	TArray<TArray<FFacet>> Facets;
	Facets.SetNum(N);
	TArray<TArray<FBox3f>> Rows; // each row of facets' bounds: whole rows far off are passed over
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
				for (int32 I = 0; I <= Spec.NU; ++I)
				{
					Rows[S][J] += Spec.Rest[Spec.Index(I, J)];
					Rows[S][J] += Spec.Rest[Spec.Index(I, J + 1)];
				}
				for (int32 I = 0; I < Spec.NU; ++I)
				{
					const FVector3f C[4] = { Spec.Rest[Spec.Index(I, J)], Spec.Rest[Spec.Index(I + 1, J)], Spec.Rest[Spec.Index(I + 1, J + 1)], Spec.Rest[Spec.Index(I, J + 1)] };
					const FVector3f Centre = (C[0] + C[1] + C[2] + C[3]) * 0.25f;
					float Reach = 0.f;
					for (const FVector3f& P : C)
					{
						Reach = FMath::Max(Reach, (P - Centre).Size());
					}
					Facets[S].Add({ Centre, Reach });
				}
			}
		});
	};
	// The nearest thing to P (sail S's point): its distance and the way off it. Another sail counts for SailShare of
	// the gap (both sails move: each gets half the room).
	auto Nearest = [&](int32 S, const FVector3f& P, float SailShare, FVector3f& OutDir) -> float
	{
		float Best = SailFar;
		FVector3f Dir;
		auto Take = [&](float D, const FVector3f& Away)
		{
			if (D < Best)
			{
				Best = D;
				OutDir = Away;
			}
		};
		for (const int32 Item : ObstaclesAt(P))
		{
			const float D = Item >= 0 ? CapsuleDistance(P, Colliders[Item], Dir) : BlockDistance(P, Blocks[-1 - Item], Dir);
			Take(D, Dir);
		}
		{
			const float D = HullDistance(P, Hull, Dir);
			Take(D, Dir);
		}
		for (const int32 O : NearSails[S])
		{
			const FShipSail& Other = Sails[O];
			for (int32 F = 0; F < Facets[O].Num(); ++F)
			{
				if (F % Other.NU == 0)
				{
					const float Reach = FMath::Min(Best, SailMaxRoom + SailFitKeep) / SailShare;
					if (Rows[O][F / Other.NU].ComputeSquaredDistanceToPoint(P) > Reach * Reach)
					{
						F += Other.NU - 1; // the whole row is out of reach
						continue;
					}
				}
				const FFacet& Facet = Facets[O][F];
				if ((P - Facet.Centre).Size() - Facet.Reach > FMath::Min(Best, SailMaxRoom + SailFitKeep) / SailShare)
				{
					continue;
				}
				const int32 I = F % Other.NU, J = F / Other.NU;
				const FVector Q0(Other.Rest[Other.Index(I, J)]), Q1(Other.Rest[Other.Index(I + 1, J)]), Q2(Other.Rest[Other.Index(I + 1, J + 1)]), Q3(Other.Rest[Other.Index(I, J + 1)]);
				for (int32 T = 0; T < 2; ++T)
				{
					const FVector3f Closest(FMath::ClosestPointOnTriangleToPoint(FVector(P), Q0, T == 0 ? Q1 : Q2, T == 0 ? Q2 : Q3));
					const FVector3f Away = P - Closest;
					const float L = Away.Size();
					Take(L * SailShare, L > 1e-5f ? Away / L : FVector3f::UpVector);
				}
			}
		}
		return Best;
	};
	// Each sail bellies one way: the side its facets are moved to when a rope goes through them.
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

	int32 Moved = 0, Pierced = 0;
	for (int32 Pass = 0; Pass < 8; ++Pass)
	{
		UpdateFacets();
		for (int32 S = 0; S < N; ++S)
		{
			FShipSail& Spec = Sails[S];
			// Points too near anything: off it.
			TArray<FVector3f> Next = Spec.Rest;
			int32 MovedHere = 0;
			ParallelFor(Spec.Rest.Num(), [&](int32 V)
			{
				if (Spec.Pinned[V])
				{
					return;
				}
				FVector3f Dir;
				const float D = Nearest(S, Spec.Rest[V], 1.f, Dir);
				if (D < SailFitKeep)
				{
					Next[V] = Spec.Rest[V] + Dir * (SailFitKeep - D + 0.01f);
				}
			});
			for (int32 V = 0; V < Next.Num(); ++V)
			{
				MovedHere += Next[V] != Spec.Rest[V] ? 1 : 0;
			}
			Spec.Rest = MoveTemp(Next);
			Moved += MovedHere;
			// Facets: a rope through one between its points, or any of its 3 x 3 samples too near anything, moves it off,
			// square to itself, away from what it nears (a rope through it: to the side the sail bellies to).
			TArray<FVector3f> Shift;
			Shift.Init(FVector3f::ZeroVector, Spec.Rest.Num());
			TArray<int32> Shares;
			Shares.Init(0, Spec.Rest.Num());
			FCriticalSection Lock;
			int32 PiercedHere = 0;
			ParallelFor(Spec.NU * Spec.NV, [&](int32 F)
			{
				const int32 I = F % Spec.NU, J = F / Spec.NU;
				const int32 C[4] = { Spec.Index(I, J), Spec.Index(I + 1, J), Spec.Index(I + 1, J + 1), Spec.Index(I, J + 1) };
				if (Spec.Pinned[C[0]] || Spec.Pinned[C[1]] || Spec.Pinned[C[2]] || Spec.Pinned[C[3]])
				{
					return; // a facet on its spar or stay: what is made fast there touches it by design
				}
				FVector3f Normal = FVector3f::CrossProduct(Spec.Rest[C[2]] - Spec.Rest[C[0]], Spec.Rest[C[3]] - Spec.Rest[C[1]]).GetSafeNormal();
				if (FVector3f::DotProduct(Normal, Belly[S]) < 0.f)
				{
					Normal = -Normal;
				}
				float Move = 0.f;
				const FVector Q0(Spec.Rest[C[0]]), Q1(Spec.Rest[C[1]]), Q2(Spec.Rest[C[2]]), Q3(Spec.Rest[C[3]]);
				for (const int32 K : NearCapsules[S])
				{
					const FShipCapsule& Capsule = Colliders[K];
					FVector Hit, HitNormal;
					if (!FMath::SegmentTriangleIntersection(FVector(Capsule.A), FVector(Capsule.B), Q0, Q1, Q2, Hit, HitNormal)
						&& !FMath::SegmentTriangleIntersection(FVector(Capsule.A), FVector(Capsule.B), Q0, Q2, Q3, Hit, HitNormal))
					{
						continue;
					}
					float Lowest = SailFar;
					for (const int32 V : C)
					{
						Lowest = FMath::Min(Lowest, FVector3f::DotProduct(Spec.Rest[V] - FVector3f(Hit), Normal));
					}
					Move = FMath::Max(Move, Capsule.Radius + SailFitKeep + 0.01f - Lowest);
				}
				if (Move <= 0.f)
				{
					float Worst = SailFitKeep;
					FVector3f Away = Normal;
					for (int32 U = 0; U < 3; ++U)
					{
						for (int32 W = 0; W < 3; ++W)
						{
							const float FU = (U + 0.5f) / 3.f, FW = (W + 0.5f) / 3.f;
							FVector3f Dir;
							const float D = Nearest(S, FMath::Lerp(FMath::Lerp(Spec.Rest[C[0]], Spec.Rest[C[1]], FU), FMath::Lerp(Spec.Rest[C[3]], Spec.Rest[C[2]], FU), FW), 1.f, Dir);
							if (D < Worst)
							{
								Worst = D;
								Away = Dir;
							}
						}
					}
					if (Worst < SailFitKeep)
					{
						const float Side = FVector3f::DotProduct(Away, Normal) >= 0.f ? 1.f : -1.f;
						Normal *= Side;
						Move = SailFitKeep - Worst + 0.01f;
					}
				}
				if (Move > 0.f)
				{
					FScopeLock Guard(&Lock);
					for (const int32 V : C)
					{
						Shift[V] += Normal * Move;
						++Shares[V];
					}
					++PiercedHere;
				}
			});
			for (int32 V = 0; V < Spec.Rest.Num(); ++V)
			{
				if (Shares[V] > 0 && !Spec.Pinned[V])
				{
					Spec.Rest[V] += Shift[V] / float(Shares[V]);
				}
			}
			Pierced += PiercedHere;
		}
	}

	// The room each point has: the least of its own and of the facets round it (3 x 3 samples each), less the air kept.
	UpdateFacets();
	int32 Points = 0, Held = 0;
	double MarginSum = 0.0;
	for (int32 S = 0; S < N; ++S)
	{
		FShipSail& Spec = Sails[S];
		TArray<float> FacetRoom;
		FacetRoom.Init(SailFar, Spec.NU * Spec.NV);
		ParallelFor(Spec.NU * Spec.NV, [&](int32 F)
		{
			const int32 I = F % Spec.NU, J = F / Spec.NU;
			const FVector3f A = Spec.Rest[Spec.Index(I, J)], B = Spec.Rest[Spec.Index(I + 1, J)], C = Spec.Rest[Spec.Index(I + 1, J + 1)], D = Spec.Rest[Spec.Index(I, J + 1)];
			FVector3f Dir;
			for (int32 U = 0; U < 3; ++U)
			{
				for (int32 W = 0; W < 3; ++W)
				{
					const float FU = (U + 0.5f) / 3.f, FW = (W + 0.5f) / 3.f;
					FacetRoom[F] = FMath::Min(FacetRoom[F], Nearest(S, FMath::Lerp(FMath::Lerp(A, B, FU), FMath::Lerp(D, C, FU), FW), 0.5f, Dir));
				}
			}
			// A rope may pass between two samples: no sample is further from it than half their spacing.
			const float Spacing = FMath::Max(FMath::Max((B - A).Size(), (C - D).Size()), FMath::Max((D - A).Size(), (C - B).Size())) / 3.f;
			FacetRoom[F] -= Spacing * 0.75f;
		});
		Spec.Margin.Init(0.f, Spec.Rest.Num());
		ParallelFor(Spec.Rest.Num(), [&](int32 V)
		{
			if (Spec.Pinned[V])
			{
				return;
			}
			FVector3f Dir;
			float Room = Nearest(S, Spec.Rest[V], 0.5f, Dir);
			const int32 I = V % (Spec.NU + 1), J = V / (Spec.NU + 1);
			for (int32 DJ = -1; DJ <= 0; ++DJ)
			{
				for (int32 DI = -1; DI <= 0; ++DI)
				{
					const int32 FI = I + DI, FJ = J + DJ;
					if (FI >= 0 && FJ >= 0 && FI < Spec.NU && FJ < Spec.NV)
					{
						Room = FMath::Min(Room, FacetRoom[FJ * Spec.NU + FI]);
					}
				}
			}
			Spec.Margin[V] = FMath::Clamp(Room - SailFitKeep, 0.f, SailMaxRoom);
		});
		for (int32 V = 0; V < Spec.Rest.Num(); ++V)
		{
			if (!Spec.Pinned[V])
			{
				++Points;
				Held += Spec.Margin[V] < 0.02f ? 1 : 0;
				MarginSum += Spec.Margin[V];
			}
		}
	}
	return FString::Printf(TEXT("sails fitted in %.0f ms: %d points moved off, %d facets moved off, room %.2f m on average, %d of %d points with none"),
		(FPlatformTime::Seconds() - Start) * 1000.0, Moved, Pierced, Points > 0 ? MarginSum / Points : 0.0, Held, Points);
}

void FSailSimulation::Init(TArray<FShipSail> InSails, TSharedPtr<const FShipVoxels> InVerify)
{
	Sails.Reset();
	Verify = InVerify;
	Accumulator = 0.f;
	for (FShipSail& Spec : InSails)
	{
		FSailState& S = Sails.AddDefaulted_GetRef();
		const int32 Count = Spec.Rest.Num();
		if (Spec.Margin.Num() != Count)
		{
			Spec.Margin.Init(0.f, Count);
		}
		S.X = Spec.Rest;
		S.Prev = Spec.Rest;
		S.Normal.Init(FVector3f::UpVector, Count);
		S.Force.Init(FVector3f::ZeroVector, Count);
		// Masses: each point carries a quarter of its quads' canvas.
		TArray<float> Mass;
		Mass.Init(1e-3f, Count);
		for (int32 J = 0; J < Spec.NV; ++J)
		{
			for (int32 I = 0; I < Spec.NU; ++I)
			{
				const int32 A = Spec.Index(I, J), B = Spec.Index(I + 1, J), C = Spec.Index(I + 1, J + 1), D = Spec.Index(I, J + 1);
				const float Area = 0.5f * FVector3f::CrossProduct(Spec.Rest[C] - Spec.Rest[A], Spec.Rest[D] - Spec.Rest[B]).Size();
				for (int32 V : { A, B, C, D })
				{
					Mass[V] += Area * CanvasDensity * 0.25f;
				}
			}
		}
		S.InvMass.SetNum(Count);
		for (int32 V = 0; V < Count; ++V)
		{
			S.InvMass[V] = Spec.Pinned[V] ? 0.f : 1.f / Mass[V];
		}
		// Canvas: stiff against stretching, slack in compression (it folds); shear and a little bending.
		auto Link = [&](int32 I0, int32 J0, int32 I1, int32 J1, float Stretch, float Compress)
		{
			if (I1 > Spec.NU || J1 > Spec.NV || I1 < 0)
			{
				return;
			}
			const int32 A = Spec.Index(I0, J0), B = Spec.Index(I1, J1);
			if (S.InvMass[A] + S.InvMass[B] <= 0.f)
			{
				return;
			}
			S.Constraints.Add({ A, B, (Spec.Rest[A] - Spec.Rest[B]).Size(), Stretch, Compress });
		};
		for (int32 J = 0; J <= Spec.NV; ++J)
		{
			for (int32 I = 0; I <= Spec.NU; ++I)
			{
				Link(I, J, I + 1, J, 1.f, 0.25f);
				Link(I, J, I, J + 1, 1.f, 0.25f);
				Link(I, J, I + 1, J + 1, 0.6f, 0.1f);
				Link(I, J, I - 1, J + 1, 0.6f, 0.1f);
				Link(I, J, I + 2, J, 0.15f, 0.02f);
				Link(I, J, I, J + 2, 0.15f, 0.02f);
			}
		}
		S.Spec = MoveTemp(Spec);
		UpdateNormals(S);
	}
}

void FSailSimulation::UpdateNormals(FSailState& S) const
{
	const FShipSail& Spec = S.Spec;
	for (int32 J = 0; J <= Spec.NV; ++J)
	{
		for (int32 I = 0; I <= Spec.NU; ++I)
		{
			const FVector3f DU = S.X[Spec.Index(FMath::Min(I + 1, Spec.NU), J)] - S.X[Spec.Index(FMath::Max(I - 1, 0), J)];
			// Triangular sails meet in a point at their head: take the next row's direction there.
			const FVector3f DV = S.X[Spec.Index(I, FMath::Min(J + 1, Spec.NV))] - S.X[Spec.Index(I, FMath::Max(J - 1, 0))];
			const FVector3f N = FVector3f::CrossProduct(DU, DV).GetSafeNormal();
			S.Normal[Spec.Index(I, J)] = N.IsNearlyZero() ? S.Normal[Spec.Index(I, FMath::Min(J + 1, Spec.NV))] : N;
		}
	}
}

void FSailSimulation::Substep(FSailState& S, float H, const FVector3f& Wind, float Time) const
{
	const FShipSail& Spec = S.Spec;
	const int32 Count = S.X.Num();
	// Forces: weight, and the apparent wind's pressure on each quad, square to it, shared by its corners.
	for (int32 V = 0; V < Count; ++V)
	{
		S.Force[V] = FVector3f::ZeroVector;
	}
	for (int32 J = 0; J < Spec.NV; ++J)
	{
		for (int32 I = 0; I < Spec.NU; ++I)
		{
			const int32 A = Spec.Index(I, J), B = Spec.Index(I + 1, J), C = Spec.Index(I + 1, J + 1), D = Spec.Index(I, J + 1);
			const FVector3f Cross = FVector3f::CrossProduct(S.X[C] - S.X[A], S.X[D] - S.X[B]);
			const float Area = 0.5f * Cross.Size();
			if (Area < 1e-5f)
			{
				continue;
			}
			const FVector3f N = Cross / (2.f * Area);
			const FVector3f Center = (S.X[A] + S.X[B] + S.X[C] + S.X[D]) * 0.25f;
			const FVector3f Velocity = ((S.X[A] - S.Prev[A]) + (S.X[B] - S.Prev[B]) + (S.X[C] - S.Prev[C]) + (S.X[D] - S.Prev[D])) * (0.25f / H);
			// Gusts: slow swells of the wind running through the rig.
			const float Gust = 1.f + 0.12f * FMath::Sin(Time * 0.9f + Center.X * 0.08f) + 0.07f * FMath::Sin(Time * 2.3f + Center.Z * 0.21f + Center.Y * 0.13f);
			const FVector3f Relative = Wind * Gust - Velocity;
			const float Normal = FVector3f::DotProduct(Relative, N);
			const FVector3f Pressure = N * (0.5f * AirDensity * PressureCoefficient * Area * Normal * FMath::Abs(Normal));
			const FVector3f Friction = (Relative - N * Normal) * (0.02f * AirDensity * Area * Relative.Size());
			const FVector3f Each = (Pressure + Friction) * 0.25f;
			S.Force[A] += Each;
			S.Force[B] += Each;
			S.Force[C] += Each;
			S.Force[D] += Each;
		}
	}
	// Predict.
	const FVector3f Gravity(0.f, 0.f, -9.81f);
	for (int32 V = 0; V < Count; ++V)
	{
		if (S.InvMass[V] <= 0.f)
		{
			S.X[V] = Spec.Rest[V];
			S.Prev[V] = Spec.Rest[V];
			continue;
		}
		const FVector3f Velocity = (S.X[V] - S.Prev[V]) * (1.f - Damping);
		S.Prev[V] = S.X[V];
		S.X[V] += Velocity + (Gravity + S.Force[V] * S.InvMass[V]) * (H * H);
	}
	// Canvas, sheets, then each point back within its room, twice: a 20 m sail does not stretch under its own weight,
	// and nothing it could reach is ever reached.
	for (int32 Iteration = 0; Iteration < 2; ++Iteration)
	{
		for (const FConstraint& C : S.Constraints)
		{
			const float WA = S.InvMass[C.A], WB = S.InvMass[C.B];
			const FVector3f Delta = S.X[C.B] - S.X[C.A];
			const float Length = Delta.Size();
			if (Length < 1e-6f)
			{
				continue;
			}
			const float Error = Length - C.Rest;
			const float K = Error > 0.f ? C.Stretch : C.Compress;
			const FVector3f Correction = Delta * (K * Error / (Length * (WA + WB)));
			S.X[C.A] += Correction * WA;
			S.X[C.B] -= Correction * WB;
		}
		// Sheets: they pull the clews in, never push them out.
		for (const FShipSail::FSheet& Sheet : Spec.Sheets)
		{
			FVector3f& P = S.X[Sheet.Particle];
			const FVector3f Delta = P - Sheet.Anchor;
			const float Length = Delta.Size();
			if (Length > Sheet.Length)
			{
				P = Sheet.Anchor + Delta * (Sheet.Length / Length);
			}
		}
		for (int32 V = 0; V < Count; ++V)
		{
			if (S.InvMass[V] <= 0.f)
			{
				continue;
			}
			const FVector3f Away = S.X[V] - Spec.Rest[V];
			const float Room = Spec.Margin[V];
			const float Distance2 = Away.SizeSquared();
			if (Distance2 > Room * Room)
			{
				S.X[V] = Spec.Rest[V] + Away * (Room / FMath::Sqrt(Distance2));
			}
		}
	}
}

void FSailSimulation::Step(float DeltaTime, const FVector3f& Wind, float Time)
{
	if (Sails.Num() == 0)
	{
		return;
	}
	Accumulator = FMath::Min(Accumulator + DeltaTime, SubstepTime * ClothMaxSubsteps);
	const int32 Steps = FMath::FloorToInt(Accumulator / SubstepTime);
	if (Steps <= 0)
	{
		return;
	}
	Accumulator -= Steps * SubstepTime;
	const double T = FPlatformTime::Seconds();
	ParallelFor(Sails.Num(), [&](int32 Index)
	{
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			Substep(Sails[Index], SubstepTime, Wind, Time + Step * SubstepTime);
		}
		UpdateNormals(Sails[Index]);
	});
	ProfileSeconds += FPlatformTime::Seconds() - T;
	ProfileSubsteps += Steps;
	ProfileFrames += 1.0;
}

FString FSailSimulation::TakeProfile()
{
	const double F = FMath::Max(ProfileFrames, 1.0);
	int32 Points = 0;
	for (const FSailState& S : Sails)
	{
		Points += S.X.Num();
	}
	const FString Out = FString::Printf(TEXT("cloth %.2f ms a frame, %.1f substeps a frame, %d points"), ProfileSeconds * 1000.0 / F, ProfileSubsteps / F, Points);
	ProfileSeconds = ProfileSubsteps = ProfileFrames = 0.0;
	return Out;
}

int32 FSailSimulation::CountInDrawn(int32& OutSamples, FString* OutDetail) const
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
				if (S.InvMass[C[0]] <= 0.f || S.InvMass[C[1]] <= 0.f || S.InvMass[C[2]] <= 0.f || S.InvMass[C[3]] <= 0.f)
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
						if (Cell & 1)
						{
							++Inside;
							if (OutDetail && Inside <= 12)
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

int32 FSailSimulation::CountSailCrossings() const
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

void FSailSimulation::GetTopology(TArray<int32>& OutTriangles, TArray<FVector2D>& OutUV, TArray<FColor>& OutColors) const
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

void FSailSimulation::GetSurface(TArray<FVector>& OutPositionsCm, TArray<FVector>& OutNormals) const
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

void FSailSimulation::GetSheets(TArray<TPair<FVector3f, FVector3f>>& Out) const
{
	Out.Reset();
	for (const FSailState& S : Sails)
	{
		for (const FShipSail::FSheet& Sheet : S.Spec.Sheets)
		{
			Out.Add({ S.X[Sheet.Particle], Sheet.Anchor });
		}
	}
}
