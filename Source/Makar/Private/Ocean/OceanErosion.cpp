#include "Ocean/OceanErosion.h"

#include "Math/RandomStream.h"

bool FOceanHeightfield::Contains(const FVector2f& PositionM) const
{
	const FVector2f Grid = (PositionM - OriginM) / CellM;
	return IsValid() && Grid.X >= 0.f && Grid.Y >= 0.f && Grid.X <= float(Width - 1) && Grid.Y <= float(Height - 1);
}

float FOceanHeightfield::SampleArray(const TArray<float>& Values, const FVector2f& PositionM) const
{
	const FVector2f Grid = (PositionM - OriginM) / CellM;
	const float GX = FMath::Clamp(Grid.X, 0.f, float(Width - 1) - 1e-3f);
	const float GY = FMath::Clamp(Grid.Y, 0.f, float(Height - 1) - 1e-3f);
	const int32 X = int32(GX);
	const int32 Y = int32(GY);
	const float U = GX - X;
	const float V = GY - Y;
	const int32 I = Y * Width + X;
	return FMath::Lerp(FMath::Lerp(Values[I], Values[I + 1], U), FMath::Lerp(Values[I + Width], Values[I + Width + 1], U), V);
}

namespace OceanErosion
{
	namespace
	{
		const int32 NeighbourX[8] = { 1, -1, 0, 0, 1, -1, 1, -1 };
		const int32 NeighbourY[8] = { 0, 0, 1, -1, 1, 1, -1, -1 };
		const float NeighbourDistance[8] = { 1.f, 1.f, 1.f, 1.f, UE_SQRT_2, UE_SQRT_2, UE_SQRT_2, UE_SQRT_2 };

