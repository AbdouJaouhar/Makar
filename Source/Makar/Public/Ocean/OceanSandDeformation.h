#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "OceanSandDeformation.generated.h"

class UTexture2D;

/**
 * The sand the player changes: footprints, holes dug and the spoil heaped beside them, a keel dragged up the beach.
 *
 * A height offset over a square window that follows the viewer (a few centimetres per texel), on the CPU so that
 * ground queries (walking, digging, a beached boat) see exactly what is drawn, as UOceanSubsystem does for the water;
 * the terrain material reads the same values from a texture. The sand then settles by itself:
 * - slopes steeper than the angle of repose slide (dry sand ~34 degrees; damp sand holds steeper walls),
 * - wherever the swash runs over it, the relief is smoothed out and the turned-over sand washed clean within a
 *   few waves,
 * - a hole dug below the water table fills with seawater (drawn by the material).
 * Offsets are relative to the island's analytic ground (AOceanBathymetry::SampleTerrain).
 */
UCLASS(ClassGroup = (Ocean), meta = (BlueprintSpawnableComponent))
class MAKAR_API UOceanSandDeformation : public UActorComponent
{
	GENERATED_BODY()

public:
	UOceanSandDeformation();

	/** Side of the deformable square around the viewer. */
	UPROPERTY(EditAnywhere, Category = "Sand", meta = (ClampMin = "4", Units = "Meters"))
	float WindowSize = 24.f;

	/** Texels along each side (WindowSize / Resolution = detail; 768 over 24 m is ~3 cm). */
	UPROPERTY(EditAnywhere, Category = "Sand", meta = (ClampMin = "64", ClampMax = "2048"))
	int32 Resolution = 768;

	/** Steepest slope dry sand holds before it slides. */
	UPROPERTY(EditAnywhere, Category = "Sand", meta = (ClampMin = "10", ClampMax = "60", Units = "Degrees"))
	float DryReposeAngle = 34.f;

	/** Damp sand (near the water, or deeper than a few centimetres) is held by capillary bridges: steeper walls. */
	UPROPERTY(EditAnywhere, Category = "Sand", meta = (ClampMin = "10", ClampMax = "80", Units = "Degrees"))
	float DampReposeAngle = 50.f;

	/** Time for the swash to wash out most of a relief it runs over (e-folding time while covered). */
	UPROPERTY(EditAnywhere, Category = "Sand", meta = (ClampMin = "0.1", Units = "Seconds"))
	float SwashSmoothingTime = 2.f;

	/** Height above sea level of the water table under the beach: holes dug below it fill with water. */
	UPROPERTY(EditAnywhere, Category = "Sand", meta = (Units = "Meters"))
	float WaterTable = 0.08f;

	/** Presses a footprint (~26 cm) into the sand, deeper in soft dry sand, a sharp shallow print in wet sand. */
	UFUNCTION(BlueprintCallable, Category = "Sand")
	void StampFootprint(const FVector& WorldLocation, float YawDegrees, bool bLeftFoot, float Weight = 1.f);

	/**
	 * Digs a bowl and heaps the sand beside it, towards SpoilDirection (volume conserved); the walls then settle to
	 * the angle of repose. Returns the volume moved, m^3.
	 */
	UFUNCTION(BlueprintCallable, Category = "Sand")
	float Dig(const FVector& WorldLocation, float RadiusM, float DepthM, const FVector& SpoilDirection);

	/** Change of the ground height at a world location, cm (0 outside the window). */
	UFUNCTION(BlueprintPure, Category = "Sand")
	float GetHeightOffsetAt(const FVector& WorldLocation) const;

	/** Moves the window to a location now (it otherwise follows the viewer). */
	UFUNCTION(BlueprintCallable, Category = "Sand")
	void SetFocus(const FVector& WorldLocation);

	/** RG = height offset (m), disturbed sand (0..1). Null until the first tick. */
	UTexture2D* GetTexture() const { return Texture; }

	/** XY = the window's lower corner (m), Z = its size (m); Z = 0 when there is no deformation yet. */
	FVector3f GetWindow() const { return FVector3f(OriginM.X, OriginM.Y, Texture ? WindowSize : 0.f); }

	/** Called by the owner with its terrain height function (m -> m) and when the window or texture changes. */
	TFunction<float(const FVector2f&)> GroundHeight;
	DECLARE_MULTICAST_DELEGATE(FOnWindowChanged);
	FOnWindowChanged OnWindowChanged;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void EnsureAllocated();
	void Recentre(const FVector2f& CentreM);
	/** Texel coordinates (continuous) of a position in m. */
	FVector2f ToTexel(const FVector2f& PositionM) const { return (PositionM - OriginM) / TexelM(); }
	float TexelM() const { return WindowSize / float(Resolution); }
	void Touch(int32 X0, int32 Y0, int32 X1, int32 Y1);
	/** Slides sand down slopes steeper than the angle of repose, inside the active rectangle. */
	bool SettleSlopes();
	/** The swash smooths what it covers and washes the disturbed sand. */
	void WashUnderSwash(float DeltaTime);
	void Upload();

	TArray<float> Height;  // m, Resolution^2
	TArray<float> Disturb; // 0..1
	FVector2f OriginM = FVector2f::ZeroVector;
	// Texels that may still change (settling, washing), and those changed since the last upload.
	FIntRect Active;
	FIntRect Dirty;
	bool bNeedsFullUpload = true;
	double LastWashTime = 0.0;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Texture;
};
