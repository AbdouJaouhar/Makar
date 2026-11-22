#include "Ocean/OceanSimulation.h"

#include "Async/ParallelFor.h"
#include "Math/RandomStream.h"

#include <cmath>

namespace
{
	constexpr float OceanGravity = 9.81f;

	/** A cascade keeps wavelengths longer than its tile size divided by this; finer ones go to the next cascade. */
	constexpr float CascadeBandSplit = 6.f;

	float Gaussian(FRandomStream& Random)
	{
		// Box-Muller
		const float U1 = FMath::Max(Random.GetFraction(), 1e-6f);
		const float U2 = Random.GetFraction();
		return FMath::Sqrt(-2.f * FMath::Loge(U1)) * FMath::Cos(2.f * UE_PI * U2);
	}
}

void FOceanSimulation::Initialize(const FOceanSettings& InSettings)
{
	Settings = InSettings;

	N = int32(FMath::RoundUpToPowerOfTwo(uint32(FMath::Clamp(Settings.Resolution, 16, 1024))));
	LogN = FMath::FloorLog2(uint32(N));

	BitReverse.SetNumUninitialized(N);
	for (int32 Index = 0; Index < N; ++Index)
	{
		int32 Reversed = 0;
		for (int32 Bit = 0; Bit < LogN; ++Bit)
		{
			if (Index & (1 << Bit))
			{
				Reversed |= 1 << (LogN - 1 - Bit);
			}
		}
		BitReverse[Index] = Reversed;
	}

	// Inverse transform: positive exponent.
	Twiddles.SetNumUninitialized(N / 2);
	for (int32 Index = 0; Index < N / 2; ++Index)
	{
		const float Angle = 2.f * UE_PI * float(Index) / float(N);
		Twiddles[Index] = { FMath::Cos(Angle), FMath::Sin(Angle) };
	}

	// JONSWAP parameters from wind speed and fetch.
	const float U = FMath::Max(Settings.WindSpeed, 0.1f);
	const float F = FMath::Max(Settings.Fetch, 100.f);
	Alpha = 0.076f * FMath::Pow(U * U / (F * OceanGravity), 0.22f);
	PeakOmega = 22.f * FMath::Pow(OceanGravity * OceanGravity / (U * F), 1.f / 3.f);

	const float Sizes[NumCascades] = { Settings.Cascade0Size, Settings.Cascade1Size, Settings.Cascade2Size };
	for (int32 Index = 0; Index < NumCascades; ++Index)
	{
		const float KMin = Index == 0 ? 0.f : CascadeBandSplit * 2.f * UE_PI / Sizes[Index];
		const float KMax = Index + 1 < NumCascades ? CascadeBandSplit * 2.f * UE_PI / Sizes[Index + 1] : TNumericLimits<float>::Max();
		InitializeCascade(Cascades[Index], FMath::Max(Sizes[Index], 0.1f), KMin, KMax, Settings.Seed + Index * 7919);
	}

	for (TArray<FComplex>& Grid : Work)
	{
		Grid.SetNumUninitialized(N * N);
	}
	DisplacementBase.SetNumUninitialized(N * N);
	DerivativeBase.SetNumUninitialized(N * N);
	MomentBase.SetNumUninitialized(N * N);
}

void FOceanSimulation::Dispersion(float K, float& OutOmega, float& OutDOmegaDK) const
{
	const float KH = FMath::Min(K * FMath::Max(Settings.Depth, 1.f), 20.f);
	const float T = FMath::Tanh(KH);
	OutOmega = FMath::Sqrt(OceanGravity * K * T);
	OutDOmegaDK = OutOmega > 0.f ? OceanGravity * (T + KH * (1.f - T * T)) / (2.f * OutOmega) : 0.f;
}

float FOceanSimulation::JonswapSpectrum(float Omega) const
{
	if (Omega <= 0.f)
	{
		return 0.f;
	}
	const float Sigma = Omega <= PeakOmega ? 0.07f : 0.09f;
	const float R = FMath::Exp(-FMath::Square(Omega - PeakOmega) / (2.f * Sigma * Sigma * PeakOmega * PeakOmega));
	const float Ratio = PeakOmega / Omega;
	return Alpha * OceanGravity * OceanGravity / FMath::Pow(Omega, 5.f)
		* FMath::Exp(-1.25f * Ratio * Ratio * Ratio * Ratio)
		* FMath::Pow(Settings.PeakEnhancement, R);
}

