#include "ShipMenu.h"

#include "Ship/ProceduralShip.h"
#include "Ship/ShipViewer.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ShipMenu"

namespace
{
	FSlateFontInfo Font(int32 Size, const char* Weight = "Regular")
	{
		return FCoreStyle::GetDefaultFontStyle(Weight, Size);
	}

	const FLinearColor Panel(0.012f, 0.013f, 0.016f, 0.86f);
	const FLinearColor Accent(0.85f, 0.62f, 0.28f, 1.f);
	const FLinearColor TextColour(0.86f, 0.85f, 0.82f, 1.f);
	const FLinearColor Dim(0.55f, 0.55f, 0.55f, 1.f);

	FText TypeName(EShipType Type)
	{
		switch (Type)
		{
		case EShipType::Sloop: return LOCTEXT("Sloop", "Sloop");
		case EShipType::Brigantine: return LOCTEXT("Brigantine", "Brigantin");
		case EShipType::Frigate: return LOCTEXT("Frigate", "Frégate");
		default: return LOCTEXT("Galleon", "Galion");
		}
	}
}

void SShipMenu::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	Design = InArgs._Design;

	ChildSlot
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Fill)
	.Padding(16.f)
	[
		SNew(SBox)
		.WidthOverride(430.f)
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(Panel)
			.Padding(14.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
				[
					SNew(STextBlock).Text(LOCTEXT("Title", "Chantier naval")).Font(Font(18, "Bold")).ColorAndOpacity(Accent)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(1.f)[TabButton(LOCTEXT("TabType", "Navire"), 0)]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(1.f)[TabButton(LOCTEXT("TabHull", "Coque"), 1)]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(1.f)[TabButton(LOCTEXT("TabDecks", "Ponts"), 2)]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(1.f)[TabButton(LOCTEXT("TabRig", "Gréement"), 3)]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(1.f)[TabButton(LOCTEXT("TabColours", "Couleurs"), 4)]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(1.f)[TabButton(LOCTEXT("TabWind", "Vent"), 5)]
				]
				+ SVerticalBox::Slot().FillHeight(1.f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SAssignNew(Tabs, SWidgetSwitcher)
						.WidgetIndex_Lambda([this]() { return ActiveTab; })
						+ SWidgetSwitcher::Slot()[TypeTab()]
						+ SWidgetSwitcher::Slot()[HullTab()]
						+ SWidgetSwitcher::Slot()[DecksTab()]
						+ SWidgetSwitcher::Slot()[RigTab()]
						+ SWidgetSwitcher::Slot()[ColoursTab()]
						+ SWidgetSwitcher::Slot()[WindTab()]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
				[
					SNew(STextBlock).Text_Lambda([this]() { return Status; }).Font(Font(9)).ColorAndOpacity(Dim).AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
				[
					SNew(STextBlock).Text(LOCTEXT("Help", "Glisser : tourner autour   ·   Molette : zoom")).Font(Font(9)).ColorAndOpacity(Dim)
				]
			]
		]
	];
	Status = FText::Format(LOCTEXT("StatusStart", "{0}, graine {1}"), TypeName(Design.Type), Design.Seed);
}

TSharedRef<SWidget> SShipMenu::TabButton(const FText& Label, int32 Index)
{
	return SNew(SButton)
		.ButtonColorAndOpacity_Lambda([this, Index]() { return ActiveTab == Index ? Accent * 0.75f : FLinearColor(0.08f, 0.08f, 0.09f, 1.f); })
		.HAlign(HAlign_Center)
		.OnClicked_Lambda([this, Index]() { ActiveTab = Index; return FReply::Handled(); })
		[
			SNew(STextBlock).Text(Label).Font(Font(9, "Bold")).ColorAndOpacity(TextColour)
		];
}

TSharedRef<SWidget> SShipMenu::Heading(const FText& Text)
{
	return SNew(STextBlock).Text(Text).Font(Font(11, "Bold")).ColorAndOpacity(Accent);
}

void SShipMenu::Apply()
{
	if (AShipViewerController* PC = Controller.Get())
	{
		Status = LOCTEXT("Generating", "Génération…");
		TWeakPtr<SShipMenu> Weak = StaticCastSharedRef<SShipMenu>(AsShared());
		const FShipDesign Sent = Design;
		PC->ApplyDesign(Design, [Weak, Sent, Controller = Controller]()
		{
			TSharedPtr<SShipMenu> Menu = Weak.Pin();
			AShipViewerController* PC = Controller.Get();
			if (!Menu.IsValid() || !PC || !PC->GetShip())
			{
				return;
			}
			const AProceduralShip* Ship = PC->GetShip();
			Menu->Status = FText::Format(LOCTEXT("Status", "{0}, graine {1} · {2} m · {3} triangles · généré en {4} ms"), TypeName(Sent.Type), Sent.Seed,
				FText::AsNumber(FMath::RoundToInt(Sent.Length)), FText::AsNumber(Ship->Triangles), FText::AsNumber(FMath::RoundToInt(Ship->LastBuildMs)));
		});
	}
}

