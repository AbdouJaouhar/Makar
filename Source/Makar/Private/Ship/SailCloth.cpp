#include "SailCloth.h"

#include "Async/ParallelFor.h"

namespace
{
	constexpr float AirDensity = 1.2f;        // kg/m3
	constexpr float PressureCoefficient = 1.3f;
	constexpr float CanvasDensity = 0.75f;    // kg/m2: heavy flax sailcloth
	constexpr float SubstepTime = 1.f / 300.f;
	constexpr int32 MaxSubsteps = 10;          // per frame; a slow frame slows the cloth rather than exploding it
	constexpr float Damping = 0.004f;          // per substep
	constexpr float Clearance = 0.05f;         // m of air kept between the canvas and a rope or spar
	constexpr float SailSpacing = 0.18f;       // m kept between two sails

	FVector3f ClosestOnSegment(const FVector3f& P, const FVector3f& A, const FVector3f& B)
	{
		const FVector3f AB = B - A;
		const float Length2 = AB.SizeSquared();
		const float T = Length2 > 1e-8f ? FMath::Clamp(FVector3f::DotProduct(P - A, AB) / Length2, 0.f, 1.f) : 0.f;
		return A + AB * T;
	}
}

void FSailSimulation::Init(TArray<FShipSail> InSails, const TArray<FShipCapsule>& Colliders)
{
	Sails.Reset();
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
		for (const FShipCapsule& Capsule : S.Near)
		{
			const FVector3f C = ClosestOnSegment(P, Capsule.A, Capsule.B);
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

void FSailSimulation::CollideSails()
{
	// A spatial hash of every point; points of different sails closer than SailSpacing are pushed apart.
	struct FEntry
	{
		int32 Sail, Point;
	};
	TMultiMap<FIntVector, FEntry> Grid;
	auto Cell = [](const FVector3f& P) { return FIntVector(FMath::FloorToInt(P.X / SailSpacing), FMath::FloorToInt(P.Y / SailSpacing), FMath::FloorToInt(P.Z / SailSpacing)); };
	for (int32 S = 0; S < Sails.Num(); ++S)
	{
		for (int32 V = 0; V < Sails[S].X.Num(); ++V)
		{
			Grid.Add(Cell(Sails[S].X[V]), { S, V });
		}
	}
	TArray<FEntry> Found;
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
						Found.Reset();
						Grid.MultiFind(C + FIntVector(DX, DY, DZ), Found);
						for (const FEntry& E : Found)
						{
							if (E.Sail == S)
							{
								continue;
							}
							FSailState& Other = Sails[E.Sail];
							const FVector3f Delta = Sail.X[V] - Other.X[E.Point];
							const float Distance = Delta.Size();
							if (Distance >= SailSpacing || Distance < 1e-5f)
							{
								continue;
							}
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
	Accumulator = FMath::Min(Accumulator + DeltaTime, SubstepTime * MaxSubsteps);
	const int32 Steps = FMath::FloorToInt(Accumulator / SubstepTime);
	if (Steps <= 0)
	{
		return;
	}
	Accumulator -= Steps * SubstepTime;
	ParallelFor(Sails.Num(), [&](int32 Index)
	{
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			Substep(Sails[Index], SubstepTime, Wind, Time + Step * SubstepTime);
		}
	});
	CollideSails();
	ParallelFor(Sails.Num(), [&](int32 Index) { UpdateNormals(Sails[Index]); });
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
