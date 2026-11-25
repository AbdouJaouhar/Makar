#pragma once

#include "CoreMinimal.h"
#include "Ocean/OceanFlora.h"
#include "RopeSolver.h"
#include "SailCloth.h"

/**
 * Geometry helpers of the ship generator: everything in metres in ship space (X forward, Y starboard, Z up), stored in
 * centimetres. Triangles are wound from the vertices' normals (front faces towards them), so callers give normals and
 * never care about the order of corners.
 */
namespace ShipMesh
{
	/** Material slots of a ship mesh (Scripts/create_ship_materials.py). */
	enum ESection : int32
	{
		Wood = 0,  // vertex colour = albedo; UV: U along the grain (m), V across
		Sail = 1,  // two-sided canvas, U across the cloths (m), V down (m)
		Rope = 2,  // hemp, U around, V along (m)
		Metal = 3, // iron (alpha 0) or gilding (alpha 1: metallic)
		Gloss = 4, // glass, varnish
		NumSections = 5,
	};

	/** An orthonormal frame: X, Y, Z axes at O. */
	struct FShipFrame
	{
		FVector3f O = FVector3f::ZeroVector;
		FVector3f X = FVector3f::ForwardVector;
		FVector3f Y = FVector3f::RightVector;
		FVector3f Z = FVector3f::UpVector;

		FVector3f At(float A, float B, float C) const { return O + X * A + Y * B + Z * C; }
		/** X along Direction, Z as close to Up as can be. */
		static FShipFrame Along(const FVector3f& Origin, const FVector3f& Direction, const FVector3f& Up = FVector3f::UpVector);
	};

	class FBuilder
	{
	public:
		explicit FBuilder(FOceanMeshData& InData) : Data(InData) {}

		FOceanMeshData& Data;
		/** If set, the ropes and spars (tubes of the Rope and Wood sections) are recorded here: the sails rest on them. */
		TArray<FShipCapsule>* Colliders = nullptr;
		/** With Blocks set: the turned pieces (Lathe) recorded as blocks too. */
		bool bLatheBlocks = false;
		/** If set, Line() records the ropes here instead of drawing them (ShipRopes::Lay lays and draws them). */
		TArray<FShipRope>* Ropes = nullptr;
		/** If set, the large wooden boxes (tops, caps, crosstrees) are recorded here: the ropes go round them. */
		TArray<FShipBlock>* Blocks = nullptr;
		/** If set, every box and turned piece, however small (pulley blocks, cleats, hoops), for fitting the sails. */
		TArray<FShipBlock>* Pieces = nullptr;

		int32 Vertex(const FVector3f& Position, const FVector3f& Normal, const FVector2f& UV, const FLinearColor& Color);
		/** Wound so that its front faces the average of its vertices' normals. */
		void Triangle(int32 Section, int32 A, int32 B, int32 C);
		void Quad(int32 Section, int32 A, int32 B, int32 C, int32 D);
		/** A flat quad (corners in order around it) with its own vertices; UV in metres from the first corner. */
		void FlatQuad(int32 Section, const FVector3f& A, const FVector3f& B, const FVector3f& C, const FVector3f& D,
			const FLinearColor& Color, const FVector3f& Facing);
		/** A box: HalfSize along the frame's axes; the grain along X. */
		void Box(int32 Section, const FShipFrame& Frame, const FVector3f& HalfSize, const FLinearColor& Color);
		/**
		 * A tube along a polyline with a radius per point: rings square to the mean of the neighbouring segments, a frame
		 * carried along (no twist), caps at both ends if asked. Ropes: Rope section, few sides.
		 */
		void Tube(int32 Section, TConstArrayView<FVector3f> Points, TConstArrayView<float> Radii, int32 Sides,
			const FLinearColor& Color, bool bCaps = false);
		void Tube(int32 Section, const FVector3f& A, const FVector3f& B, float RadiusA, float RadiusB, int32 Sides,
			const FLinearColor& Color, bool bCaps = false);
		/** A rope from A to B sagging by Sag metres at its middle (a parabola, close to the catenary of a taut line). */
		void Line(const FVector3f& A, const FVector3f& B, float Radius, float Sag = 0.f, const FLinearColor& Color = RopeColor);
		/** A solid of revolution around Axis from Origin: Profile = (distance along the axis, radius), in order. */
		void Lathe(int32 Section, const FVector3f& Origin, const FVector3f& Axis, TConstArrayView<FVector2f> Profile, int32 Sides,
			const FLinearColor& Color);
		/**
		 * A grid surface Position(U, V), U and V in 0-1, (NU + 1) x (NV + 1) vertices, normals from the grid, UV in metres
		 * along its edges. bFlip: normals the other way.
		 */
		void Grid(int32 Section, int32 NU, int32 NV, TFunctionRef<FVector3f(float, float)> Position, const FLinearColor& Color,
			bool bFlip = false, TFunctionRef<FLinearColor(float, float)>* ColorAt = nullptr);
		/**
		 * A beam swept along a path: a rectangle Width (across, along the frame's Y) x Height (along its Z) square to
		 * the path, Up giving its roll. Grain along the path. Ends capped.
		 */
		void Beam(int32 Section, TConstArrayView<FVector3f> Path, const FVector3f& Up, float Width, float Height, const FLinearColor& Color);

		static const FLinearColor RopeColor;
		static const FLinearColor TarredRopeColor;
		static const FLinearColor IronColor;

	private:
		void CapRing(int32 Section, int32 First, int32 Sides, const FVector3f& Center, const FVector3f& Facing, const FLinearColor& Color,
			TConstArrayView<FVector3f> Ring);
	};

	/** Appends From's vertices and triangles to To (parts built in parallel, merged in order). */
	void Append(FOceanMeshData& To, const FOceanMeshData& From);

	/** Deterministic hash noise in 0-1. */
	float Hash(int32 A, int32 B = 0, int32 C = 0);
	/** A colour scaled by a factor around 1 (Amount: its spread), from a hash. */
	FLinearColor Jitter(const FLinearColor& Color, float Amount, int32 A, int32 B = 0, int32 C = 0);
}
