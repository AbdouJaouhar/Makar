#include "ShipMeshBuilder.h"

namespace ShipMesh
{
	// Tarred standing rigging is near black; running rigging is untarred hemp, a greyish straw.
	const FLinearColor FBuilder::RopeColor = FLinearColor(0.19f, 0.15f, 0.1f);
	const FLinearColor FBuilder::TarredRopeColor = FLinearColor(0.035f, 0.03f, 0.025f);
	const FLinearColor FBuilder::IronColor = FLinearColor(0.035f, 0.033f, 0.03f, 0.f);

	FShipFrame FShipFrame::Along(const FVector3f& Origin, const FVector3f& Direction, const FVector3f& Up)
	{
		FShipFrame Frame;
		Frame.O = Origin;
		Frame.X = Direction.GetSafeNormal();
		FVector3f Side = FVector3f::CrossProduct(Up, Frame.X);
		if (Side.SizeSquared() < 1e-6f)
		{
			Side = FVector3f::CrossProduct(FVector3f::ForwardVector, Frame.X);
			if (Side.SizeSquared() < 1e-6f)
			{
				Side = FVector3f::RightVector;
			}
		}
		Frame.Y = Side.GetSafeNormal();
		Frame.Z = FVector3f::CrossProduct(Frame.X, Frame.Y);
		return Frame;
	}

	void Append(FOceanMeshData& To, const FOceanMeshData& From)
	{
		const int32 Offset = To.Vertices.Num();
		To.Vertices.Append(From.Vertices);
		for (int32 Section = 0; Section < From.Sections.Num() && Section < To.Sections.Num(); ++Section)
		{
			TArray<int32>& Indices = To.Sections[Section];
			Indices.Reserve(Indices.Num() + From.Sections[Section].Num());
			for (int32 Index : From.Sections[Section])
			{
				Indices.Add(Index + Offset);
			}
		}
	}

	float Hash(int32 A, int32 B, int32 C)
	{
		uint32 H = uint32(A) * 0x8da6b343u ^ uint32(B) * 0xd8163841u ^ uint32(C) * 0xcb1ab31fu;
		H ^= H >> 13;
		H *= 0x5bd1e995u;
		H ^= H >> 15;
		return float(H & 0xffffffu) / float(0x1000000);
	}

	FLinearColor Jitter(const FLinearColor& Color, float Amount, int32 A, int32 B, int32 C)
	{
		const float Light = 1.f + (Hash(A, B, C) * 2.f - 1.f) * Amount;
		const float Warm = 1.f + (Hash(A + 7, B, C) * 2.f - 1.f) * Amount * 0.3f;
		return FLinearColor(Color.R * Light * Warm, Color.G * Light, Color.B * Light / Warm, Color.A);
	}

	int32 FBuilder::Vertex(const FVector3f& Position, const FVector3f& Normal, const FVector2f& UV, const FLinearColor& Color)
	{
		return Data.AddVertex(Position * 100.f, Normal.GetSafeNormal(), UV, Color);
	}

	void FBuilder::Triangle(int32 Section, int32 A, int32 B, int32 C)
	{
		const FOceanMeshData::FVertex& VA = Data.Vertices[A];
		const FOceanMeshData::FVertex& VB = Data.Vertices[B];
		const FOceanMeshData::FVertex& VC = Data.Vertices[C];
		const FVector3f Facing = VA.Normal + VB.Normal + VC.Normal;
		// The engine's front faces: cross(B - A, C - A) points away from the normal (left-handed, clockwise).
		const FVector3f Cross = FVector3f::CrossProduct(VB.Position - VA.Position, VC.Position - VA.Position);
		if (FVector3f::DotProduct(Cross, Facing) > 0.f)
		{
			Data.AddTriangle(Section, A, C, B);
		}
		else
		{
			Data.AddTriangle(Section, A, B, C);
		}
	}

	void FBuilder::Quad(int32 Section, int32 A, int32 B, int32 C, int32 D)
	{
		Triangle(Section, A, B, C);
		Triangle(Section, A, C, D);
	}

	void FBuilder::FlatQuad(int32 Section, const FVector3f& A, const FVector3f& B, const FVector3f& C, const FVector3f& D,
		const FLinearColor& Color, const FVector3f& Facing)
	{
		FVector3f Normal = FVector3f::CrossProduct(C - A, D - B).GetSafeNormal();
		if (FVector3f::DotProduct(Normal, Facing) < 0.f)
		{
			Normal = -Normal;
		}
		const FVector3f U = (B - A).GetSafeNormal();
		auto UV = [&](const FVector3f& P) { return FVector2f(FVector3f::DotProduct(P - A, U), FVector3f::DotProduct(P - A, FVector3f::CrossProduct(Normal, U))); };
		const int32 I0 = Vertex(A, Normal, UV(A), Color);
		const int32 I1 = Vertex(B, Normal, UV(B), Color);
		const int32 I2 = Vertex(C, Normal, UV(C), Color);
		const int32 I3 = Vertex(D, Normal, UV(D), Color);
		Quad(Section, I0, I1, I2, I3);
	}

