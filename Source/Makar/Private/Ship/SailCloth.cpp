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
	constexpr float Clearance = 0.05f;         // m of air kept between the canvas and a rope or spar
	constexpr float SailSpacing = 0.18f;       // m kept between two sails
	constexpr float CapsuleCell = 0.6f;        // m: the ropes' grid (no rope or spar keeps the canvas further off)

	FVector3f ClothClosest(const FVector3f& P, const FVector3f& A, const FVector3f& B)
	{
		const FVector3f AB = B - A;
		const float Length2 = AB.SizeSquared();
		const float T = Length2 > 1e-8f ? FMath::Clamp(FVector3f::DotProduct(P - A, AB) / Length2, 0.f, 1.f) : 0.f;
		return A + AB * T;
	}

	/** Closest points of segments P0-P1 and A-B: the parameters along each. */
	void ClothSegments(const FVector3f& P0, const FVector3f& P1, const FVector3f& A, const FVector3f& B, float& OutS, float& OutT)
	{
		const FVector3f D1 = P1 - P0, D2 = B - A, R = P0 - A;
		const float AA = D1.SizeSquared(), EE = D2.SizeSquared(), F = FVector3f::DotProduct(D2, R);
		const float C = FVector3f::DotProduct(D1, R), Bd = FVector3f::DotProduct(D1, D2);
		const float Denominator = AA * EE - Bd * Bd;
		float S = Denominator > 1e-9f ? FMath::Clamp((Bd * F - C * EE) / Denominator, 0.f, 1.f) : 0.f;
		float T = EE > 1e-9f ? (Bd * S + F) / EE : 0.f;
		if (T < 0.f || T > 1.f)
		{
			T = FMath::Clamp(T, 0.f, 1.f);
			S = AA > 1e-9f ? FMath::Clamp((Bd * T - C) / AA, 0.f, 1.f) : 0.f;
		}
		OutS = S;
		OutT = T;
	}
}

void FShipVoxels::Reset(const FBox3f& Bounds, float InCell)
{
	Cell = InCell;
	Origin = Bounds.Min;
	const FVector3f Extent = Bounds.GetSize() / Cell;
	Size = FIntVector(FMath::CeilToInt(Extent.X) + 1, FMath::CeilToInt(Extent.Y) + 1, FMath::CeilToInt(Extent.Z) + 1);
	Cells.SetNumZeroed(int64(Size.X) * Size.Y * Size.Z);
}

void FShipVoxels::Fill(const FVector3f& A, const FVector3f& B, const FVector3f& C)
{
	FBox3f Box(ForceInit);
	Box += A;
	Box += B;
	Box += C;
	if (Box.Max.X < Origin.X || Box.Max.Y < Origin.Y || Box.Max.Z < Origin.Z
		|| Box.Min.X > Origin.X + Size.X * Cell || Box.Min.Y > Origin.Y + Size.Y * Cell || Box.Min.Z > Origin.Z + Size.Z * Cell)
	{
		return;
	}
	// Samples 0.8 cell apart over the triangle: every cell it passes through is marked.
	const float Step = Cell * 0.8f; // a cell skipped at a corner is filled by Grow
	const int32 NU = FMath::Clamp(FMath::CeilToInt((B - A).Size() / Step), 1, 4000);
	const int32 NV = FMath::Clamp(FMath::CeilToInt((C - A).Size() / Step), 1, 4000);
	for (int32 I = 0; I <= NU; ++I)
	{
		const float S = float(I) / NU;
		const int32 JMax = FMath::Min(NV, FMath::CeilToInt((1.f - S) * NV));
		for (int32 J = 0; J <= JMax; ++J)
		{
			const float T = FMath::Min(float(J) / NV, 1.f - S);
			const FVector3f Q = ((A + (B - A) * S + (C - A) * T) - Origin) / Cell;
			const int32 X = FMath::FloorToInt(Q.X), Y = FMath::FloorToInt(Q.Y), Z = FMath::FloorToInt(Q.Z);
			if (X >= 0 && Y >= 0 && Z >= 0 && X < Size.X && Y < Size.Y && Z < Size.Z)
			{
				Cells[(int64(Z) * Size.Y + Y) * Size.X + X] = 1; // the same value from any thread
			}
		}
	}
}

