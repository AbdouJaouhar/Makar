#include "Ocean/OceanSandDeformation.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Ocean/OceanSubsystem.h"
#include "TextureResource.h"
#if WITH_EDITOR
#include "LevelEditorViewport.h"
#endif

namespace
{
	/** Coarse grid of the water depth over the sand, for the swash: one ocean query per node. */
	constexpr float WashGridM = 0.5f;
	/** Below this, an offset is no relief at all. */
	constexpr float NegligibleM = 2e-4f;

	/** Grows Into to hold Rect (FIntRect::Union works in place, an empty rectangle stays empty). */
	void Grow(FIntRect& Into, const FIntRect& Rect)
	{
		if (Into.Area() <= 0)
		{
			Into = Rect;
		}
		else
		{
			Into.Union(Rect);
		}
	}
}

UOceanSandDeformation::UOceanSandDeformation()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.f; // the slopes settle visibly: every frame
}

void UOceanSandDeformation::EnsureAllocated()
{
	const int32 N = Resolution;
	if (Height.Num() != N * N)
	{
		Height.Init(0.f, N * N);
		Disturb.Init(0.f, N * N);
		Active = FIntRect();
		Dirty = FIntRect();
		Texture = nullptr;
	}
	if (!Texture)
	{
		Texture = UTexture2D::CreateTransient(N, N, PF_FloatRGBA, TEXT("SandDeformation"));
		Texture->SRGB = false;
		Texture->Filter = TF_Bilinear;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->UpdateResource();
		bNeedsFullUpload = true;
		OnWindowChanged.Broadcast();
	}
}

void UOceanSandDeformation::SetFocus(const FVector& WorldLocation)
{
	EnsureAllocated();
	Recentre(FVector2f(float(WorldLocation.X * 0.01), float(WorldLocation.Y * 0.01)));
}

void UOceanSandDeformation::Recentre(const FVector2f& CentreM)
{
	const float Texel = TexelM();
	const FVector2f Wanted = CentreM - FVector2f(0.5f * WindowSize);
	const FIntPoint Shift(FMath::RoundToInt((Wanted.X - OriginM.X) / Texel), FMath::RoundToInt((Wanted.Y - OriginM.Y) / Texel));
	if (Shift == FIntPoint::ZeroValue)
	{
		return;
	}
	const int32 N = Resolution;
	const bool bAnything = Active.Area() > 0 || Dirty.Area() > 0
		|| Height.ContainsByPredicate([](float H) { return FMath::Abs(H) > NegligibleM; });
	if (bAnything)
	{
		// The relief keeps its place in the world: texel (X, Y) of the new window is (X + Shift) of the old one.
		TArray<float> NewHeight, NewDisturb;
		NewHeight.Init(0.f, N * N);
		NewDisturb.Init(0.f, N * N);
		for (int32 Y = 0; Y < N; ++Y)
		{
			const int32 SY = Y + Shift.Y;
			if (SY < 0 || SY >= N)
			{
				continue;
			}
			for (int32 X = 0; X < N; ++X)
			{
				const int32 SX = X + Shift.X;
				if (SX >= 0 && SX < N)
				{
					NewHeight[Y * N + X] = Height[SY * N + SX];
					NewDisturb[Y * N + X] = Disturb[SY * N + SX];
				}
			}
		}
		Height = MoveTemp(NewHeight);
		Disturb = MoveTemp(NewDisturb);
		Active = FIntRect(0, 0, N, N); // let it settle and wash over the whole window once
	}
	OriginM += FVector2f(Shift) * Texel;
	bNeedsFullUpload = true;
	OnWindowChanged.Broadcast();
}

void UOceanSandDeformation::Touch(int32 X0, int32 Y0, int32 X1, int32 Y1)
{
	const FIntRect Rect(FMath::Clamp(X0, 0, Resolution), FMath::Clamp(Y0, 0, Resolution), FMath::Clamp(X1, 0, Resolution),
		FMath::Clamp(Y1, 0, Resolution));
	if (Rect.Area() <= 0)
	{
		return;
	}
	Grow(Active, Rect);
	Grow(Dirty, Rect);
}