void SShipMenu::Remake(EShipType Type, int32 Seed)
{
	const bool bSails = Design.bSailsSet;
	Design = FShipDesign::Make(Type, Seed);
	Design.bSailsSet = bSails;
	Apply();
}

void SShipMenu::AddFloat(const TSharedRef<SVerticalBox>& Box, const FText& Label, float FShipDesign::* Field, float Min, float Max, float Step)
{
	Box->AddSlot().AutoHeight().Padding(0.f, 3.f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.55f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(Label).Font(Font(10)).ColorAndOpacity(TextColour)
		]
		+ SHorizontalBox::Slot().FillWidth(0.45f)
		[
			SNew(SSpinBox<float>)
			.MinValue(Min).MaxValue(Max).Delta(Step).MinFractionalDigits(2).MaxFractionalDigits(2)
			.Font(Font(10))
			.Value_Lambda([this, Field]() { return Design.*Field; })
			.OnValueChanged_Lambda([this, Field](float Value) { Design.*Field = Value; })
			.OnValueCommitted_Lambda([this, Field](float Value, ETextCommit::Type) { Design.*Field = Value; Apply(); })
		]
	];
}

void SShipMenu::AddInt(const TSharedRef<SVerticalBox>& Box, const FText& Label, int32 FShipDesign::* Field, int32 Min, int32 Max)
{
	Box->AddSlot().AutoHeight().Padding(0.f, 3.f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.55f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(Label).Font(Font(10)).ColorAndOpacity(TextColour)
		]
		+ SHorizontalBox::Slot().FillWidth(0.45f)
		[
			SNew(SSpinBox<int32>)
			.MinValue(Min).MaxValue(Max).Delta(1)
			.Font(Font(10))
			.Value_Lambda([this, Field]() { return Design.*Field; })
			.OnValueChanged_Lambda([this, Field](int32 Value) { Design.*Field = Value; })
			.OnValueCommitted_Lambda([this, Field](int32 Value, ETextCommit::Type) { Design.*Field = Value; Apply(); })
		]
	];
}

void SShipMenu::AddBool(const TSharedRef<SVerticalBox>& Box, const FText& Label, bool FShipDesign::* Field)
{
	Box->AddSlot().AutoHeight().Padding(0.f, 3.f)
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([this, Field]() { return Design.*Field ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this, Field](ECheckBoxState State) { Design.*Field = State == ECheckBoxState::Checked; Apply(); })
		[
			SNew(STextBlock).Text(Label).Font(Font(10)).ColorAndOpacity(TextColour)
		]
	];
}

void SShipMenu::AddColour(const TSharedRef<SVerticalBox>& Box, const FText& Label, FLinearColor FShipDesign::* Field)
{
	auto Channel = [this, Field](int32 C) -> TSharedRef<SWidget>
	{
		return SNew(SSpinBox<float>)
			.MinValue(0.f).MaxValue(1.f).Delta(0.01f).MinFractionalDigits(2).MaxFractionalDigits(2)
			.Font(Font(9))
			.Value_Lambda([this, Field, C]() { return (&(Design.*Field).R)[C]; })
			.OnValueChanged_Lambda([this, Field, C](float Value) { (&(Design.*Field).R)[C] = Value; })
			.OnValueCommitted_Lambda([this, Field, C](float Value, ETextCommit::Type) { (&(Design.*Field).R)[C] = Value; Apply(); });
	};
	Box->AddSlot().AutoHeight().Padding(0.f, 4.f)
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Label).Font(Font(10)).ColorAndOpacity(TextColour)
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).WidthOverride(60.f).HeightOverride(16.f)
				[
					SNew(SColorBlock).Color_Lambda([this, Field]() { FLinearColor C = Design.*Field; C.A = 1.f; return C; })
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 0.f, 2.f, 0.f)[Channel(0)]
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(2.f, 0.f)[Channel(1)]
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(2.f, 0.f, 0.f, 0.f)[Channel(2)]
		]
	];
}