void FShipVoxels::Grow()
{
	ParallelFor(Size.Z, [this](int32 Z)
	{
		for (int32 Y = 0; Y < Size.Y; ++Y)
		{
			for (int32 X = 0; X < Size.X; ++X)
			{
				uint8& C = Cells[(int64(Z) * Size.Y + Y) * Size.X + X];
				if (C & 1)
				{
					continue;
				}
				auto Wood = [this](int32 I, int32 J, int32 K)
				{
					return I >= 0 && J >= 0 && K >= 0 && I < Size.X && J < Size.Y && K < Size.Z && (Cells[(int64(K) * Size.Y + J) * Size.X + I] & 1);
				};
				if (Wood(X - 1, Y, Z) || Wood(X + 1, Y, Z) || Wood(X, Y - 1, Z) || Wood(X, Y + 1, Z) || Wood(X, Y, Z - 1) || Wood(X, Y, Z + 1))
				{
					C |= 2; // only the wood bit is read: no cell grows from a grown one
				}
			}
		}
	});
}

void FSailSimulation::Init(TArray<FShipSail> InSails, const TArray<FShipCapsule>& Colliders, const TArray<FShipBlock>& Solids, const FShipHullShape& InHull,
	TSharedPtr<const FShipVoxels> InVoxels)
{
	Sails.Reset();
	Hull = InHull;
	Voxels = InVoxels;
	Accumulator = 0.f;
	for (FShipSail& Spec : InSails)
	{
		FSailState& S = Sails.AddDefaulted_GetRef();
		const int32 Count = Spec.Rest.Num();
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
		// The ropes and spars this sail can reach.
		FBox3f Box(ForceInit);
		for (const FVector3f& P : Spec.Rest)
		{
			Box += P;
		}
		Box = Box.ExpandBy(FMath::Max(Box.GetExtent().GetMax() * 0.5f, 1.f));
		for (const FShipCapsule& Capsule : Colliders)
		{
			FBox3f CapsuleBox(ForceInit);
			CapsuleBox += Capsule.A;
			CapsuleBox += Capsule.B;
			if (CapsuleBox.ExpandBy(Capsule.Radius).Intersect(Box))
			{
				S.Near.Add(Capsule);
			}
		}
		for (const FShipBlock& Solid : Solids)
		{
			const FVector3f Reach = (Solid.X * Solid.Half.X).GetAbs() + (Solid.Y * Solid.Half.Y).GetAbs() + (Solid.Z * Solid.Half.Z).GetAbs();
			if (FBox3f(Solid.O - Reach, Solid.O + Reach).Intersect(Box))
			{
				S.NearSolids.Add(Solid);
			}
		}
		S.FrameStart = S.X;
		S.GridOrigin = Box.Min;
		S.Spec = MoveTemp(Spec);
		UpdateNormals(S);
	}
	ParallelFor(Sails.Num(), [this](int32 Index) { BuildGrid(Sails[Index]); });
}

