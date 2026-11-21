#include "Ocean/OceanMeshComponent.h"

void UOceanMeshComponent::Serialize(FArchive& Ar)
{
	// Only skip real saves: undo/redo and PIE duplication must keep the geometry.
	if (!(Ar.IsSaving() && Ar.IsPersistent() && !Ar.IsTransacting()))
	{
		Super::Serialize(Ar);
		return;
	}

	// Write empty sections, then put the geometry back for the editor.
	TArray<FProcMeshSection> Sections;
	Sections.Reserve(GetNumSections());
	for (int32 Index = 0; Index < GetNumSections(); ++Index)
	{
		FProcMeshSection* Section = GetProcMeshSection(Index);
		Sections.Add(MoveTemp(*Section));
		*Section = FProcMeshSection();
	}

	Super::Serialize(Ar);

	for (int32 Index = 0; Index < Sections.Num(); ++Index)
	{
		*GetProcMeshSection(Index) = MoveTemp(Sections[Index]);
	}
}