void SShipMenu::AddDeck(const TSharedRef<SVerticalBox>& Box, int32 Index)
{
	Box->AddSlot().AutoHeight().Padding(0.f, 3.f)
	[
		SNew(SHorizontalBox)
		.Visibility_Lambda([this, Index]() { return Design.RaisedDecks.IsValidIndex(Index) ? EVisibility::Visible : EVisibility::Collapsed; })
		+ SHorizontalBox::Slot().FillWidth(0.55f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Font(Font(10)).ColorAndOpacity(TextColour)
			.Text_Lambda([this, Index]()
			{
				if (!Design.RaisedDecks.IsValidIndex(Index))
				{
					return FText::GetEmpty();
				}
				const FShipDeckLevel& L = Design.RaisedDecks[Index];
				const FText Name = L.To >= 0.95f ? LOCTEXT("Forecastle", "Gaillard d'avant")
					: (Index > 0 && L.To < Design.RaisedDecks[0].To ? LOCTEXT("Poop", "Dunette") : LOCTEXT("Quarterdeck", "Gaillard d'arrière"));
				return FText::Format(LOCTEXT("DeckHeight", "{0} : hauteur"), Name);
			})
		]
		+ SHorizontalBox::Slot().FillWidth(0.45f)
		[
			SNew(SSpinBox<float>)
			.MinValue(0.f).MaxValue(3.5f).Delta(0.05f).MinFractionalDigits(2).MaxFractionalDigits(2)
			.Font(Font(10))
			.Value_Lambda([this, Index]() { return Design.RaisedDecks.IsValidIndex(Index) ? Design.RaisedDecks[Index].Height : 0.f; })
			.OnValueChanged_Lambda([this, Index](float Value) { if (Design.RaisedDecks.IsValidIndex(Index)) { Design.RaisedDecks[Index].Height = Value; } })
			.OnValueCommitted_Lambda([this, Index](float Value, ETextCommit::Type)
			{
				if (Design.RaisedDecks.IsValidIndex(Index))
				{
					Design.RaisedDecks[Index].Height = Value;
					Apply();
				}
			})
		]
	];
}

TSharedRef<SWidget> SShipMenu::TypeTab()
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	Box->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)[Heading(LOCTEXT("Type", "Type de navire"))];
	for (EShipType Type : { EShipType::Sloop, EShipType::Brigantine, EShipType::Frigate, EShipType::Galleon })
	{
		Box->AddSlot().AutoHeight().Padding(0.f, 2.f)
		[
			SNew(SButton)
			.ButtonColorAndOpacity_Lambda([this, Type]() { return Design.Type == Type ? Accent * 0.75f : FLinearColor(0.08f, 0.08f, 0.09f, 1.f); })
			.OnClicked_Lambda([this, Type]() { Remake(Type, Design.Seed); return FReply::Handled(); })
			[
				SNew(STextBlock).Text(TypeName(Type)).Font(Font(11, "Bold")).ColorAndOpacity(TextColour)
			]
		];
	}
	Box->AddSlot().AutoHeight().Padding(0.f, 12.f, 0.f, 6.f)[Heading(LOCTEXT("Variant", "Variante"))];
	Box->AddSlot().AutoHeight().Padding(0.f, 3.f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.3f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("Seed", "Graine")).Font(Font(10)).ColorAndOpacity(TextColour)
		]
		+ SHorizontalBox::Slot().FillWidth(0.35f)
		[
			SNew(SSpinBox<int32>)
			.MinValue(0).MaxValue(99999).Delta(1).Font(Font(10))
			.Value_Lambda([this]() { return Design.Seed; })
			.OnValueChanged_Lambda([this](int32 Value) { Design.Seed = Value; })
			.OnValueCommitted_Lambda([this](int32 Value, ETextCommit::Type) { Remake(Design.Type, Value); })
		]
		+ SHorizontalBox::Slot().FillWidth(0.35f).Padding(6.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.HAlign(HAlign_Center)
			.OnClicked_Lambda([this]() { Remake(Design.Type, FMath::RandRange(0, 99999)); return FReply::Handled(); })
			[
				SNew(STextBlock).Text(LOCTEXT("Random", "Au hasard")).Font(Font(10)).ColorAndOpacity(TextColour)
			]
		]
	];
	Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
	[
		SNew(STextBlock).AutoWrapText(true).Font(Font(9)).ColorAndOpacity(Dim)
		.Text(LOCTEXT("TypeHelp", "Changer de type ou de graine tire un nouveau navire : les réglages des autres onglets repartent de ses valeurs."))
	];
	return Box;
}