void FSailSimulation::BuildGrid(FSailState& S)
{
	FBox3f Box(ForceInit);
	for (const FVector3f& P : S.Spec.Rest)
	{
		Box += P;
	}
	Box = Box.ExpandBy(FMath::Max(Box.GetExtent().GetMax() * 0.5f, 1.f));
	S.GridOrigin = Box.Min;
	const FVector3f Extent = Box.GetSize() / CapsuleCell;
	S.GridSize = FIntVector(FMath::CeilToInt(Extent.X) + 1, FMath::CeilToInt(Extent.Y) + 1, FMath::CeilToInt(Extent.Z) + 1);
	const int32 Cells = S.GridSize.X * S.GridSize.Y * S.GridSize.Z;
	// Each capsule into the cells round points half a cell apart along it.
	TArray<uint64> Pairs;
	for (int32 C = 0; C < S.Near.Num(); ++C)
	{
		const FShipCapsule& Capsule = S.Near[C];
		const int32 N = FMath::Clamp(FMath::CeilToInt((Capsule.B - Capsule.A).Size() / (CapsuleCell * 0.5f)), 1, 1000);
		for (int32 K = 0; K <= N; ++K)
		{
			const FVector3f Q = (FMath::Lerp(Capsule.A, Capsule.B, float(K) / N) - S.GridOrigin) / CapsuleCell;
			const int32 X = FMath::FloorToInt(Q.X), Y = FMath::FloorToInt(Q.Y), Z = FMath::FloorToInt(Q.Z);
			for (int32 DZ = -1; DZ <= 1; ++DZ)
			{
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DX = -1; DX <= 1; ++DX)
					{
						const int32 I = X + DX, J = Y + DY, L = Z + DZ;
						if (I >= 0 && J >= 0 && L >= 0 && I < S.GridSize.X && J < S.GridSize.Y && L < S.GridSize.Z)
						{
							Pairs.Add((uint64((L * S.GridSize.Y + J) * S.GridSize.X + I) << 32) | uint32(C));
						}
					}
				}
			}
		}
	}
	Pairs.Sort();
	S.GridStart.Init(0, Cells + 1);
	S.GridItems.Reset(Pairs.Num());
	uint64 Last = ~uint64(0);
	for (const uint64 Pair : Pairs)
	{
		if (Pair == Last)
		{
			continue;
		}
		Last = Pair;
		++S.GridStart[int32(Pair >> 32) + 1];
		S.GridItems.Add(int32(Pair & 0xffffffffu));
	}
	for (int32 I = 0; I < Cells; ++I)
	{
		S.GridStart[I + 1] += S.GridStart[I];
	}
}

TArrayView<const int32> FSailSimulation::CapsulesAt(const FSailState& S, const FVector3f& P)
{
	const FVector3f Q = (P - S.GridOrigin) / CapsuleCell;
	const int32 X = FMath::FloorToInt(Q.X), Y = FMath::FloorToInt(Q.Y), Z = FMath::FloorToInt(Q.Z);
	if (X < 0 || Y < 0 || Z < 0 || X >= S.GridSize.X || Y >= S.GridSize.Y || Z >= S.GridSize.Z || S.GridStart.Num() == 0)
	{
		return TArrayView<const int32>();
	}
	const int32 Cell = (Z * S.GridSize.Y + Y) * S.GridSize.X + X;
	return TArrayView<const int32>(S.GridItems.GetData() + S.GridStart[Cell], S.GridStart[Cell + 1] - S.GridStart[Cell]);
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
	// Canvas.
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
	// Ropes and spars.
	for (int32 V = 0; V < Count; ++V)
	{
		if (S.InvMass[V] <= 0.f)
		{
			continue;
		}
		FVector3f& P = S.X[V];
		for (const int32 CapsuleIndex : CapsulesAt(S, P))
		{
			const FShipCapsule& Capsule = S.Near[CapsuleIndex];
			const FVector3f C = ClothClosest(P, Capsule.A, Capsule.B);
			const FVector3f Away = P - C;
			const float Distance = Away.Size();
			const float Keep = Capsule.Radius + Clearance;
			if (Distance < Keep)
			{
				// Out the way it came in (the side its previous position was on).
				const FVector3f Side = Distance > 1e-4f ? Away / Distance : (S.Prev[V] - C).GetSafeNormal();
				P = C + Side * Keep;
			}
		}
	}
}