void UOceanSandDeformation::StampFootprint(const FVector& WorldLocation, float YawDegrees, bool bLeftFoot, float Weight)
{
	EnsureAllocated();
	const FVector2f P(float(WorldLocation.X * 0.01), float(WorldLocation.Y * 0.01));
	const float Ground = GroundHeight ? GroundHeight(P) : 1.f;
	// Firm wet sand near the water takes a sharp, shallow print and barely bulges; soft dry sand above the swash sinks
	// a few centimetres and is pushed out into a rim, mostly at the toe where the foot pushes off.
	const float Damp = 1.f - FMath::SmoothStep(0.25f, 0.9f, Ground);
	const float Depth = Weight * FMath::Lerp(0.03f, 0.007f, Damp);
	const float RimShare = FMath::Lerp(0.65f, 0.15f, Damp);

	const float Yaw = FMath::DegreesToRadians(YawDegrees);
	const FVector2f Forward(FMath::Cos(Yaw), FMath::Sin(Yaw));
	const FVector2f Side(-Forward.Y, Forward.X);
	const float Inner = bLeftFoot ? -1.f : 1.f; // the big toe's side
	// Heel, ball and toes: ellipses (centre along / across the foot, radii), m.
	struct FBlob { float U, V, RU, RV; };
	const FBlob Blobs[] = { { -0.075f, 0.f, 0.055f, 0.042f }, { 0.05f, 0.008f * Inner, 0.07f, 0.05f }, { 0.115f, 0.012f * Inner, 0.03f, 0.046f } };
	auto Shape = [&](const FVector2f& Q)
	{
		const FVector2f D = Q - P;
		const float U = FVector2f::DotProduct(D, Forward);
		const float V = FVector2f::DotProduct(D, Side);
		float S = 10.f;
		for (const FBlob& B : Blobs)
		{
			S = FMath::Min(S, FMath::Sqrt(FMath::Square((U - B.U) / B.RU) + FMath::Square((V - B.V) / B.RV)));
		}
		return TPair<float, float>(S, U);
	};

	const float Texel = TexelM();
	const FVector2f T0 = ToTexel(P - FVector2f(0.3f));
	const FVector2f T1 = ToTexel(P + FVector2f(0.3f));
	const int32 X0 = FMath::Max(0, FMath::FloorToInt(T0.X)), Y0 = FMath::Max(0, FMath::FloorToInt(T0.Y));
	const int32 X1 = FMath::Min(Resolution, FMath::CeilToInt(T1.X)), Y1 = FMath::Min(Resolution, FMath::CeilToInt(T1.Y));
	if (X0 >= X1 || Y0 >= Y1)
	{
		return;
	}
	// The print: a flat-bottomed dip with sloping sides. Then the rim gets the displaced volume (its share).
	float Removed = 0.f, RimWeights = 0.f;
	for (int32 Y = Y0; Y < Y1; ++Y)
	{
		for (int32 X = X0; X < X1; ++X)
		{
			const FVector2f Q = OriginM + (FVector2f(X, Y) + 0.5f) * Texel;
			const TPair<float, float> SU = Shape(Q);
			const int32 I = Y * Resolution + X;
			const float Profile = 1.f - FMath::SmoothStep(0.75f, 1.05f, SU.Key);
			if (Profile > 0.f)
			{
				// A foot sinks to a depth, not by a depth: trampling the same spot does not dig a pit.
				const float Target = FMath::Min(Height[I], -Depth * Profile + 0.5f * Height[I]);
				Removed += Height[I] - Target;
				Height[I] = Target;
				Disturb[I] = FMath::Max(Disturb[I], 0.8f * Profile);
			}
			const float Ring = FMath::Exp(-FMath::Square((SU.Key - 1.3f) / 0.2f)) * (SU.Value > 0.f ? 1.6f : 1.f);
			RimWeights += SU.Key > 1.f ? Ring : 0.f;
		}
	}
	if (RimWeights > 0.f)
	{
		const float PerWeight = Removed * RimShare / RimWeights;
		for (int32 Y = Y0; Y < Y1; ++Y)
		{
			for (int32 X = X0; X < X1; ++X)
			{
				const FVector2f Q = OriginM + (FVector2f(X, Y) + 0.5f) * Texel;
				const TPair<float, float> SU = Shape(Q);
				if (SU.Key > 1.f)
				{
					const float Ring = FMath::Exp(-FMath::Square((SU.Key - 1.3f) / 0.2f)) * (SU.Value > 0.f ? 1.6f : 1.f);
					const int32 I = Y * Resolution + X;
					Height[I] += PerWeight * Ring;
					Disturb[I] = FMath::Max(Disturb[I], 0.5f * FMath::Min(Ring, 1.f));
				}
			}
		}
	}
	Touch(X0, Y0, X1, Y1);
}

