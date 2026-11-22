#include "Ocean/OceanShoreData.h"

FVector4f FOceanShoreData::Sample(const FVector2f& PositionM) const
{
	if (Resolution <= 0 || SizeM <= 0.f || Texels.Num() != Resolution * Resolution)
	{
		return FVector4f(DeepWaterM, DeepWaterM, 0.f, 0.f);
	}

	const FVector2f Uv = (PositionM - OriginM) / SizeM;
	if (Uv.X < 0.f || Uv.X > 1.f || Uv.Y < 0.f || Uv.Y > 1.f)
	{
		return FVector4f(DeepWaterM, DeepWaterM, 0.f, 0.f);
	}

	const float U = FMath::Clamp(Uv.X * Resolution - 0.5f, 0.f, float(Resolution - 1));
	const float V = FMath::Clamp(Uv.Y * Resolution - 0.5f, 0.f, float(Resolution - 1));
	const int32 X0 = int32(U);
	const int32 Y0 = int32(V);
	const int32 X1 = FMath::Min(X0 + 1, Resolution - 1);
	const int32 Y1 = FMath::Min(Y0 + 1, Resolution - 1);
	const float FracU = U - X0;
	const float FracV = V - Y0;

	const FVector4f Bottom = FMath::Lerp(Texels[Y0 * Resolution + X0], Texels[Y0 * Resolution + X1], FracU);
	const FVector4f Top = FMath::Lerp(Texels[Y1 * Resolution + X0], Texels[Y1 * Resolution + X1], FracU);
	return FMath::Lerp(Bottom, Top, FracV);
}
