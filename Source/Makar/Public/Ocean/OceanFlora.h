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
	};

	TArray<FVertex> Vertices;
	TArray<TArray<int32>> Sections;

	explicit FOceanMeshData(int32 NumSections = 1) { Sections.SetNum(NumSections); }

	int32 AddVertex(const FVector3f& Position, const FVector3f& Normal, const FVector2f& UV, const FLinearColor& Color);
	void AddTriangle(int32 Section, int32 A, int32 B, int32 C);
};

/**
 * Procedural Caribbean flora and coastal rocks, built as static meshes at load time (nothing is saved).
 *  - Coconut palm: parametric (a palm does not branch): curved ringed trunk, arching fronds with leaflets, nuts.
 *  - Broadleaf tree and sea grape shrub: stochastic L-systems (string rewriting) read by a 3D turtle,
 *    with leaf clusters at the branch tips.
 *  - Rock: an icosphere displaced by 3D fractal noise, flattened at the base, like eroded coral limestone.
 * Section 0 = bark / trunk, 1 = foliage (plants); rocks have one section.
 */
namespace OceanFlora
{
	MAKAR_API FOceanMeshData MakeCoconutPalm(int32 Seed);
	MAKAR_API FOceanMeshData MakeBroadleafTree(int32 Seed);
	MAKAR_API FOceanMeshData MakeSeaGrape(int32 Seed);
	MAKAR_API FOceanMeshData MakeRock(int32 Seed);

	/** Builds a transient static mesh (works in the editor and in a cooked game). One material per section. */
	MAKAR_API UStaticMesh* BuildStaticMesh(UObject* Outer, const FOceanMeshData& Data, const TArray<UMaterialInterface*>& Materials);
}