	void FBuilder::Box(int32 Section, const FShipFrame& F, const FVector3f& H, const FLinearColor& Color)
	{
		if (Blocks && Section == Wood && H.GetMax() >= 0.12f)
		{
			Blocks->Add({ F.O, F.X, F.Y, F.Z, H });
		}
		if (Pieces && (Section == Wood || Section == Metal))
		{
			Pieces->Add({ F.O, F.X, F.Y, F.Z, H });
		}
		auto P = [&](float SX, float SY, float SZ) { return F.At(SX * H.X, SY * H.Y, SZ * H.Z); };
		// +X / -X ends, +Y / -Y sides, +Z top, -Z bottom.
		FlatQuad(Section, P(1, -1, -1), P(1, 1, -1), P(1, 1, 1), P(1, -1, 1), Color, F.X);
		FlatQuad(Section, P(-1, -1, -1), P(-1, 1, -1), P(-1, 1, 1), P(-1, -1, 1), Color, -F.X);
		FlatQuad(Section, P(-1, 1, -1), P(1, 1, -1), P(1, 1, 1), P(-1, 1, 1), Color, F.Y);
		FlatQuad(Section, P(-1, -1, -1), P(1, -1, -1), P(1, -1, 1), P(-1, -1, 1), Color, -F.Y);
		FlatQuad(Section, P(-1, -1, 1), P(1, -1, 1), P(1, 1, 1), P(-1, 1, 1), Color, F.Z);
		FlatQuad(Section, P(-1, -1, -1), P(1, -1, -1), P(1, 1, -1), P(-1, 1, -1), Color, -F.Z);
	}

	void FBuilder::CapRing(int32 Section, int32 First, int32 Sides, const FVector3f& Center, const FVector3f& Facing,
		const FLinearColor& Color, TConstArrayView<FVector3f> Ring)
	{
		const int32 Middle = Vertex(Center, Facing, FVector2f(0.5f, 0.5f), Color);
		TArray<int32> Rim;
		for (int32 Side = 0; Side < Sides; ++Side)
		{
			Rim.Add(Vertex(Ring[Side], Facing, FVector2f(float(Side) / Sides, 0.f), Color));
		}
		for (int32 Side = 0; Side < Sides; ++Side)
		{
			Triangle(Section, Middle, Rim[Side], Rim[(Side + 1) % Sides]);
		}
	}

