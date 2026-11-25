#include "RopeSolver.h"

#include "Async/ParallelFor.h"
#include "ShipMeshBuilder.h"

namespace
{
	constexpr int32 Iterations = 80;
	constexpr float Gap = 0.005f; // m between a rope and what it rests on

	FVector3f RopeClosest(const FVector3f& P, const FVector3f& A, const FVector3f& B)
	{
		const FVector3f AB = B - A;
		const float Length2 = AB.SizeSquared();
		const float T = Length2 > 1e-8f ? FMath::Clamp(FVector3f::DotProduct(P - A, AB) / Length2, 0.f, 1.f) : 0.f;
		return A + AB * T;
	}

	/** Pushes P out of a capsule; true if it was inside. */
	bool OutOfCapsule(FVector3f& P, const FShipCapsule& C, float Radius, const FVector3f& Hint)
	{
		const FVector3f Q = RopeClosest(P, C.A, C.B);
		const FVector3f Away = P - Q;
		const float Distance = Away.Size();
		const float Keep = C.Radius + Radius + Gap;
		if (Distance >= Keep)
		{
			return false;
		}
		FVector3f Dir = Distance > 1e-5f ? Away / Distance : Hint;
		if (Dir.IsNearlyZero())
		{
			Dir = FVector3f::UpVector;
		}
		P = Q + Dir * Keep;
		return true;
	}

	/** Pushes P out of a box through its nearest face; true if it was inside. */
	bool OutOfBlock(FVector3f& P, const FShipBlock& K, float Radius)
	{
		const FVector3f D = P - K.O;
		const float L[3] = { FVector3f::DotProduct(D, K.X), FVector3f::DotProduct(D, K.Y), FVector3f::DotProduct(D, K.Z) };
		const float H[3] = { K.Half.X + Radius + Gap, K.Half.Y + Radius + Gap, K.Half.Z + Radius + Gap };
		int32 Axis = -1;
		float Least = TNumericLimits<float>::Max();
		for (int32 I = 0; I < 3; ++I)
		{
			const float Depth = H[I] - FMath::Abs(L[I]);
			if (Depth <= 0.f)
			{
				return false;
			}
			if (Depth < Least)
			{
				Least = Depth;
				Axis = I;
			}
		}
		const FVector3f Axes[3] = { K.X, K.Y, K.Z };
		P += Axes[Axis] * (L[Axis] >= 0.f ? Least : -Least);
		return true;
	}
}