void FSailSimulation::CollideSolids(FSailState& S) const
{
	// The canvas rests on the decks, rails, bulwarks and tops instead of passing into them: a point inside a solid
	// goes back out through the face it came in by (the side it was on when the frame began), and stops there.
	for (int32 V = 0; V < S.X.Num(); ++V)
	{
		if (S.InvMass[V] <= 0.f)
		{
			continue;
		}
		FVector3f& P = S.X[V];
		// The hull's side, as its lines draw it: a point does not go through the planking; one that would comes back to
		// the side it was on (outboard, or inboard over the deck), or over the rail if that is nearer.
		if (Hull.IsValid())
		{
			const float H = P.Z - Hull.ZOffset;
			const float Rail = Hull.RailAt(P.X);
			const float Half = H < Rail + Clearance ? Hull.HalfAt(P.X, FMath::Min(H, Rail)) : 0.f;
			if (Half > 0.05f)
			{
				const FVector3f& P0 = S.FrameStart[V];
				const float Half0 = Hull.HalfAt(P0.X, FMath::Min(P0.Z - Hull.ZOffset, Hull.RailAt(P0.X)));
				const bool bWasOutside = Half0 <= 0.05f || FMath::Abs(P0.Y) >= Half0 - 0.5f * Hull.Wall;
				const float Sign = FMath::Abs(P0.Y) > 1e-3f ? FMath::Sign(P0.Y) : (P.Y >= 0.f ? 1.f : -1.f);
				const float Over = Rail + Clearance - H; // to go over the rail
				if (bWasOutside && FMath::Abs(P.Y) < Half + Clearance)
				{
					const float Out = Half + Clearance - Sign * P.Y;
					if (Over < Out)
					{
						P.Z += Over;
					}
					else
					{
						P.Y = Sign * (Half + Clearance);
					}
					S.Prev[V] = P;
				}
				else if (!bWasOutside && FMath::Abs(P.Y) > Half - Hull.Wall - Clearance)
				{
					const float In = Sign * P.Y - (Half - Hull.Wall - Clearance);
					if (Over < In)
					{
						P.Z += Over;
					}
					else
					{
						P.Y = Sign * (Half - Hull.Wall - Clearance);
					}
					S.Prev[V] = P;
				}
			}
		}
		for (const FShipBlock& K : S.NearSolids)
		{
			const FVector3f D = P - K.O;
			const FVector3f Q(FVector3f::DotProduct(D, K.X), FVector3f::DotProduct(D, K.Y), FVector3f::DotProduct(D, K.Z));
			const FVector3f Keep = K.Half + FVector3f(Clearance);
			if (FMath::Abs(Q.X) >= Keep.X || FMath::Abs(Q.Y) >= Keep.Y || FMath::Abs(Q.Z) >= Keep.Z)
			{
				continue;
			}
			const FVector3f D0 = S.FrameStart[V] - K.O;
			const FVector3f Q0(FVector3f::DotProduct(D0, K.X), FVector3f::DotProduct(D0, K.Y), FVector3f::DotProduct(D0, K.Z));
			int32 Axis = INDEX_NONE;
			float Sign = 1.f, Best = 1e9f;
			bool bFromOutside = false;
			for (int32 A = 0; A < 3; ++A)
			{
				const bool bOutside = FMath::Abs(Q0[A]) >= Keep[A] - 1e-4f;
				if (bFromOutside && !bOutside)
				{
					continue;
				}
				const float S0 = bOutside ? FMath::Sign(Q0[A]) : (Q[A] >= 0.f ? 1.f : -1.f);
				const float Depth = Keep[A] - S0 * Q[A];
				if ((bOutside && !bFromOutside) || Depth < Best)
				{
					Best = Depth;
					Axis = A;
					Sign = S0;
					bFromOutside = bFromOutside || bOutside;
				}
			}
			const FVector3f Normal = Axis == 0 ? K.X : Axis == 1 ? K.Y : K.Z;
			P += Normal * (Sign * Best);
			S.Prev[V] = P; // it stops against the solid
		}
	}
	// The ropes between two points of the canvas: each edge of the cloth kept clear of the ropes and spars near it (the
	// points alone would let a stay slip through the panel between them).
	const FShipSail& Spec = S.Spec;
	auto Edge = [&](int32 E0, int32 E1)
	{
		const float W0 = S.InvMass[E0] > 0.f ? 1.f : 0.f, W1 = S.InvMass[E1] > 0.f ? 1.f : 0.f;
		if (W0 == 0.f || W1 == 0.f)
		{
			return; // an edge on a spar or a stay is held by it
		}
		// The capsules near five points along the edge (no two more than a quarter edge apart).
		TArray<int32, TInlineAllocator<32>> Candidates;
		for (int32 K = 0; K <= 4; ++K)
		{
			for (const int32 CapsuleIndex : CapsulesAt(S, FMath::Lerp(S.X[E0], S.X[E1], K * 0.25f)))
			{
				Candidates.AddUnique(CapsuleIndex);
			}
		}
		for (const int32 CapsuleIndex : Candidates)
		{
			const FShipCapsule& Capsule = S.Near[CapsuleIndex];
			float U, T;
			ClothSegments(S.X[E0], S.X[E1], Capsule.A, Capsule.B, U, T);
			if (U < 0.05f || U > 0.95f)
			{
				continue; // the points' own test handles it
			}
			const FVector3f OnEdge = FMath::Lerp(S.X[E0], S.X[E1], U);
			const FVector3f OnRope = FMath::Lerp(Capsule.A, Capsule.B, T);
			const FVector3f Away = OnEdge - OnRope;
			const float Distance = Away.Size();
			const float Keep = Capsule.Radius + Clearance;
			if (Distance >= Keep)
			{
				continue;
			}
			// Back to the side the edge was on when the frame began.
			const FVector3f Before = FMath::Lerp(S.FrameStart[E0], S.FrameStart[E1], U) - OnRope;
			FVector3f Side = Distance > 1e-4f ? Away / Distance : Before.GetSafeNormal();
			if (FVector3f::DotProduct(Side, Before) < 0.f && Before.SizeSquared() > 1e-8f)
			{
				// It crossed during the frame: it goes back.
				Side = (Before - FVector3f::DotProduct(Before, (Capsule.B - Capsule.A).GetSafeNormal()) * (Capsule.B - Capsule.A).GetSafeNormal()).GetSafeNormal();
			}
			const FVector3f Delta = OnRope + Side * Keep - OnEdge;
			const float A0 = 1.f - U, A1 = U, Scale = 1.f / (A0 * A0 + A1 * A1);
			S.X[E0] += Delta * (A0 * Scale);
			S.X[E1] += Delta * (A1 * Scale);
		}
	};
	for (int32 J = 0; J <= Spec.NV; ++J)
	{
		for (int32 I = 0; I <= Spec.NU; ++I)
		{
			if (I < Spec.NU)
			{
				Edge(Spec.Index(I, J), Spec.Index(I + 1, J));
			}
			if (J < Spec.NV)
			{
				Edge(Spec.Index(I, J), Spec.Index(I, J + 1));
			}
		}
	}
	// Everything drawn in wood and iron: each point followed along its whole path of the frame, stopped where it
	// would enter the solid; a facet whose middle or an edge whose middle would enter it goes back to where it was
	// (where nothing went through). Points that began inside (the cut sail) are let out.
	if (Voxels.IsValid() && Voxels->IsValid())
	{
		const FShipVoxels& G = *Voxels;
		const float Step = G.Cell * 0.5f;
		for (int32 V = 0; V < S.X.Num(); ++V)
		{
			if (S.InvMass[V] <= 0.f)
			{
				continue;
			}
			const FVector3f A = S.FrameStart[V], Bp = S.X[V];
			if (G.Blocked(A))
			{
				continue;
			}
			const int32 Steps = FMath::Clamp(FMath::CeilToInt((Bp - A).Size() / Step), 1, 64);
			FVector3f Free = A;
			for (int32 K = 1; K <= Steps; ++K)
			{
				const FVector3f Q = FMath::Lerp(A, Bp, float(K) / Steps);
				if (G.Blocked(Q))
				{
					S.X[V] = Free;
					S.Prev[V] = Free;
					break;
				}
				Free = Q;
			}
		}
		auto Back = [&](int32 V)
		{
			if (S.InvMass[V] > 0.f)
			{
				S.X[V] = S.FrameStart[V];
				S.Prev[V] = S.FrameStart[V];
			}
		};
		auto Enters = [&](const FVector3f& Now, const FVector3f& Before) { return G.Blocked(Now) && !G.Blocked(Before); };
		for (int32 J = 0; J < Spec.NV; ++J)
		{
			for (int32 I = 0; I < Spec.NU; ++I)
			{
				const int32 C0 = Spec.Index(I, J), C1 = Spec.Index(I + 1, J), C2 = Spec.Index(I + 1, J + 1), C3 = Spec.Index(I, J + 1);
				const FVector3f Mid = (S.X[C0] + S.X[C1] + S.X[C2] + S.X[C3]) * 0.25f;
				const FVector3f Mid0 = (S.FrameStart[C0] + S.FrameStart[C1] + S.FrameStart[C2] + S.FrameStart[C3]) * 0.25f;
				if (Enters(Mid, Mid0) || Enters((S.X[C0] + S.X[C1]) * 0.5f, (S.FrameStart[C0] + S.FrameStart[C1]) * 0.5f)
					|| Enters((S.X[C0] + S.X[C3]) * 0.5f, (S.FrameStart[C0] + S.FrameStart[C3]) * 0.5f))
				{
					Back(C0);
					Back(C1);
					Back(C2);
					Back(C3);
				}
			}
		}
	}
	S.FrameStart = S.X;
}

