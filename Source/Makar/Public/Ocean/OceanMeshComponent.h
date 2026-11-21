#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "OceanMeshComponent.generated.h"

/**
 * Procedural mesh whose geometry is never written to disk.
 * AOceanActor rebuilds its grid on load, and saving it would add ~100 MB to every map containing the ocean.
 */
UCLASS(ClassGroup = (Rendering))
class MAKAR_API UOceanMeshComponent : public UProceduralMeshComponent
{
	GENERATED_BODY()

public:
	virtual void Serialize(FArchive& Ar) override;
};