float UOceanSandDeformation::Dig(const FVector& WorldLocation, float RadiusM, float DepthM, const FVector& SpoilDirection)
{
	EnsureAllocated();
	const FVector2f P(float(WorldLocation.X * 0.01), float(WorldLocation.Y * 0.01));
	FVector2f Dir(float(SpoilDirection.X), float(SpoilDirection.Y));
	Dir = Dir.SizeSquared() > 1e-6f ? Dir.GetSafeNormal() : FVector2f(1.f, 0.f);
	const float R = FMath::Max(RadiusM, 0.05f);
	const FVector2f Heap = P + Dir * (1.7f * R);
	const float RH = 1.1f * R;
	const float Texel = TexelM();
	const float Area = Texel * Texel;

	auto Bounds = [&](const FVector2f& C, float Radius, int32& X0, int32& Y0, int32& X1, int32& Y1)
	{
		const FVector2f A = ToTexel(C - FVector2f(Radius)), B = ToTexel(C + FVector2f(Radius));
		X0 = FMath::Max(0, FMath::FloorToInt(A.X)); Y0 = FMath::Max(0, FMath::FloorToInt(A.Y));
		X1 = FMath::Min(Resolution, FMath::CeilToInt(B.X)); Y1 = FMath::Min(Resolution, FMath::CeilToInt(B.Y));
	};
	// The bowl: a paraboloid scooped out of whatever is there.
	int32 X0, Y0, X1, Y1;
	Bounds(P, R, X0, Y0, X1, Y1);
	float Volume = 0.f;
	for (int32 Y = Y0; Y < Y1; ++Y)
	{
		for (int32 X = X0; X < X1; ++X)
		{
			const FVector2f Q = OriginM + (FVector2f(X, Y) + 0.5f) * Texel;
			const float Q2 = FVector2f::DistSquared(Q, P) / (R * R);
			if (Q2 < 1.f)
			{
				const int32 I = Y * Resolution + X;
				const float Cut = DepthM * (1.f - Q2);
				Height[I] -= Cut;
				Disturb[I] = 1.f;
				Volume += Cut * Area;
			}
		}
	}
	Touch(X0, Y0, X1, Y1);
	// The spoil: a mound of the same volume beside it.
	int32 HX0, HY0, HX1, HY1;
	Bounds(Heap, RH, HX0, HY0, HX1, HY1);
	float Weights = 0.f;
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		for (int32 Y = HY0; Y < HY1; ++Y)
		{
			for (int32 X = HX0; X < HX1; ++X)
			{
				const FVector2f Q = OriginM + (FVector2f(X, Y) + 0.5f) * Texel;
				const float W = 1.f - FVector2f::DistSquared(Q, Heap) / (RH * RH);
				if (W <= 0.f)
				{
					continue;
				}
				if (Pass == 0)
				{
					Weights += W;
				}
				else
				{
					const int32 I = Y * Resolution + X;
					Height[I] += Volume / (Area * Weights) * W;
					Disturb[I] = 1.f;
				}
			}
		}
		if (Weights <= 0.f)
		{
			break;
		}
	}
	Touch(HX0, HY0, HX1, HY1);
	return Volume;
}

float UOceanSandDeformation::GetHeightOffsetAt(const FVector& WorldLocation) const
{
	if (Height.Num() != Resolution * Resolution)
	{
		return 0.f;
	}
	const FVector2f T = ToTexel(FVector2f(float(WorldLocation.X * 0.01), float(WorldLocation.Y * 0.01))) - 0.5f;
	const int32 X = FMath::FloorToInt(T.X), Y = FMath::FloorToInt(T.Y);
	if (X < 0 || Y < 0 || X + 1 >= Resolution || Y + 1 >= Resolution)
	{
		return 0.f;
	}
	const float FX = T.X - X, FY = T.Y - Y;
	const int32 I = Y * Resolution + X;
	const float H = FMath::Lerp(FMath::Lerp(Height[I], Height[I + 1], FX), FMath::Lerp(Height[I + Resolution], Height[I + Resolution + 1], FX), FY);
	return H * 100.f;
}

