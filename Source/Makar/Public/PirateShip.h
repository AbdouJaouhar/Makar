#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "PirateShip.generated.h"

class UBoxComponent;
class UStaticMeshComponent;
class USpringArmComponent;
class UCameraComponent;
class UBuoyancyComponent;
class UInputMappingContext;
class UInputAction;
class ACannonball;
struct FInputActionValue;

/**
 * Physics-driven sailing ship.
 * Floats thanks to the Water plugin's buoyancy component (pontoons at the hull corners),
 * is pushed forward by its sails and turned by its rudder, and fires broadsides.
 */
UCLASS()
class MAKAR_API APirateShip : public APawn
{
	GENERATED_BODY()

public:
	APirateShip();

	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent,
		AController* EventInstigator, AActor* DamageCauser) override;

	/** 0 = sails furled, MaxSailLevel = full sail. */
	UFUNCTION(BlueprintCallable, Category = "Ship")
	void SetSailLevel(int32 NewLevel);

	/** -1 = hard to port (left), +1 = hard to starboard (right). */
	UFUNCTION(BlueprintCallable, Category = "Ship")
	void SetRudderInput(float Value);

	UFUNCTION(BlueprintCallable, Category = "Ship")
	void FireBroadside(bool bStarboard);

	UFUNCTION(BlueprintPure, Category = "Ship")
	bool IsSunk() const { return bSunk; }

	UFUNCTION(BlueprintPure, Category = "Ship")
	float GetHealth() const { return Health; }

	/** Hook for sinking FX / sounds in a Blueprint subclass. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Ship")
	void OnSunk();

	// --- Sailing tuning (accelerations, independent of mass) ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|Sailing")
	int32 MaxSailLevel = 3;

	/** Forward acceleration at full sail, cm/s². Top speed = SailAcceleration / ForwardDrag. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|Sailing")
	float SailAcceleration = 250.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|Sailing")
	float ForwardDrag = 0.2f;

	/** Resistance to sideways drift (the keel). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|Sailing")
	float KeelDrag = 1.5f;

	/** Yaw acceleration at full rudder and full speed, rad/s². */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|Sailing")
	float TurnAcceleration = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|Sailing")
	float YawDamping = 1.f;

	/** Speed (cm/s) at which the rudder reaches full authority. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|Sailing")
	float FullSteerSpeed = 600.f;

	// --- Combat tuning ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|Combat")
	float MaxHealth = 100.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|Combat")
	TSubclassOf<ACannonball> CannonballClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|Combat")
	int32 CannonsPerSide = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|Combat")
	float MuzzleSpeed = 5000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|Combat")
	float CannonElevationDegrees = 8.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ship|Combat")
	float ReloadTime = 3.f;

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ship")
	TObjectPtr<UBoxComponent> Hull;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ship")
	TObjectPtr<UStaticMeshComponent> HullMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ship")
	TObjectPtr<UStaticMeshComponent> MastMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ship")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ship")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ship")
	TObjectPtr<UBuoyancyComponent> Buoyancy;

	UPROPERTY(BlueprintReadOnly, Category = "Ship")
	int32 SailLevel = 0;

	/** Target rudder from input/AI; the actual rudder eases toward it. */
	float RudderInput = 0.f;
	float Rudder = 0.f;

	float Health = 0.f;
	bool bSunk = false;

private:
	void OnSteer(const FInputActionValue& Value);
	void OnSteerReleased(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void OnRaiseSails(const FInputActionValue& Value);
	void OnLowerSails(const FInputActionValue& Value);
	void OnFirePort(const FInputActionValue& Value);
	void OnFireStarboard(const FInputActionValue& Value);

	void Sink();

	// Input assets are built in code so the project needs no .uasset to be playable.
	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MappingContext;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> SteerAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> LookAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> RaiseSailsAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> LowerSailsAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> FirePortAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> FireStarboardAction;

	/** World time at which each side may fire again: [0] port, [1] starboard. */
	float NextFireTime[2] = { 0.f, 0.f };
};