TSharedRef<SWidget> SShipMenu::HullTab()
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	Box->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)[Heading(LOCTEXT("Dimensions", "Dimensions (m)"))];
	AddFloat(Box, LOCTEXT("Length", "Longueur"), &FShipDesign::Length, 12.f, 50.f, 0.1f);
	AddFloat(Box, LOCTEXT("Beam", "Largeur"), &FShipDesign::Beam, 3.f, 15.f, 0.05f);
	AddFloat(Box, LOCTEXT("Depth", "Creux"), &FShipDesign::Depth, 1.5f, 9.f, 0.05f);
	AddFloat(Box, LOCTEXT("Draft", "Tirant d'eau"), &FShipDesign::Draft, 1.f, 7.f, 0.05f);
	Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 6.f)[Heading(LOCTEXT("Lines", "Formes"))];
	AddFloat(Box, LOCTEXT("Fullness", "Plénitude du maître-couple"), &FShipDesign::SectionFullness, 1.1f, 3.5f, 0.02f);
	AddFloat(Box, LOCTEXT("MaxBeam", "Hauteur du fort"), &FShipDesign::MaxBeamHeight, 0.4f, 0.9f, 0.01f);
	AddFloat(Box, LOCTEXT("Bow", "Rondeur de l'avant"), &FShipDesign::BowFullness, 1.2f, 3.5f, 0.02f);
	AddFloat(Box, LOCTEXT("Stern", "Rondeur de l'arrière"), &FShipDesign::SternFullness, 1.1f, 3.f, 0.02f);
	AddFloat(Box, LOCTEXT("Tumblehome", "Rentrée des hauts"), &FShipDesign::Tumblehome, 0.f, 0.3f, 0.005f);
	AddFloat(Box, LOCTEXT("SheerFore", "Tonture avant"), &FShipDesign::SheerFore, 0.f, 2.5f, 0.02f);
	AddFloat(Box, LOCTEXT("SheerAft", "Tonture arrière"), &FShipDesign::SheerAft, 0.f, 2.5f, 0.02f);
	AddFloat(Box, LOCTEXT("StemRake", "Élancement de l'étrave"), &FShipDesign::StemRake, 0.2f, 6.f, 0.05f);
	AddFloat(Box, LOCTEXT("SternRake", "Quête de l'étambot"), &FShipDesign::SternRake, 0.f, 2.f, 0.02f);
	AddFloat(Box, LOCTEXT("TransomWidth", "Largeur du tableau"), &FShipDesign::TransomWidth, 0.3f, 0.9f, 0.01f);
	AddFloat(Box, LOCTEXT("TransomBottom", "Hauteur de la voûte"), &FShipDesign::TransomBottom, 0.35f, 0.8f, 0.01f);
	Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 6.f)[Heading(LOCTEXT("Planking", "Bordage"))];
	AddFloat(Box, LOCTEXT("PlankWidth", "Largeur des bordages"), &FShipDesign::PlankWidth, 0.15f, 0.4f, 0.005f);
	AddFloat(Box, LOCTEXT("PlankLength", "Longueur des bordages"), &FShipDesign::PlankLength, 3.f, 12.f, 0.1f);
	return Box;
}

TSharedRef<SWidget> SShipMenu::DecksTab()
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	Box->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)[Heading(LOCTEXT("Decks", "Ponts"))];
	AddFloat(Box, LOCTEXT("Bulwark", "Hauteur du pavois"), &FShipDesign::Bulwark, 0.5f, 2.f, 0.02f);
	for (int32 I = 0; I < 3; ++I)
	{
		AddDeck(Box, I);
	}
	Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 6.f)[Heading(LOCTEXT("Guns", "Artillerie"))];
	AddInt(Box, LOCTEXT("GunsUpper", "Canons par bord (pont)"), &FShipDesign::Guns, 0, 16);
	AddInt(Box, LOCTEXT("GunsLower", "Canons par bord (batterie basse)"), &FShipDesign::LowerGuns, 0, 16);
	AddFloat(Box, LOCTEXT("GunLength", "Longueur des pièces"), &FShipDesign::GunLength, 1.2f, 3.2f, 0.05f);
	return Box;
}