bool UOceanSandDeformation::SettleSlopes()
{
	if (Active.Area() <= 0)
	{
		return false;
	}
	// Sand slides from a texel to its neighbour while the slope between them is steeper than the angle of repose:
	// half the excess moves (each pair levels to the limit), relaxed over a few passes per frame so that walls slump
	// visibly rather than snapping. Sand deeper than ~5 cm below the old surface is damp and holds steeper walls.
	const float Texel = TexelM();
	const float DryLimit = FMath::Tan(FMath::DegreesToRadians(DryReposeAngle)) * Texel;
	const float DampLimit = FMath::Tan(FMath::DegreesToRadians(DampReposeAngle)) * Texel;
	const int32 N = Resolution;
	const FIntRect R(FMath::Max(Active.Min.X - 1, 0), FMath::Max(Active.Min.Y - 1, 0), FMath::Min(Active.Max.X + 1, N), FMath::Min(Active.Max.Y + 1, N));
	FIntRect Moved;
	bool bMoved = false;
	for (int32 Pass = 0; Pass < 6; ++Pass)
	{
		for (int32 Y = R.Min.Y; Y < R.Max.Y; ++Y)
		{
			for (int32 X = R.Min.X; X < R.Max.X; ++X)
			{
				const int32 I = Y * N + X;
				for (int32 Neighbour = 0; Neighbour < 2; ++Neighbour)
				{
					const int32 NX = X + (Neighbour == 0), NY = Y + (Neighbour == 1);
					if (NX >= N || NY >= N)
					{
						continue;
					}
					const int32 J = NY * N + NX;
					const float Difference = Height[I] - Height[J];
					const float Limit = FMath::Min(Height[I], Height[J]) < -0.05f ? DampLimit : DryLimit;
					const float Excess = FMath::Abs(Difference) - Limit;
					if (Excess <= 1e-5f)
					{
						continue;
					}
					const float Move = 0.35f * Excess * FMath::Sign(Difference);
					Height[I] -= Move;
					Height[J] += Move;
					Disturb[I] = FMath::Max(Disturb[I], 0.6f);
					Disturb[J] = FMath::Max(Disturb[J], 0.6f);
					const FIntRect Pair(FMath::Min(X, NX), FMath::Min(Y, NY), FMath::Max(X, NX) + 1, FMath::Max(Y, NY) + 1);
					Grow(Moved, Pair);
					bMoved = true;
				}
			}
		}
	}
	if (bMoved)
	{
		// What moved may make its neighbours slide next frame.
		Active = FIntRect(FMath::Max(Moved.Min.X - 2, 0), FMath::Max(Moved.Min.Y - 2, 0), FMath::Min(Moved.Max.X + 2, N), FMath::Min(Moved.Max.Y + 2, N));
		Grow(Dirty, Moved);
	}
	return bMoved;
}

void UOceanSandDeformation::WashUnderSwash(float DeltaTime)
{
	const UWorld* World = GetWorld();
	const UOceanSubsystem* Ocean = World ? World->GetSubsystem<UOceanSubsystem>() : nullptr;
	if (!Ocean || !GroundHeight)
	{
		return;
	}
	const int32 N = Resolution;
	// The part of the window that holds any relief.
	FIntRect Used;
	bool bAny = false;
	for (int32 Y = 0; Y < N; ++Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			const int32 I = Y * N + X;
			if (FMath::Abs(Height[I]) > NegligibleM || Disturb[I] > 0.01f)
			{
				const FIntRect Cell(X, Y, X + 1, Y + 1);
				Grow(Used, Cell);
				bAny = true;
			}
		}
	}
	if (!bAny)
	{
		return;
	}
	// Water thickness over the undisturbed ground on a coarse grid (one ocean query per node), interpolated.
	const float Texel = TexelM();
	const FVector2f A = OriginM + FVector2f(Used.Min) * Texel;
	const FVector2f B = OriginM + FVector2f(Used.Max) * Texel;
	const int32 GX = FMath::CeilToInt((B.X - A.X) / WashGridM) + 1;
	const int32 GY = FMath::CeilToInt((B.Y - A.Y) / WashGridM) + 1;
	TArray<float> Water;
	Water.SetNumUninitialized(GX * GY);
	bool bCovered = false;
	for (int32 J = 0; J < GY; ++J)
	{
		for (int32 I = 0; I < GX; ++I)
		{
			const FVector2f Q = A + FVector2f(I, J) * WashGridM;
			float WaterZ = 0.f;
			Ocean->GetWaterHeightAt(FVector(Q.X * 100.f, Q.Y * 100.f, 0.f), WaterZ);
			Water[J * GX + I] = WaterZ * 0.01f - GroundHeight(Q);
			bCovered |= Water[J * GX + I] > -0.1f;
		}
	}
	if (!bCovered)
	{
		return;
	}
	// Under the swash the relief relaxes towards the undisturbed beach and the turned-over sand is washed back to the
	// colour of the rest: covered for SwashSmoothingTime, most of a footprint is gone (a few waves).
	const float Keep = FMath::Exp(-DeltaTime / FMath::Max(SwashSmoothingTime, 0.1f));
	for (int32 Y = Used.Min.Y; Y < Used.Max.Y; ++Y)
	{
		for (int32 X = Used.Min.X; X < Used.Max.X; ++X)
		{
			const FVector2f G = ((OriginM + (FVector2f(X, Y) + 0.5f) * Texel) - A) / WashGridM;
			const int32 I0 = FMath::Clamp(FMath::FloorToInt(G.X), 0, GX - 2), J0 = FMath::Clamp(FMath::FloorToInt(G.Y), 0, GY - 2);
			const float FX = FMath::Clamp(G.X - I0, 0.f, 1.f), FY = FMath::Clamp(G.Y - J0, 0.f, 1.f);
			const float Over = FMath::Lerp(FMath::Lerp(Water[J0 * GX + I0], Water[J0 * GX + I0 + 1], FX),
				FMath::Lerp(Water[(J0 + 1) * GX + I0], Water[(J0 + 1) * GX + I0 + 1], FX), FY);
			const int32 K = Y * N + X;
			if (Over - Height[K] > 0.003f) // a few millimetres of water over this texel
			{
				Height[K] *= Keep;
				Disturb[K] *= Keep * Keep;
				if (FMath::Abs(Height[K]) < NegligibleM)
				{
					Height[K] = 0.f;
				}
			}
		}
	}
	Grow(Dirty, Used);
	Grow(Active, Used); // washed walls may slide
}

