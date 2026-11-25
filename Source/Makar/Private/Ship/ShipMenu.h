#pragma once

#include "CoreMinimal.h"
#include "Ship/ShipDesign.h"
#include "Widgets/SCompoundWidget.h"

class AProceduralShip;
class AShipViewerController;
class SVerticalBox;
class SWidgetSwitcher;

/**
 * The ship viewer's options menu, in tabs: type and seed; hull lines; decks and guns; rig and sails; colours. Every
 * change generates the ship again (on release of a value being dragged).
 */
class SShipMenu : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SShipMenu) {}
		SLATE_ARGUMENT(TWeakObjectPtr<AShipViewerController>, Controller)
		SLATE_ARGUMENT(FShipDesign, Design)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	/** Sends the design to the ship. */
	void Apply();
	/** A new design of this type and seed (every hand edit is lost). */
	void Remake(EShipType Type, int32 Seed);

	TSharedRef<SWidget> TabButton(const FText& Label, int32 Index);
	TSharedRef<SWidget> TypeTab();
	TSharedRef<SWidget> HullTab();
	TSharedRef<SWidget> DecksTab();
	TSharedRef<SWidget> RigTab();
	TSharedRef<SWidget> ColoursTab();
	TSharedRef<SWidget> WindTab();
	/** A setting of the ship actor itself (the wind): changes at once, no new ship. */
	void AddShipFloat(const TSharedRef<SVerticalBox>& Box, const FText& Label, float AProceduralShip::* Field, float Min, float Max, float Step);

	void AddFloat(const TSharedRef<SVerticalBox>& Box, const FText& Label, float FShipDesign::* Field, float Min, float Max, float Step);
	void AddInt(const TSharedRef<SVerticalBox>& Box, const FText& Label, int32 FShipDesign::* Field, int32 Min, int32 Max);
	void AddBool(const TSharedRef<SVerticalBox>& Box, const FText& Label, bool FShipDesign::* Field);
	void AddColour(const TSharedRef<SVerticalBox>& Box, const FText& Label, FLinearColor FShipDesign::* Field);
	void AddDeck(const TSharedRef<SVerticalBox>& Box, int32 Index);
	TSharedRef<SWidget> Heading(const FText& Text);

	TWeakObjectPtr<AShipViewerController> Controller;
	FShipDesign Design;
	TSharedPtr<SWidgetSwitcher> Tabs;
	int32 ActiveTab = 0;
	FText Status;
};