		/** Fills every depression up to its spill height plus a tiny slope, so that every land cell drains to an outlet
		 * (the sea, or the edge of the grid). Priority flood (Barnes, Lehman and Mulla 2014) with an epsilon gradient. */
		void FillDepressions(TArray<float>& Heights, int32 W, int32 H, float SeaLevel)
		{
			struct FCell
			{
				float Height;
				int32 Index;
			};
			auto Lower = [](const FCell& A, const FCell& B) { return A.Height < B.Height; };
			TArray<FCell> Open;
			Open.Reserve(W * H / 4);
			TBitArray<> Closed(false, W * H);
			for (int32 Y = 0; Y < H; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					const int32 I = Y * W + X;
					if (Heights[I] < SeaLevel || X == 0 || Y == 0 || X == W - 1 || Y == H - 1)
					{
						Open.HeapPush({ Heights[I], I }, Lower);
						Closed[I] = true;
					}
				}
			}
			constexpr float Epsilon = 1e-4f;
			while (Open.Num() > 0)
			{
				FCell Cell;
				Open.HeapPop(Cell, Lower, EAllowShrinking::No);
				const int32 X = Cell.Index % W;
				const int32 Y = Cell.Index / W;
				for (int32 N = 0; N < 8; ++N)
				{
					const int32 NX = X + NeighbourX[N];
					const int32 NY = Y + NeighbourY[N];
					if (NX < 0 || NY < 0 || NX >= W || NY >= H)
					{
						continue;
					}
					const int32 NI = NY * W + NX;
					if (Closed[NI])
					{
						continue;
					}
					Closed[NI] = true;
					Heights[NI] = FMath::Max(Heights[NI], Heights[Cell.Index] + Epsilon);
					Open.HeapPush({ Heights[NI], NI }, Lower);
				}
			}
		}
	}

	/** Stream power iterations, with optional uplift (added each iteration) and hillslope diffusion (C = D dt / dx2). */
	static void Evolve(FOceanHeightfield& Field, const TArray<float>* Erodibility, const TArray<float>* Uplift, int32 Iterations,
		float K, float M, float Diffusion, float SeaLevel)
	{
		const int32 W = Field.Width;
		const int32 H = Field.Height;
		const int32 Count = W * H;
		TArray<float>& Heights = Field.Heights;
		TArray<int32> Receiver, DonorCount, DonorStart, Donors, Stack;
		TArray<float> Area, ReceiverDistance;
		Receiver.SetNumUninitialized(Count);
		ReceiverDistance.SetNumUninitialized(Count);
		DonorCount.SetNumUninitialized(Count);
		DonorStart.SetNumUninitialized(Count + 1);
		Donors.SetNumUninitialized(Count);
		Stack.SetNumUninitialized(Count);
		Area.SetNumUninitialized(Count);
		const float CellArea = Field.CellM * Field.CellM;

		TArray<float> Previous;
		for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
		{
			if (Uplift)
			{
				for (int32 I = 0; I < Count; ++I)
				{
					Heights[I] += (*Uplift)[I];
				}
			}
			FillDepressions(Heights, W, H, SeaLevel);

			// Receivers: the steepest lower neighbour (outlets drain to themselves).
			FMemory::Memzero(DonorCount.GetData(), Count * sizeof(int32));
			for (int32 Y = 0; Y < H; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					const int32 I = Y * W + X;
					Receiver[I] = I;
					ReceiverDistance[I] = 1.f;
					if (Heights[I] < SeaLevel || X == 0 || Y == 0 || X == W - 1 || Y == H - 1)
					{
						continue;
					}
					float Steepest = 0.f;
					for (int32 N = 0; N < 8; ++N)
					{
						const int32 NI = (Y + NeighbourY[N]) * W + X + NeighbourX[N];
						const float Slope = (Heights[I] - Heights[NI]) / NeighbourDistance[N];
						if (Slope > Steepest)
						{
							Steepest = Slope;
							Receiver[I] = NI;
							ReceiverDistance[I] = NeighbourDistance[N] * Field.CellM;
						}
					}
					if (Receiver[I] != I)
					{
						++DonorCount[Receiver[I]];
					}
				}
			}
			// Donor lists (compressed), then the stack: each outlet followed by everything upstream of it, receivers
			// always before their donors.
			DonorStart[0] = 0;
			for (int32 I = 0; I < Count; ++I)
			{
				DonorStart[I + 1] = DonorStart[I] + DonorCount[I];
			}
			FMemory::Memzero(DonorCount.GetData(), Count * sizeof(int32));
			for (int32 I = 0; I < Count; ++I)
			{
				if (Receiver[I] != I)
				{
					const int32 R = Receiver[I];
					Donors[DonorStart[R] + DonorCount[R]++] = I;
				}
			}
			int32 StackSize = 0;
			for (int32 I = 0; I < Count; ++I)
			{
				if (Receiver[I] != I)
				{
					continue;
				}
				int32 Read = StackSize;
				Stack[StackSize++] = I;
				while (Read < StackSize)
				{
					const int32 C = Stack[Read++];
					for (int32 D = DonorStart[C]; D < DonorStart[C + 1]; ++D)
					{
						Stack[StackSize++] = Donors[D];
					}
				}
			}

			// Drained area, from the ridges down.
			for (int32 I = 0; I < Count; ++I)
			{
				Area[I] = CellArea;
			}
			for (int32 S = StackSize - 1; S >= 0; --S)
			{
				const int32 I = Stack[S];
				if (Receiver[I] != I)
				{
					Area[Receiver[I]] += Area[I];
				}
			}

			// Implicit incision, from the outlets up: h = (h + F h_receiver) / (1 + F), F = K A^m / distance.
			for (int32 S = 0; S < StackSize; ++S)
			{
				const int32 I = Stack[S];
				const int32 R = Receiver[I];
				if (R == I)
				{
					continue;
				}
				const float F = K * (Erodibility ? (*Erodibility)[I] : 1.f) * FMath::Pow(Area[I], M) / ReceiverDistance[I];
				Heights[I] = (Heights[I] + F * Heights[R]) / (1.f + F);
			}

			// Hillslope diffusion (soil creep) rounds the ridges; outlets stay put.
			if (Diffusion > 0.f)
			{
				Previous = Heights;
				for (int32 Y = 1; Y + 1 < H; ++Y)
				{
					for (int32 X = 1; X + 1 < W; ++X)
					{
						const int32 I = Y * W + X;
						if (Receiver[I] != I)
						{
							Heights[I] += Diffusion * (Previous[I - 1] + Previous[I + 1] + Previous[I - W] + Previous[I + W] - 4.f * Previous[I]);
						}
					}
				}
			}
		}

		// Drainage area as the runoff map (log scale, normalised).
		Field.Flow.SetNumUninitialized(Count);
		float MaxFlow = 0.f;
		for (int32 I = 0; I < Count; ++I)
		{
			Field.Flow[I] = Heights[I] < SeaLevel ? 0.f : FMath::Loge(Area[I] / CellArea);
			MaxFlow = FMath::Max(MaxFlow, Field.Flow[I]);
		}
		for (float& Value : Field.Flow)
		{
			Value = MaxFlow > 0.f ? Value / MaxFlow : 0.f;
		}
	}

	void StreamPower(FOceanHeightfield& Field, const TArray<float>& Erodibility, int32 Iterations, float K, float M, float SeaLevel)
	{
		Evolve(Field, &Erodibility, nullptr, Iterations, K, M, 0.f, SeaLevel);
	}

	void LandscapeEvolution(FOceanHeightfield& Field, const TArray<float>& Uplift, int32 Iterations, float K, float M, float Diffusion)
	{
		Evolve(Field, nullptr, &Uplift, Iterations, K, M, Diffusion, 1e-3f);
	}

	void Hydraulic(FOceanHeightfield& Field, const FHydraulicSettings& Settings, const TArray<float>& Erodibility, int32 Seed)
	{
		const int32 W = Field.Width;
		const int32 H = Field.Height;
		TArray<float>& Heights = Field.Heights;
		TArray<float> Flow;
		Flow.SetNumZeroed(W * H);

		// Erosion brush: the cells within Radius, weighted by closeness, weights summing to 1.
		struct FBrushCell
		{
			int32 DX;
			int32 DY;
			float Weight;
		};
		TArray<FBrushCell> Brush;
		float WeightSum = 0.f;
		const int32 R = FMath::Max(Settings.Radius, 1);
		for (int32 DY = -R; DY <= R; ++DY)
		{
			for (int32 DX = -R; DX <= R; ++DX)
			{
				const float Distance = FMath::Sqrt(float(DX * DX + DY * DY));
				if (Distance <= R)
				{
					const float Weight = 1.f - Distance / (R + 1.f);
					Brush.Add({ DX, DY, Weight });
					WeightSum += Weight;
				}
			}
		}
		for (FBrushCell& Cell : Brush)
		{
			Cell.Weight /= WeightSum;
		}

		// Bilinear height and gradient at a point of the grid (in cells).
		auto HeightAndGradient = [&](float PX, float PY, float& OutHeight, float& OutGX, float& OutGY)
		{
			const int32 X = int32(PX);
			const int32 Y = int32(PY);
			const float U = PX - X;
			const float V = PY - Y;
			const int32 I = Y * W + X;
			const float H00 = Heights[I], H10 = Heights[I + 1], H01 = Heights[I + W], H11 = Heights[I + W + 1];
			OutGX = (H10 - H00) * (1.f - V) + (H11 - H01) * V;
			OutGY = (H01 - H00) * (1.f - U) + (H11 - H10) * U;
			OutHeight = H00 * (1.f - U) * (1.f - V) + H10 * U * (1.f - V) + H01 * (1.f - U) * V + H11 * U * V;
		};

		FRandomStream Random(Seed);
		for (int32 Droplet = 0; Droplet < Settings.Droplets; ++Droplet)
		{
			float PX = Random.FRandRange(1.f, W - 2.f);
			float PY = Random.FRandRange(1.f, H - 2.f);
			float Height, GX, GY;
			HeightAndGradient(PX, PY, Height, GX, GY);
			if (Height < Settings.RainAbove)
			{
				continue; // rain on the sea
			}
			float DirX = 0.f, DirY = 0.f, Speed = 1.f, Water = 1.f, Sediment = 0.f;
			for (int32 Step = 0; Step < Settings.Lifetime; ++Step)
			{
				const int32 NX = int32(PX);
				const int32 NY = int32(PY);
				const float U = PX - NX;
				const float V = PY - NY;
				const int32 Node = NY * W + NX;
				HeightAndGradient(PX, PY, Height, GX, GY);

				// Downhill, with some inertia.
				DirX = DirX * Settings.Inertia - GX * (1.f - Settings.Inertia);
				DirY = DirY * Settings.Inertia - GY * (1.f - Settings.Inertia);
				const float Length = FMath::Sqrt(DirX * DirX + DirY * DirY);
				if (Length < 1e-6f)
				{
					break; // a flat pit: the droplet soaks in
				}
				DirX /= Length;
				DirY /= Length;
				Flow[Node] += Water;
				PX += DirX;
				PY += DirY;
				if (PX < 1.f || PY < 1.f || PX >= W - 2.f || PY >= H - 2.f)
				{
					break;
				}

				float NewHeight, NGX, NGY;
				HeightAndGradient(PX, PY, NewHeight, NGX, NGY);
				const float Drop = NewHeight - Height; // negative downhill
				const float Capacity = FMath::Max(-Drop * Speed * Water * Settings.CapacityFactor, Settings.MinCapacity);
				if (Sediment > Capacity || Drop > 0.f)
				{
					// Uphill: fill the pit behind; over capacity: drop part of the load. On the four corners of the cell left.
					const float Deposit = Drop > 0.f ? FMath::Min(Drop, Sediment) : (Sediment - Capacity) * Settings.DepositSpeed;
					Sediment -= Deposit;
					Heights[Node] += Deposit * (1.f - U) * (1.f - V);
					Heights[Node + 1] += Deposit * U * (1.f - V);
					Heights[Node + W] += Deposit * (1.f - U) * V;
					Heights[Node + W + 1] += Deposit * U * V;
				}
				else
				{
					// Under capacity: erode the ground around, never deeper than the drop (no holes).
					const float Amount = FMath::Min((Capacity - Sediment) * Settings.ErodeSpeed, -Drop);
					for (const FBrushCell& Cell : Brush)
					{
						const int32 BX = NX + Cell.DX;
						const int32 BY = NY + Cell.DY;
						if (BX < 0 || BY < 0 || BX >= W || BY >= H)
						{
							continue;
						}
						const int32 BI = BY * W + BX;
						const float Removed = Amount * Cell.Weight * Erodibility[BI];
						Heights[BI] -= Removed;
						Sediment += Removed;
					}
				}
				Speed = FMath::Sqrt(FMath::Max(0.f, Speed * Speed - Drop * Settings.Gravity));
				Water *= 1.f - Settings.Evaporation;
				if (NewHeight < Settings.SeaLevel)
				{
					break; // into the sea, sediment and all
				}
			}
		}

		// Runoff normalised to ~1 in the main gullies (log scale: it spans orders of magnitude).
		float MaxFlow = 0.f;
		for (float& Value : Flow)
		{
			Value = FMath::Loge(1.f + Value);
			MaxFlow = FMath::Max(MaxFlow, Value);
		}
		if (MaxFlow > 0.f)
		{
			for (float& Value : Flow)
			{
				Value /= MaxFlow;
			}
		}
		Field.Flow = MoveTemp(Flow);
	}

	void Thermal(FOceanHeightfield& Field, const TArray<float>& Talus, int32 Iterations, float Rate)
	{
		const int32 W = Field.Width;
		const int32 H = Field.Height;
		TArray<float>& Heights = Field.Heights;
		TArray<float> Delta;
		Delta.SetNumUninitialized(W * H);
		static const int32 Offsets[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
		for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
		{
			FMemory::Memzero(Delta.GetData(), Delta.Num() * sizeof(float));
			// Each node gives to its lower neighbours what exceeds the angle of repose (its own material's), in
			// proportion to the excesses; gathered into Delta, then applied, so the pass does not depend on the order.
			for (int32 Y = 1; Y + 1 < H; ++Y)
			{
				for (int32 X = 1; X + 1 < W; ++X)
				{
					const int32 I = Y * W + X;
					const float Here = Heights[I];
					float Excess[8];
					float Total = 0.f;
					float Largest = 0.f;
					for (int32 N = 0; N < 8; ++N)
					{
						const float Distance = N < 4 ? Field.CellM : Field.CellM * UE_SQRT_2;
						const float Difference = Here - Heights[I + Offsets[N][1] * W + Offsets[N][0]] - Talus[I] * Distance;
						Excess[N] = FMath::Max(Difference, 0.f);
						Total += Excess[N];
						Largest = FMath::Max(Largest, Excess[N]);
					}
					if (Total <= 0.f)
					{
						continue;
					}
					const float Moved = Rate * 0.5f * Largest;
					Delta[I] -= Moved;
					for (int32 N = 0; N < 8; ++N)
					{
						if (Excess[N] > 0.f)
						{
							Delta[I + Offsets[N][1] * W + Offsets[N][0]] += Moved * Excess[N] / Total;
						}
					}
				}
			}
			for (int32 I = 0; I < W * H; ++I)
			{
				Heights[I] += Delta[I];
			}
		}
	}
}