void UOceanSandDeformation::Upload()
{
	if (!Texture || !Texture->GetResource())
	{
		return;
	}
	const int32 N = Resolution;
	const FIntRect R = bNeedsFullUpload ? FIntRect(0, 0, N, N) : Dirty;
	if (bNeedsFullUpload)
	{
		UE_LOG(LogTemp, Verbose, TEXT("OceanSandDeformation: full upload, window at (%.1f, %.1f) m"), OriginM.X, OriginM.Y);
	}
	if (R.Area() <= 0)
	{
		return;
	}
	const int32 W = R.Width(), H = R.Height();
	FFloat16Color* Data = new FFloat16Color[W * H];
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			const int32 I = (R.Min.Y + Y) * N + R.Min.X + X;
			Data[Y * W + X] = FFloat16Color(FLinearColor(Height[I], Disturb[I], 0.f, 0.f));
		}
	}
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(R.Min.X, R.Min.Y, 0, 0, W, H);
	Texture->UpdateTextureRegions(0, 1, Region, W * sizeof(FFloat16Color), sizeof(FFloat16Color), reinterpret_cast<uint8*>(Data),
		[](uint8* Source, const FUpdateTextureRegion2D* Regions)
		{
			delete[] reinterpret_cast<FFloat16Color*>(Source);
			delete Regions;
		});
	Dirty = FIntRect();
	bNeedsFullUpload = false;
}

void UOceanSandDeformation::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	EnsureAllocated();

	// Follow the viewer: the player's camera in game, the level viewport's camera in the editor.
	bool bHasViewer = false;
	FVector Viewer;
	if (const UWorld* World = GetWorld(); World && World->IsGameWorld())
	{
		if (const APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(this, 0))
		{
			Viewer = CameraManager->GetCameraLocation();
			bHasViewer = true;
		}
	}
#if WITH_EDITOR
	else if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->IsPerspective())
	{
		Viewer = GCurrentLevelEditingViewportClient->GetViewLocation();
		bHasViewer = true;
	}
#endif
	if (bHasViewer)
	{
		const FVector2f V(float(Viewer.X * 0.01), float(Viewer.Y * 0.01));
		const FVector2f Centre = OriginM + FVector2f(0.5f * WindowSize);
		if (FMath::Max(FMath::Abs(V.X - Centre.X), FMath::Abs(V.Y - Centre.Y)) > 0.25f * WindowSize)
		{
			Recentre(V);
		}
	}

	if (!SettleSlopes())
	{
		Active = FIntRect();
	}
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (Now - LastWashTime >= 0.1)
	{
		WashUnderSwash(float(FMath::Min(Now - LastWashTime, 0.5)));
		LastWashTime = Now;
	}
	Upload();
}
