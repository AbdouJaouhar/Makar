#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UObject;
class UStaticMesh;

/** Geometry of a generated mesh: one triangle list per material slot. Positions in cm, Z up. */
struct MAKAR_API FOceanMeshData
{
	struct FVertex
	{
		FVector3f Position;
		FVector3f Normal;
		FVector2f UV;
		FLinearColor Color;
		/** Wind data (UV channels 1 and 2, Shaders/Flora.ush): palm fronds give metres along the rachis, metres along the
		 *  leaflet, the frond's and the leaflet's phases; zero elsewhere. */
		FVector4f Wind = FVector4f(0.f, 0.f, 0.f, 0.f);
	};

	TArray<FVertex> Vertices;
	/** Given to the vertices AddVertex adds. */
	FVector4f CurrentWind = FVector4f(0.f, 0.f, 0.f, 0.f);
	TArray<TArray<int32>> Sections;

	explicit FOceanMeshData(int32 NumSections = 1) { Sections.SetNum(NumSections); }

	int32 AddVertex(const FVector3f& Position, const FVector3f& Normal, const FVector2f& UV, const FLinearColor& Color);
	void AddTriangle(int32 Section, int32 A, int32 B, int32 C);
};

/**
 * Procedural Caribbean flora and coastal rocks, built as static meshes at load time (nothing is saved).
 *  - Coconut palm: parametric (a palm does not branch): curved ringed trunk, arching fronds as textured leaf
 *    cards, nuts.
 *  - Broadleaf tree, sea grape shrub and coastal bush: stochastic L-systems (string rewriting) read by a 3D turtle,
 *    with leaf masses of textured cards (a cluster of Infinigen leaves) at the branch tips.
 *  - Rock: an icosphere displaced by 3D fractal noise, flattened at the base, like eroded coral limestone.
 * Section 0 = bark / trunk, 1 = foliage (plants; the leaflets of palms), 2 = coconuts (palms); rocks have one section.
 */
namespace OceanFlora
{
	/** Detail 0: every leaflet modelled (near), 1: a third as many, wider and flat (middle), 2: a dozen broad strips a side (far). */
	MAKAR_API FOceanMeshData MakeCoconutPalm(int32 Seed, int32 Detail = 2);
	/**
	 * b3DLeaves: real leaves in section 2 (the near levels of detail) instead of leaf cards in section 1; LeafScale > 1
	 * gives fewer, larger leaves for a middle level.
	 */
	MAKAR_API FOceanMeshData MakeBroadleafTree(int32 Seed, bool b3DLeaves = false, float LeafScale = 1.f);
	MAKAR_API FOceanMeshData MakeTropicalAlmond(int32 Seed, bool b3DLeaves = false, float LeafScale = 1.f);
	MAKAR_API FOceanMeshData MakeSeaGrape(int32 Seed, bool b3DLeaves = false, float LeafScale = 1.f);
	MAKAR_API FOceanMeshData MakeCoastalBush(int32 Seed, bool b3DLeaves = false, float LeafScale = 1.f);
	/** Solid 3D plants: section 0 stems (M_Bark), section 1 leaves (M_Foliage). Cacti: section 0 only. */
	MAKAR_API FOceanMeshData MakeAgave(int32 Seed);
	MAKAR_API FOceanMeshData MakeTurksCap(int32 Seed);
	MAKAR_API FOceanMeshData MakeColumnCactus(int32 Seed);
	MAKAR_API FOceanMeshData MakeBromeliad(int32 Seed);
	MAKAR_API FOceanMeshData MakeFern(int32 Seed);
	MAKAR_API FOceanMeshData MakeHeliconia(int32 Seed);
	MAKAR_API FOceanMeshData MakeBeachCreeper(int32 Seed);
	/** Ground litter under the palms: a dead frond lying on the sand (M_PalmFrond), a fallen coconut (M_Bark). */
	MAKAR_API FOceanMeshData MakeFallenFrond(int32 Seed, int32 Detail = 1);
	MAKAR_API FOceanMeshData MakeCoconut(int32 Seed);
	/** Crossed grass cards (M_GrassTuft), SpanCm square, the tuft's base at BaseV of the image. */
	MAKAR_API FOceanMeshData MakeGrassTuft(int32 Seed, float SpanCm, float BaseV);
	MAKAR_API FOceanMeshData MakeRock(int32 Seed);

	/** Caribbean coastal stones, unit size (the longest semi-axis is 100 cm), flat base, vertex-coloured. */
	enum class ERockKind : uint8
	{
		Limestone,   // broken blocks of bedded coral limestone: angular faces, ledges, dissolution pits, rills
		Ironshore,   // low, black-grey, razor-jagged limestone of the spray zone
		Beachrock,   // tabular slabs of cemented beach sand, cut by joints
		Volcanic,    // rounded dark andesite boulders (Lesser Antilles)
		CoralRubble, // bleached knobbly lumps of dead coral
		Cobble,      // water-worn pebbles
	};
	MAKAR_API FOceanMeshData MakeCaribbeanRock(ERockKind Kind, int32 Seed);

	/** Builds a transient static mesh (works in the editor and in a cooked game). One material per section. */
	MAKAR_API UStaticMesh* BuildStaticMesh(UObject* Outer, const FOceanMeshData& Data, const TArray<UMaterialInterface*>& Materials,
		int32 ColorSection = -1);

	/** The same with levels of detail (Lods[0] nearest); ScreenSizes[i]: screen share below which level i takes over. */
	MAKAR_API UStaticMesh* BuildStaticMeshLods(UObject* Outer, const TArray<const FOceanMeshData*>& Lods,
		const TArray<UMaterialInterface*>& Materials, const TArray<float>& ScreenSizes, const TArray<int32>& ColorSections = {});
	/** Moves section From's triangles into section To (one material, and their vertex colours show: see ColorSections). */
	MAKAR_API void MergeSection(FOceanMeshData& Data, int32 From, int32 To);

	/**
	 * Version of the generators above, in the names of the baked meshes (AOceanIslandFlora::BakeMeshes): bump it when a
	 * generator changes, so that stale baked meshes are not loaded (the flora then generates its meshes until re-baked).
	 */
	constexpr int32 GeneratorVersion = 3;

#if WITH_EDITOR
	/**
	 * Saves Data as a Nanite static mesh asset at PackagePath (/Game/...): the flora built once in the editor instead of
	 * on every load, rendered by Nanite (millions of triangles, no levels of detail to author). bFoliage: thin parts
	 * simplified into voxels in the distance rather than thinning out (ENaniteShapePreservation::Voxelize).
	 */
	MAKAR_API UStaticMesh* BakeNaniteMesh(const FString& PackagePath, const FOceanMeshData& Data, const TArray<UMaterialInterface*>& Materials,
		int32 ColorSection, bool bFoliage);
#endif
}