TSharedRef<SWidget> SShipMenu::RigTab()
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	Box->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)[Heading(LOCTEXT("Rig", "Mâture"))];
	AddFloat(Box, LOCTEXT("MastHeight", "Hauteur des mâts (× longueur)"), &FShipDesign::MastHeight, 0.6f, 1.4f, 0.01f);
	AddFloat(Box, LOCTEXT("Bowsprit", "Beaupré (× longueur)"), &FShipDesign::Bowsprit, 0.2f, 0.8f, 0.01f);
	Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 6.f)[Heading(LOCTEXT("Sails", "Voiles"))];
	AddBool(Box, LOCTEXT("SailsSet", "Voiles établies (sinon ferlées)"), &FShipDesign::bSailsSet);
	Box->AddSlot().AutoHeight()
	[
		SNew(SBox)
		.Visibility_Lambda([this]() { return Design.Type == EShipType::Galleon ? EVisibility::Visible : EVisibility::Collapsed; })
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this]() { return Design.bBonaventure ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Design.bBonaventure = State == ECheckBoxState::Checked; Apply(); })
			[
				SNew(STextBlock).Text(LOCTEXT("Bonaventure", "Quatrième mât (contre-artimon)")).Font(Font(10)).ColorAndOpacity(TextColour)
			]
		]
	];
	return Box;
}

TSharedRef<SWidget> SShipMenu::ColoursTab()
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	Box->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)[Heading(LOCTEXT("Paint", "Peinture (R, V, B)"))];
	AddColour(Box, LOCTEXT("Topsides", "Œuvres mortes"), &FShipDesign::TopsideColor);
	AddColour(Box, LOCTEXT("Wales", "Préceintes et lisses"), &FShipDesign::WaleColor);
	AddColour(Box, LOCTEXT("Band", "Bande des sabords"), &FShipDesign::BandColor);
	Box->AddSlot().AutoHeight().Padding(0.f, 2.f)
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([this]() { return Design.BandColor.A > 0.f ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Design.BandColor.A = State == ECheckBoxState::Checked ? 1.f : 0.f; Apply(); })
		[
			SNew(STextBlock).Text(LOCTEXT("BandOn", "Bande peinte")).Font(Font(10)).ColorAndOpacity(TextColour)
		]
	];
	AddColour(Box, LOCTEXT("Bottom", "Carène"), &FShipDesign::BottomColor);
	AddColour(Box, LOCTEXT("DeckColour", "Pont"), &FShipDesign::DeckColor);
	AddColour(Box, LOCTEXT("SailColour", "Voiles"), &FShipDesign::SailColor);
	return Box;
}

void SShipMenu::AddShipFloat(const TSharedRef<SVerticalBox>& Box, const FText& Label, float AProceduralShip::* Field, float Min, float Max, float Step)
{
	auto Ship = [this]() -> AProceduralShip* { AShipViewerController* PC = Controller.Get(); return PC ? PC->GetShip() : nullptr; };
	Box->AddSlot().AutoHeight().Padding(0.f, 3.f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.55f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(Label).Font(Font(10)).ColorAndOpacity(TextColour)
		]
		+ SHorizontalBox::Slot().FillWidth(0.45f)
		[
			SNew(SSpinBox<float>)
			.MinValue(Min).MaxValue(Max).Delta(Step).MinFractionalDigits(0).MaxFractionalDigits(1)
			.Font(Font(10))
			.Value_Lambda([Ship, Field]() { AProceduralShip* S = Ship(); return S ? S->*Field : 0.f; })
			.OnValueChanged_Lambda([Ship, Field](float Value) { if (AProceduralShip* S = Ship()) { S->*Field = Value; } })
			.OnValueCommitted_Lambda([Ship, Field](float Value, ETextCommit::Type) { if (AProceduralShip* S = Ship()) { S->*Field = Value; } })
		]
	];
}

TSharedRef<SWidget> SShipMenu::WindTab()
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	Box->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)[Heading(LOCTEXT("Wind", "Vent"))];
	AddShipFloat(Box, LOCTEXT("WindFrom", "D'où il vient (° depuis l'étrave)"), &AProceduralShip::WindFrom, 0.f, 360.f, 1.f);
	AddShipFloat(Box, LOCTEXT("WindSpeed", "Force (m/s)"), &AProceduralShip::WindSpeed, 0.f, 25.f, 0.1f);
	Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
	[
		SNew(STextBlock).AutoWrapText(true).Font(Font(9)).ColorAndOpacity(Dim)
		.Text(LOCTEXT("WindHelp", "0° : vent debout, 90° : de tribord, 180° : vent arrière, 270° : de bâbord. Les voiles sont de la toile simulée : "
			"elles se gonflent, faseyent ou sont masquées selon le vent, et s'appuient sur les cordages et les mâts. 7 m/s : jolie brise (force 4)."))
	];
	return Box;
}

#undef LOCTEXT_NAMESPACE