	void FBuilder::Tube(int32 Section, TConstArrayView<FVector3f> Points, TConstArrayView<float> Radii, int32 Sides,
		const FLinearColor& Color, bool bCaps)
	{
		const int32 Count = Points.Num();
		if (Count < 2)
		{
			return;
		}
		if (Colliders && (Section == Rope || Section == Wood))
		{
			for (int32 I = 0; I + 1 < Count; ++I)
			{
				Colliders->Add({ Points[I], Points[I + 1], FMath::Max(Radii[I], Radii[I + 1]), Section == Wood });
			}
		}
		// Tangents, and a frame carried along by the smallest rotation from one to the next.
		TArray<FVector3f> Tangents;
		Tangents.SetNum(Count);
		for (int32 I = 0; I < Count; ++I)
		{
			const FVector3f Prev = Points[FMath::Max(I - 1, 0)], Next = Points[FMath::Min(I + 1, Count - 1)];
			Tangents[I] = (Next - Prev).GetSafeNormal();
		}
		FVector3f Normal = FVector3f::CrossProduct(Tangents[0], FMath::Abs(Tangents[0].Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector).GetSafeNormal();
		float Along = 0.f;
		TArray<FVector3f> FirstRing, LastRing;
		const int32 First = Data.Vertices.Num();
		for (int32 I = 0; I < Count; ++I)
		{
			if (I > 0)
			{
				Along += (Points[I] - Points[I - 1]).Size();
				// Remove the tangent component: the frame turns with the curve without twisting.
				Normal = (Normal - Tangents[I] * FVector3f::DotProduct(Normal, Tangents[I])).GetSafeNormal();
			}
			const FVector3f Binormal = FVector3f::CrossProduct(Tangents[I], Normal);
			for (int32 Side = 0; Side <= Sides; ++Side)
			{
				const float Angle = 2.f * UE_PI * Side / Sides;
				const FVector3f Out = Normal * FMath::Cos(Angle) + Binormal * FMath::Sin(Angle);
				const FVector3f P = Points[I] + Out * Radii[I];
				Vertex(P, Out, FVector2f(float(Side) / Sides, Along), Color);
				if (Side < Sides)
				{
					(I == 0 ? FirstRing : LastRing).Add(P);
				}
			}
			if (I > 0 && I < Count - 1)
			{
				LastRing.Reset();
			}
		}
		const int32 Stride = Sides + 1;
		for (int32 I = 0; I + 1 < Count; ++I)
		{
			for (int32 Side = 0; Side < Sides; ++Side)
			{
				const int32 A = First + I * Stride + Side;
				Quad(Section, A, A + 1, A + Stride + 1, A + Stride);
			}
		}
		if (bCaps)
		{
			CapRing(Section, 0, Sides, Points[0], -Tangents[0], Color, FirstRing);
			CapRing(Section, 0, Sides, Points[Count - 1], Tangents[Count - 1], Color, LastRing);
		}
	}

	void FBuilder::Tube(int32 Section, const FVector3f& A, const FVector3f& B, float RadiusA, float RadiusB, int32 Sides,
		const FLinearColor& Color, bool bCaps)
	{
		const FVector3f Points[2] = { A, B };
		const float Radii[2] = { RadiusA, RadiusB };
		Tube(Section, Points, Radii, Sides, Color, bCaps);
	}

	void FBuilder::Line(const FVector3f& A, const FVector3f& B, float Radius, float Sag, const FLinearColor& Color)
	{
		if (Ropes)
		{
			Ropes->Add({ A, B, Radius, Sag, Color });
			return;
		}
		const int32 Sides = Radius > 0.025f ? 6 : 4;
		if (Sag <= 0.001f)
		{
			Tube(Rope, A, B, Radius, Radius, Sides, Color);
			return;
		}
		TArray<FVector3f> Points;
		TArray<float> Radii;
		const int32 Segments = 10;
		for (int32 I = 0; I <= Segments; ++I)
		{
			const float T = float(I) / Segments;
			Points.Add(FMath::Lerp(A, B, T) - FVector3f(0.f, 0.f, 4.f * Sag * T * (1.f - T)));
			Radii.Add(Radius);
		}
		Tube(Rope, Points, Radii, Sides, Color);
	}

	void FBuilder::Lathe(int32 Section, const FVector3f& Origin, const FVector3f& Axis, TConstArrayView<FVector2f> Profile, int32 Sides,
		const FLinearColor& Color)
	{
		const FShipFrame F = FShipFrame::Along(Origin, Axis);
		const int32 Count = Profile.Num();
		const int32 First = Data.Vertices.Num();
		// A turned piece (lantern, bell, capstan...) recorded as the box round it, when asked.
		if (((Blocks && bLatheBlocks) || Pieces) && Count > 0 && (Section == Wood || Section == Metal))
		{
			float X0 = Profile[0].X, X1 = Profile[0].X, R = 0.f;
			for (const FVector2f& P : Profile)
			{
				X0 = FMath::Min(X0, P.X);
				X1 = FMath::Max(X1, P.X);
				R = FMath::Max(R, P.Y);
			}
			const FShipBlock Piece{ F.O + F.X * (0.5f * (X0 + X1)), F.X, F.Y, F.Z, FVector3f(FMath::Max(0.5f * (X1 - X0), 0.02f), R, R) };
			if (Blocks && bLatheBlocks && R >= 0.04f)
			{
				Blocks->Add(Piece);
			}
			if (Pieces)
			{
				Pieces->Add(Piece);
			}
		}
		float Along = 0.f;
		for (int32 I = 0; I < Count; ++I)
		{
			// Normal of the profile: square to the segment around this point, in the (axis, radius) plane.
			const FVector2f Prev = Profile[FMath::Max(I - 1, 0)], Next = Profile[FMath::Min(I + 1, Count - 1)];
			const FVector2f Dir = (Next - Prev).GetSafeNormal();
			const FVector2f N2(-Dir.Y, Dir.X); // (along, out)
			if (I > 0)
			{
				Along += (Profile[I] - Profile[I - 1]).Size();
			}
			for (int32 Side = 0; Side <= Sides; ++Side)
			{
				const float Angle = 2.f * UE_PI * Side / Sides;
				const FVector3f Out = F.Y * FMath::Cos(Angle) + F.Z * FMath::Sin(Angle);
				Vertex(F.O + F.X * Profile[I].X + Out * Profile[I].Y, F.X * N2.X + Out * N2.Y, FVector2f(float(Side) / Sides, Along), Color);
			}
		}
		const int32 Stride = Sides + 1;
		for (int32 I = 0; I + 1 < Count; ++I)
		{
			for (int32 Side = 0; Side < Sides; ++Side)
			{
				const int32 A = First + I * Stride + Side;
				Quad(Section, A, A + 1, A + Stride + 1, A + Stride);
			}
		}
	}

	void FBuilder::Grid(int32 Section, int32 NU, int32 NV, TFunctionRef<FVector3f(float, float)> Position, const FLinearColor& Color,
		bool bFlip, TFunctionRef<FLinearColor(float, float)>* ColorAt)
	{
		TArray<FVector3f> P;
		P.SetNum((NU + 1) * (NV + 1));
		for (int32 J = 0; J <= NV; ++J)
		{
			for (int32 I = 0; I <= NU; ++I)
			{
				P[J * (NU + 1) + I] = Position(float(I) / NU, float(J) / NV);
			}
		}
		// UV in metres: U along the first row, V along the first column.
		TArray<float> U, V;
		U.SetNumZeroed(NU + 1);
		V.SetNumZeroed(NV + 1);
		for (int32 I = 1; I <= NU; ++I)
		{
			U[I] = U[I - 1] + (P[I] - P[I - 1]).Size();
		}
		for (int32 J = 1; J <= NV; ++J)
		{
			V[J] = V[J - 1] + (P[J * (NU + 1)] - P[(J - 1) * (NU + 1)]).Size();
		}
		const int32 First = Data.Vertices.Num();
		for (int32 J = 0; J <= NV; ++J)
		{
			for (int32 I = 0; I <= NU; ++I)
			{
				const FVector3f DU = P[J * (NU + 1) + FMath::Min(I + 1, NU)] - P[J * (NU + 1) + FMath::Max(I - 1, 0)];
				const FVector3f DV = P[FMath::Min(J + 1, NV) * (NU + 1) + I] - P[FMath::Max(J - 1, 0) * (NU + 1) + I];
				FVector3f N = FVector3f::CrossProduct(DU, DV).GetSafeNormal();
				if (bFlip)
				{
					N = -N;
				}
				const FLinearColor C = ColorAt ? (*ColorAt)(float(I) / NU, float(J) / NV) : Color;
				Vertex(P[J * (NU + 1) + I], N, FVector2f(U[I], V[J]), C);
			}
		}
		for (int32 J = 0; J < NV; ++J)
		{
			for (int32 I = 0; I < NU; ++I)
			{
				const int32 A = First + J * (NU + 1) + I;
				Quad(Section, A, A + 1, A + NU + 2, A + NU + 1);
			}
		}
	}

	void FBuilder::Beam(int32 Section, TConstArrayView<FVector3f> Path, const FVector3f& Up, float Width, float Height, const FLinearColor& Color)
	{
		const int32 Count = Path.Num();
		if (Count < 2)
		{
			return;
		}
		TArray<FShipFrame> Frames;
		for (int32 I = 0; I < Count; ++I)
		{
			const FVector3f Dir = Path[FMath::Min(I + 1, Count - 1)] - Path[FMath::Max(I - 1, 0)];
			Frames.Add(FShipFrame::Along(Path[I], Dir, Up));
		}
		// Four faces, each with its own vertices (hard edges): +Z, +Y, -Z, -Y.
		const FVector2f Corners[5] = { { -1.f, 1.f }, { 1.f, 1.f }, { 1.f, -1.f }, { -1.f, -1.f }, { -1.f, 1.f } };
		for (int32 Face = 0; Face < 4; ++Face)
		{
			const FVector2f C0 = Corners[Face], C1 = Corners[Face + 1];
			const int32 First = Data.Vertices.Num();
			float Along = 0.f;
			for (int32 I = 0; I < Count; ++I)
			{
				if (I > 0)
				{
					Along += (Path[I] - Path[I - 1]).Size();
				}
				const FShipFrame& F = Frames[I];
				const FVector3f Normal = F.Y * (C0.X + C1.X) * 0.5f + F.Z * (C0.Y + C1.Y) * 0.5f;
				Vertex(F.At(0.f, C0.X * Width * 0.5f, C0.Y * Height * 0.5f), Normal, FVector2f(Along, 0.f), Color);
				Vertex(F.At(0.f, C1.X * Width * 0.5f, C1.Y * Height * 0.5f), Normal, FVector2f(Along, Face % 2 ? Height : Width), Color);
			}
			for (int32 I = 0; I + 1 < Count; ++I)
			{
				const int32 A = First + I * 2;
				Quad(Section, A, A + 1, A + 3, A + 2);
			}
		}
		for (int32 End = 0; End < 2; ++End)
		{
			const FShipFrame& F = Frames[End ? Count - 1 : 0];
			const float W = Width * 0.5f, H = Height * 0.5f;
			FlatQuad(Section, F.At(0.f, -W, -H), F.At(0.f, W, -H), F.At(0.f, W, H), F.At(0.f, -W, H), Color, End ? F.X : -F.X);
		}
	}
}