int32 FSailSimulation::CountInWood(int32& OutSamples) const
{
	OutSamples = 0;
	int32 Inside = 0;
	if (!Voxels.IsValid() || !Voxels->IsValid())
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
				const FVector3f& A = S.X[Spec.Index(I, J)];
				const FVector3f& B = S.X[Spec.Index(I + 1, J)];
				const FVector3f& C = S.X[Spec.Index(I + 1, J + 1)];
				const FVector3f& D = S.X[Spec.Index(I, J + 1)];
				// Facets on a spar (pinned along it) touch its wood by design.
				if (S.InvMass[Spec.Index(I, J)] <= 0.f || S.InvMass[Spec.Index(I + 1, J)] <= 0.f || S.InvMass[Spec.Index(I + 1, J + 1)] <= 0.f || S.InvMass[Spec.Index(I, J + 1)] <= 0.f)
				{
					continue;
				}
				for (int32 U = 0; U < 4; ++U)
				{
					for (int32 V = 0; V < 4; ++V)
					{
						const float FU = (U + 0.5f) / 4.f, FV = (V + 0.5f) / 4.f;
						++OutSamples;
						Inside += Voxels->InWood(FMath::Lerp(FMath::Lerp(A, B, FU), FMath::Lerp(D, C, FU), FV)) ? 1 : 0;
					}
				}
			}
		}
	}
	return Inside;
}