float FOceanSimulation::DirectionalSpreading(float Omega, float Theta) const
{
	// Hasselmann spreading exponent, plus a swell term that narrows long waves.
	const float Ratio = Omega / PeakOmega;
	double S = Ratio > 1.f ? 9.77 * FMath::Pow(Ratio, -2.5f) : 6.97 * FMath::Pow(Ratio, 5.f);
	S += 16.0 * FMath::Tanh(PeakOmega / FMath::Max(Omega, 1e-4f)) * Settings.Swell * Settings.Swell;
	S = FMath::Max(S, 1e-3);

	// Normalisation so the spreading integrates to 1 over [-pi, pi]: 2^(2s-1)/pi * Gamma(s+1)^2 / Gamma(2s+1).
	const double LogQ = (2.0 * S - 1.0) * std::log(2.0) - std::log(double(UE_PI)) + 2.0 * std::lgamma(S + 1.0) - std::lgamma(2.0 * S + 1.0);
	const double D = std::exp(LogQ) * std::pow(std::abs(std::cos(0.5 * double(Theta))), 2.0 * S);

	return FMath::Lerp(float(D), 1.f / (2.f * UE_PI), FMath::Clamp(Settings.DirectionalSpread, 0.f, 1.f));
}

void FOceanSimulation::InitializeCascade(FCascade& Cascade, float PatchSize, float KMin, float KMax, int32 Seed) const
{
	const int32 Count = N * N;
	Cascade.PatchSize = PatchSize;
	Cascade.H0.SetNumZeroed(Count);
	Cascade.H0MinusConj.SetNumZeroed(Count);
	Cascade.Omega.SetNumZeroed(Count);
	Cascade.K.SetNumZeroed(Count);
	Cascade.Foam.SetNumZeroed(Count);

	FRandomStream Random(Seed);
	const float DeltaK = 2.f * UE_PI / PatchSize;
	const float WindRadians = FMath::DegreesToRadians(Settings.WindDirection);

	for (int32 Y = 0; Y < N; ++Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			const int32 Index = Y * N + X;
			const int32 NX = X < N / 2 ? X : X - N;
			const int32 NY = Y < N / 2 ? Y : Y - N;
			const FVector2f KVec(NX * DeltaK, NY * DeltaK);
			const float K = KVec.Size();

			float Omega = 0.f;
			float DOmegaDK = 0.f;
			Dispersion(K, Omega, DOmegaDK);
			Cascade.K[Index] = KVec;
			Cascade.Omega[Index] = Omega;

			// Always draw, so the random field does not depend on which k are filtered out.
			const float G1 = Gaussian(Random);
			const float G2 = Gaussian(Random);

			if (K < 1e-6f || K < KMin || K >= KMax)
			{
				continue;
			}

			// Spectrum over (omega, theta) converted to a density over the (kx, ky) plane.
			const float Theta = FMath::Atan2(KVec.Y, KVec.X) - WindRadians;
			const float Spectrum = JonswapSpectrum(Omega) * DirectionalSpreading(Omega, Theta) * DOmegaDK / K
				* FMath::Exp(-K * K * Settings.ShortWaveFade * Settings.ShortWaveFade);

			// E|h0|^2 = S dk^2 / 2, so that the surface variance equals the spectrum integral.
			const float Amplitude = 0.5f * FMath::Sqrt(FMath::Max(Spectrum, 0.f)) * DeltaK * Settings.AmplitudeScale;
			Cascade.H0[Index] = { G1 * Amplitude, G2 * Amplitude };
		}
	}

	for (int32 Y = 0; Y < N; ++Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			const FComplex& Minus = Cascade.H0[((N - Y) % N) * N + (N - X) % N];
			Cascade.H0MinusConj[Y * N + X] = { Minus.Re, -Minus.Im };
		}
	}
}

void FOceanSimulation::Simulate(float Time, float DeltaTime, FOceanFrame& Out)
{
	Out.Time = Time;
	Out.Cascades.SetNum(NumCascades);
	for (int32 Index = 0; Index < NumCascades; ++Index)
	{
		SimulateCascade(Cascades[Index], Time, DeltaTime, Out.Cascades[Index]);
	}
	Out.bValid = true;
}