int32 ShipRopes::Lay(ShipMesh::FBuilder& B, const TArray<FShipRope>& Ropes, const TArray<FShipCapsule>& Spars, const TArray<FShipBlock>& Blocks)
{
	TArray<TArray<FVector3f>> Laid;
	Laid.SetNum(Ropes.Num());
	TArray<int32> Inside;
	Inside.SetNumZeroed(Ropes.Num());
	ParallelFor(Ropes.Num(), [&](int32 Index)
	{
		const FShipRope& R = Ropes[Index];
		const float Span = (R.B - R.A).Size();
		TArray<FVector3f>& P = Laid[Index];
		const int32 Segments = FMath::Clamp(FMath::CeilToInt(Span / 0.12f), 2, 200);
		// Length: the straight distance for a rope set up taut; a parabola's arc for one hanging slack.
		const float Length = R.Sag > 0.001f ? Span + 8.f * R.Sag * R.Sag / (3.f * FMath::Max(Span, 0.01f)) : Span;
		const float Rest = Length / Segments;
		for (int32 I = 0; I <= Segments; ++I)
		{
			const float T = float(I) / Segments;
			P.Add(FMath::Lerp(R.A, R.B, T) - FVector3f(0.f, 0.f, 4.f * R.Sag * T * (1.f - T)));
		}
		// The solids near this rope (its ends excepted: it is made fast to them).
		FBox3f Box(ForceInit);
		for (const FVector3f& Q : P)
		{
			Box += Q;
		}
		Box = Box.ExpandBy(0.5f);
		TArray<const FShipCapsule*> NearSpars;
		for (const FShipCapsule& C : Spars)
		{
			FBox3f CB(ForceInit);
			CB += C.A;
			CB += C.B;
			if (CB.ExpandBy(C.Radius).Intersect(Box))
			{
				NearSpars.Add(&C);
			}
		}
		TArray<const FShipBlock*> NearBlocks;
		for (const FShipBlock& K : Blocks)
		{
			const float Reach = K.Half.Size();
			if (FBox3f(K.O - FVector3f(Reach), K.O + FVector3f(Reach)).Intersect(Box))
			{
				NearBlocks.Add(&K);
			}
		}
		if (NearSpars.Num() == 0 && NearBlocks.Num() == 0 && R.Sag <= 0.001f)
		{
			return; // straight and clear
		}
		const FVector3f Side = FVector3f::CrossProduct(R.B - R.A, FVector3f::UpVector).GetSafeNormal();
		for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
		{
			// The chain: no point farther from the next than its share of the length (a rope does not stretch, but it
			// can go slack).
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				for (int32 I = 0; I < Segments; ++I)
				{
					FVector3f& A = P[I];
					FVector3f& C = P[I + 1];
					const FVector3f D = C - A;
					const float Distance = D.Size();
					if (Distance <= Rest || Distance < 1e-6f)
					{
						continue;
					}
					const float WA = I == 0 ? 0.f : 1.f, WC = I + 1 == Segments ? 0.f : 1.f;
					if (WA + WC <= 0.f)
					{
						continue;
					}
					const FVector3f Fix = D * ((Distance - Rest) / (Distance * (WA + WC)));
					A += Fix * WA;
					C -= Fix * WC;
				}
			}
			// Weight (slack ropes hang), then the solids it rests on.
			for (int32 I = 1; I < Segments; ++I)
			{
				P[I].Z -= R.Sag > 0.001f ? 0.004f : 0.f;
				for (const FShipCapsule* C : NearSpars)
				{
					OutOfCapsule(P[I], *C, R.Radius, Side);
				}
				for (const FShipBlock* K : NearBlocks)
				{
					OutOfBlock(P[I], *K, R.Radius);
				}
			}
		}
		// Whatever is still inside a solid, points and the middles of the segments between them.
		for (int32 I = 3; I < 2 * Segments - 3; ++I)
		{
			FVector3f Q = I % 2 ? (P[I / 2] + P[I / 2 + 1]) * 0.5f : P[I / 2];
			bool bIn = false;
			for (const FShipCapsule* C : NearSpars)
			{
				FVector3f T = Q;
				bIn |= OutOfCapsule(T, FShipCapsule{ C->A, C->B, C->Radius - 0.02f - Gap }, R.Radius * 0.5f, Side);
			}
			Inside[Index] += bIn ? 1 : 0;
		}
	});
	int32 Total = 0;
	for (int32 Index = 0; Index < Ropes.Num(); ++Index)
	{
		const FShipRope& R = Ropes[Index];
		const int32 Sides = R.Radius > 0.025f ? 6 : 4;
		if (Laid[Index].Num() == 0)
		{
			B.Tube(ShipMesh::Rope, R.A, R.B, R.Radius, R.Radius, Sides, R.Color);
			continue;
		}
		// Points in line with their neighbours dropped (within 1 cm): straight runs are one segment again.
		const TArray<FVector3f>& Full = Laid[Index];
		TArray<FVector3f> Kept = { Full[0] };
		for (int32 I = 1; I + 1 < Full.Num(); ++I)
		{
			const FVector3f& From = Kept.Last();
			const FVector3f& Next = Full[I + 1];
			bool bNeeded = false;
			// Every point skipped since the last kept one must stay within 1 cm of the chord to the next.
			for (int32 J = I; J > 0 && Full[J] != From; --J)
			{
				if ((Full[J] - RopeClosest(Full[J], From, Next)).Size() > 0.01f)
				{
					bNeeded = true;
					break;
				}
			}
			if (bNeeded)
			{
				Kept.Add(Full[I]);
			}
		}
		Kept.Add(Full.Last());
		TArray<float> Radii;
		Radii.Init(R.Radius, Kept.Num());
		B.Tube(ShipMesh::Rope, Kept, Radii, Sides, R.Color);
		if (Inside[Index] > 0)
		{
			UE_LOG(LogTemp, Display, TEXT("ShipRopes: rope %d from %s to %s, r %.3f: %d points inside"), Index, *R.A.ToString(), *R.B.ToString(), R.Radius, Inside[Index]);
		}
		Total += Inside[Index];
	}
	return Total;
}