int32 FSailSimulation::CountInHull(int32& OutSamples) const
{
	OutSamples = 0;
	int32 Inside = 0;
	if (!Hull.IsValid())
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
				const FVector3f& A = S.X[Spec.Index(I, J)];
				const FVector3f& B = S.X[Spec.Index(I + 1, J)];
				const FVector3f& C = S.X[Spec.Index(I + 1, J + 1)];
				const FVector3f& D = S.X[Spec.Index(I, J + 1)];
				for (int32 U = 0; U < 4; ++U)
				{
					for (int32 V = 0; V < 4; ++V)
					{
						const float FU = (U + 0.5f) / 4.f, FV = (V + 0.5f) / 4.f;
						const FVector3f P = FMath::Lerp(FMath::Lerp(A, B, FU), FMath::Lerp(D, C, FU), FV);
						++OutSamples;
						const float H = P.Z - Hull.ZOffset;
						if (H >= Hull.RailAt(P.X))
						{
							continue;
						}
						const float Half = Hull.HalfAt(P.X, H);
						if (Half > 0.05f && FMath::Abs(P.Y) < Half && FMath::Abs(P.Y) > Half - Hull.Wall)
						{
							++Inside;
						}
					}
				}
			}
		}
	}
	return Inside;
}

void FSailSimulation::CollideSails()
{
	// Every point hashed by its cell (SailSpacing) into a table sorted once (counting sort); points of different
	// sails closer than SailSpacing are pushed apart.
	struct FEntry
	{
		int32 Sail, Point;
	};
	int32 Total = 0;
	for (const FSailState& S : Sails)
	{
		Total += S.X.Num();
	}
	const uint32 Mask = FMath::RoundUpToPowerOfTwo(uint32(FMath::Max(Total * 2, 64))) - 1;
	auto Cell = [](const FVector3f& P) { return FIntVector(FMath::FloorToInt(P.X / SailSpacing), FMath::FloorToInt(P.Y / SailSpacing), FMath::FloorToInt(P.Z / SailSpacing)); };
	auto Bucket = [Mask](const FIntVector& C) { return (uint32(C.X) * 73856093u ^ uint32(C.Y) * 19349663u ^ uint32(C.Z) * 83492791u) & Mask; };
	TArray<int32> Start;
	Start.Init(0, Mask + 2);
	TArray<uint32> Of;
	Of.SetNumUninitialized(Total);
	int32 N = 0;
	for (const FSailState& S : Sails)
	{
		for (const FVector3f& P : S.X)
		{
			Of[N] = Bucket(Cell(P));
			++Start[Of[N] + 1];
			++N;
		}
	}
	for (uint32 B = 0; B <= Mask; ++B)
	{
		Start[B + 1] += Start[B];
	}
	TArray<FEntry> Items;
	Items.SetNumUninitialized(Total);
	TArray<int32> Fill = Start;
	N = 0;
	for (int32 S = 0; S < Sails.Num(); ++S)
	{
		for (int32 V = 0; V < Sails[S].X.Num(); ++V)
		{
			Items[Fill[Of[N]]++] = { S, V };
			++N;
		}
	}
	for (int32 S = 0; S < Sails.Num(); ++S)
	{
		FSailState& Sail = Sails[S];
		for (int32 V = 0; V < Sail.X.Num(); ++V)
		{
			if (Sail.InvMass[V] <= 0.f)
			{
				continue;
			}
			const FIntVector C = Cell(Sail.X[V]);
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DZ = -1; DZ <= 1; ++DZ)
					{
						const uint32 B = Bucket(C + FIntVector(DX, DY, DZ));
						for (int32 K = Start[B]; K < Start[B + 1]; ++K)
						{
							const FEntry& E = Items[K];
							if (E.Sail == S)
							{
								continue;
							}
							FSailState& Other = Sails[E.Sail];
							const FVector3f Delta = Sail.X[V] - Other.X[E.Point];
							const float Distance2 = Delta.SizeSquared();
							if (Distance2 >= SailSpacing * SailSpacing || Distance2 < 1e-10f)
							{
								continue;
							}
							const float Distance = FMath::Sqrt(Distance2);
							const float WA = Sail.InvMass[V], WB = Other.InvMass[E.Point];
							const FVector3f Push = Delta * ((SailSpacing - Distance) / (Distance * (WA + WB)));
							Sail.X[V] += Push * WA;
							Other.X[E.Point] -= Push * WB;
						}
					}
				}
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
	double T = FPlatformTime::Seconds();
	ParallelFor(Sails.Num(), [&](int32 Index)
	{
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			Substep(Sails[Index], SubstepTime, Wind, Time + Step * SubstepTime);
		}
	});
	Profile[0] += FPlatformTime::Seconds() - T;
	T = FPlatformTime::Seconds();
	ParallelFor(Sails.Num(), [&](int32 Index) { CollideSolids(Sails[Index]); });
	Profile[1] += FPlatformTime::Seconds() - T;
	T = FPlatformTime::Seconds();
	CollideSails();
	Profile[2] += FPlatformTime::Seconds() - T;
	T = FPlatformTime::Seconds();
	ParallelFor(Sails.Num(), [&](int32 Index) { UpdateNormals(Sails[Index]); });
	Profile[3] += FPlatformTime::Seconds() - T;
	Profile[4] += Steps;
	Profile[5] += 1.0;
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
