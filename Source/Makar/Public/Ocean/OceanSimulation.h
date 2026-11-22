#pragma once

#include "CoreMinimal.h"
#include "Math/Float16Color.h"
#include "OceanSettings.h"

/** Result of one simulation step for one cascade. */
struct FOceanCascadeFrame
{
	float PatchSize = 0.f;
	int32 Resolution = 0;

	/** Significant wave height of this cascade alone (4 x RMS height), m. Drives the shallow-water limit. */
	float SignificantHeight = 0.f;

	/** Full-precision displacement (X, Y horizontal, Z height), metres. Used for CPU water queries. */
	TArray<FVector3f> Displacement;

	/** Horizontal displacement gradient (dDispX/dX, dDispY/dY, dDispX/dY), choppiness applied. Feeds the foam map. */
	TArray<FVector3f> JacobianTerms;

	/** GPU data, one array per mip. RGBA = displacement X, Y, Z (m), foam. */
	TArray<TArray<FFloat16Color>> DisplacementMips;

	/** GPU data, one array per mip. RGBA = dHeight/dX, dHeight/dY, dDispX/dX, dDispY/dY. */
	TArray<TArray<FFloat16Color>> DerivativeMips;

	/**
	 * GPU data, one array per mip. RGBA = slope second moments: sx^2, sy^2, sx*sy (slopes = dHeight/dX, dY).
	 * Mip filtering averages them, so Moments - Derivatives^2 is the slope variance hidden inside a texel
	 * (LEAN mapping): the shader turns it into roughness instead of letting distant waves flatten into a mirror.
	 */
	TArray<TArray<FFloat16Color>> MomentMips;
};

struct FOceanFrame
{
	bool bValid = false;
	float Time = 0.f;
	TArray<FOceanCascadeFrame> Cascades;

	/** World-space foam around the camera (see FOceanFoamMap), one array per mip. R = foam amount. */
	TArray<TArray<FFloat16Color>> FoamMapMips;
	/** World position (m) of the foam map's lower corner, and its width (m). */
	FVector2f FoamMapOrigin = FVector2f::ZeroVector;
	float FoamMapSize = 0.f;
};

/**
 * Tessendorf FFT ocean on the CPU.
 * Three cascades of different tile sizes, each owning a disjoint band of wavelengths.
 * Simulate() is safe to run on a worker thread (it only touches this object and the output frame).
 */
class MAKAR_API FOceanSimulation
{
public:
	static constexpr int32 NumCascades = 3;

	void Initialize(const FOceanSettings& InSettings);
	void Simulate(float Time, float DeltaTime, FOceanFrame& Out);

	int32 GetResolution() const { return N; }
	int32 GetNumMips() const { return LogN + 1; }

private:
	struct FComplex
	{
		float Re = 0.f;
		float Im = 0.f;
	};

	struct FCascade
	{
		float PatchSize = 0.f;
		TArray<FComplex> H0;          // h0(k)
		TArray<FComplex> H0MinusConj; // conj(h0(-k))
		TArray<float> Omega;          // angular frequency of each k
		TArray<FVector2f> K;          // wave vector, rad/m
		TArray<float> Foam;           // persistent foam, decays over time
	};

	void InitializeCascade(FCascade& Cascade, float PatchSize, float KMin, float KMax, int32 Seed) const;
	void SimulateCascade(FCascade& Cascade, float Time, float DeltaTime, FOceanCascadeFrame& Out);

	void Dispersion(float K, float& OutOmega, float& OutDOmegaDK) const;
	float JonswapSpectrum(float Omega) const;
	float DirectionalSpreading(float Omega, float Theta) const;

	void InverseFFT1D(FComplex* Data) const;
	void InverseFFT2D(TArray<FComplex>* Grids, int32 NumGrids) const;
	void BuildMips(const TArray<FVector4f>& Base, TArray<TArray<FFloat16Color>>& OutMips) const;

	FOceanSettings Settings;
	int32 N = 0;
	int32 LogN = 0;
	float Alpha = 0.f;     // JONSWAP energy scale
	float PeakOmega = 0.f; // JONSWAP peak frequency

	TArray<int32> BitReverse;
	TArray<FComplex> Twiddles;

	FCascade Cascades[NumCascades];

	// Scratch buffers, reused every frame.
	TArray<FComplex> Work[4];
	TArray<FVector4f> DisplacementBase;
	TArray<FVector4f> DerivativeBase;
	TArray<FVector4f> MomentBase;
};
