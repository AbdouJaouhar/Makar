#include "Ship/ShipCollision.h"

#include "PhysicsEngine/BodySetup.h"

UShipCollisionComponent::UShipCollisionComponent()
{
	SetCollisionProfileName(TEXT("BlockAll"));
	SetGenerateOverlapEvents(false);
	bHiddenInGame = true;
	SetMobility(EComponentMobility::Movable);
}

void UShipCollisionComponent::SetBoxes(const TArray<FBox3>& Boxes)
{
	Body = NewObject<UBodySetup>(this, NAME_None, RF_Transient);
	Body->CollisionTraceFlag = CTF_UseSimpleAsComplex;
	Body->bGenerateMirroredCollision = false;
	LocalBounds = FBox(ForceInit);
	for (const FBox3& B : Boxes)
	{
		FKBoxElem Elem(B.Half.X * 2.0, B.Half.Y * 2.0, B.Half.Z * 2.0);
		Elem.Center = B.Center;
		Elem.Rotation = B.Rotation;
		Body->AggGeom.BoxElems.Add(Elem);
		LocalBounds += FBox(B.Center - FVector(B.Half.GetMax()), B.Center + FVector(B.Half.GetMax()));
	}
	Body->CreatePhysicsMeshes();
	RecreatePhysicsState();
	UpdateBounds();
}

FBoxSphereBounds UShipCollisionComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	return LocalBounds.IsValid ? FBoxSphereBounds(LocalBounds).TransformBy(LocalToWorld) : FBoxSphereBounds(LocalToWorld.GetLocation(), FVector::ZeroVector, 0.f);
}