void FOceanSimulation::SimulateCascade(FCascade& Cascade, float Time, float DeltaTime, FOceanCascadeFrame& Out)
{
	// Two real fields A and B share one complex FFT: transform A + iB, read A from Re and B from Im.
	auto Pack = [](const FComplex& A, const FComplex& B) { return FComplex{ A.Re - B.Im, A.Im + B.Re }; };
	auto Scale = [](const FComplex& C, float S) { return FComplex{ C.Re * S, C.Im * S }; };

	// 1) Evolve the spectrum to time t and build the 8 fields in frequency space.
	ParallelFor(N, [&](int32 Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			const int32 Index = Y * N + X;
			const FComplex& A = Cascade.H0[Index];
			const FComplex& B = Cascade.H0MinusConj[Index];

			float S, C;
			FMath::SinCos(&S, &C, Cascade.Omega[Index] * Time);

			// h(k,t) = h0(k) e^{-i w t} + conj(h0(-k)) e^{i w t}  (waves travel along +k)
			const FComplex H{
				A.Re * C + A.Im * S + B.Re * C - B.Im * S,
				A.Im * C - A.Re * S + B.Im * C + B.Re * S };
			const FComplex IH{ -H.Im, H.Re }; // i * h

			const FVector2f K = Cascade.K[Index];
			const float KLen = K.Size();
			const float InvK = KLen > 1e-6f ? 1.f / KLen : 0.f;

			const FComplex DispX = Scale(IH, K.X * InvK);           // horizontal displacement
			const FComplex DispY = Scale(IH, K.Y * InvK);
			const FComplex DHeightDX = Scale(IH, K.X);              // slopes
			const FComplex DHeightDY = Scale(IH, K.Y);
			const FComplex DDispXDX = Scale(H, -K.X * K.X * InvK);  // Jacobian terms
			const FComplex DDispYDY = Scale(H, -K.Y * K.Y * InvK);
			const FComplex DDispXDY = Scale(H, -K.X * K.Y * InvK);

			Work[0][Index] = Pack(DispX, DispY);
			Work[1][Index] = Pack(H, DDispXDY);
			Work[2][Index] = Pack(DHeightDX, DHeightDY);
			Work[3][Index] = Pack(DDispXDX, DDispYDY);
		}
	});

	// 2) Back to space.
	InverseFFT2D(Work, 4);

	// 3) Assemble outputs, accumulate foam where the surface is compressed.
	const float Lambda = Settings.Choppiness;
	const float FoamFade = FMath::Exp(-Settings.FoamDecay * FMath::Max(DeltaTime, 0.f));
	Out.PatchSize = Cascade.PatchSize;
	Out.Resolution = N;
	Out.Displacement.SetNumUninitialized(N * N);
	Out.JacobianTerms.SetNumUninitialized(N * N);

	ParallelFor(N, [&](int32 Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			const int32 Index = Y * N + X;
			const float DispX = Work[0][Index].Re * Lambda;
			const float DispY = Work[0][Index].Im * Lambda;
			const float Height = Work[1][Index].Re;
			const float DDispXDY = Work[1][Index].Im * Lambda;
			const float DDispXDX = Work[3][Index].Re * Lambda;
			const float DDispYDY = Work[3][Index].Im * Lambda;

			const float Jacobian = (1.f + DDispXDX) * (1.f + DDispYDY) - DDispXDY * DDispXDY;
			const float NewFoam = FMath::Clamp((Settings.FoamThreshold - Jacobian) * Settings.FoamIntensity, 0.f, 1.f);
			float& Foam = Cascade.Foam[Index];
			Foam = FMath::Max(NewFoam, Foam * FoamFade);

			Out.Displacement[Index] = FVector3f(DispX, DispY, Height);
			Out.JacobianTerms[Index] = FVector3f(DDispXDX, DDispYDY, DDispXDY);
			DisplacementBase[Index] = FVector4f(DispX, DispY, Height, Foam);
			const float SlopeX = Work[2][Index].Re;
			const float SlopeY = Work[2][Index].Im;
			DerivativeBase[Index] = FVector4f(SlopeX, SlopeY, DDispXDX, DDispYDY);
			MomentBase[Index] = FVector4f(SlopeX * SlopeX, SlopeY * SlopeY, SlopeX * SlopeY, 0.f);
		}
	});

	double SumOfSquares = 0.0;
	for (const FVector3f& Displacement : Out.Displacement)
	{
		SumOfSquares += double(Displacement.Z) * Displacement.Z;
	}
	Out.SignificantHeight = 4.f * float(FMath::Sqrt(SumOfSquares / FMath::Max(Out.Displacement.Num(), 1)));

	BuildMips(DisplacementBase, Out.DisplacementMips);
	BuildMips(DerivativeBase, Out.DerivativeMips);
	BuildMips(MomentBase, Out.MomentMips);
}

void FOceanSimulation::InverseFFT1D(FComplex* Data) const
{
	for (int32 Index = 0; Index < N; ++Index)
	{
		const int32 Reversed = BitReverse[Index];
		if (Index < Reversed)
		{
			Swap(Data[Index], Data[Reversed]);
		}
	}

	for (int32 Size = 2; Size <= N; Size <<= 1)
	{
		const int32 Half = Size / 2;
		const int32 Step = N / Size;
		for (int32 Start = 0; Start < N; Start += Size)
		{
			for (int32 Offset = 0; Offset < Half; ++Offset)
			{
				const FComplex& W = Twiddles[Offset * Step];
				FComplex& A = Data[Start + Offset];
				FComplex& B = Data[Start + Offset + Half];
				const FComplex T{ W.Re * B.Re - W.Im * B.Im, W.Re * B.Im + W.Im * B.Re };
				B = { A.Re - T.Re, A.Im - T.Im };
				A = { A.Re + T.Re, A.Im + T.Im };
			}
		}
	}
}

void FOceanSimulation::InverseFFT2D(TArray<FComplex>* Grids, int32 NumGrids) const
{
	// Rows
	ParallelFor(NumGrids * N, [&](int32 Job)
	{
		InverseFFT1D(Grids[Job / N].GetData() + (Job % N) * N);
	});

	// Columns
	ParallelFor(NumGrids * N, [&](int32 Job)
	{
		FComplex* Grid = Grids[Job / N].GetData();
		const int32 Column = Job % N;
		TArray<FComplex, TInlineAllocator<512>> Line;
		Line.SetNumUninitialized(N);
		for (int32 Row = 0; Row < N; ++Row)
		{
			Line[Row] = Grid[Row * N + Column];
		}
		InverseFFT1D(Line.GetData());
		for (int32 Row = 0; Row < N; ++Row)
		{
			Grid[Row * N + Column] = Line[Row];
		}
	});
}

void FOceanSimulation::BuildMips(const TArray<FVector4f>& Base, TArray<TArray<FFloat16Color>>& OutMips) const
{
	const int32 NumMips = GetNumMips();
	OutMips.SetNum(NumMips);

	TArray<FVector4f> Current = Base;
	TArray<FVector4f> Next;
	int32 Size = N;

	for (int32 Mip = 0; Mip < NumMips; ++Mip)
	{
		TArray<FFloat16Color>& Dest = OutMips[Mip];
		Dest.SetNumUninitialized(Size * Size);
		for (int32 Index = 0; Index < Size * Size; ++Index)
		{
			const FVector4f& V = Current[Index];
			Dest[Index] = FFloat16Color(FLinearColor(V.X, V.Y, V.Z, V.W));
		}

		if (Mip + 1 < NumMips)
		{
			const int32 NextSize = FMath::Max(Size / 2, 1);
			Next.SetNumUninitialized(NextSize * NextSize);
			for (int32 Y = 0; Y < NextSize; ++Y)
			{
				for (int32 X = 0; X < NextSize; ++X)
				{
					const int32 SX = X * 2;
					const int32 SY = Y * 2;
					Next[Y * NextSize + X] = (Current[SY * Size + SX] + Current[SY * Size + SX + 1]
						+ Current[(SY + 1) * Size + SX] + Current[(SY + 1) * Size + SX + 1]) * 0.25f;
				}
			}
			Swap(Current, Next);
			Size = NextSize;
		}
	}
}
