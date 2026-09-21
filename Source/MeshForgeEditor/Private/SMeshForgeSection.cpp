#include "SMeshForgeSection.h"

#include "MeshForgeEditorStyle.h"

#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace MeshForgeSectionPrivate
{
	FLinearColor BadgeColour(EMeshSectionState State)
	{
		switch (State)
		{
		case EMeshSectionState::Done:      return MeshForgeStyle::Good();
		case EMeshSectionState::Current:   return MeshForgeStyle::Info();
		case EMeshSectionState::Attention: return MeshForgeStyle::Warn();
		default:                           return MeshForgeStyle::Quiet();
		}
	}
}

void SMeshForgeSection::Construct(const FArguments& InArgs)
{
	const TAttribute<EMeshSectionState> State = InArgs._State;
	const TAttribute<FText> Summary = InArgs._Summary;
	const TAttribute<FText> Subtitle = InArgs._Subtitle;
	const bool bInset = InArgs._Inset;

	TSharedRef<SHorizontalBox> Header = SNew(SHorizontalBox);

	// The badge: the position, or a tick once it has run.
	if (InArgs._Number > 0)
	{
		const float Size = bInset ? 20.f : 22.f;

		Header->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
		[
			SNew(SBox)
			.WidthOverride(Size)
			.HeightOverride(Size)
			[
				SNew(SBorder)
				.BorderImage(MeshForgeStyle::BadgeBrush())
				.BorderBackgroundColor_Lambda([State]() { return FSlateColor(MeshForgeSectionPrivate::BadgeColour(State.Get())); })
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.Padding(0.f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", bInset ? 9 : 10))
						.Text(FText::AsNumber(InArgs._Number))
						.ColorAndOpacity(FLinearColor::White)
						.Visibility_Lambda([State]() { return State.Get() == EMeshSectionState::Done ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
					]
					+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Check"))
						.ColorAndOpacity(FLinearColor::White)
						.DesiredSizeOverride(FVector2D(14.f, 14.f))
						.Visibility_Lambda([State]() { return State.Get() == EMeshSectionState::Done ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
					]
				]
			]
		];
	}

	Header->AddSlot().AutoWidth().VAlign(VAlign_Center)
	[
		SNew(STextBlock)
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", bInset ? 10 : 11))
		.Text(InArgs._Title)
	];

	// What the section holds or where it stands, so a closed one still says it.
	Header->AddSlot().FillWidth(1.f).VAlign(VAlign_Center).Padding(14.f, 0.f, 8.f, 0.f)
	[
		SNew(STextBlock)
		.Text(Summary)
		.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
	];

	if (InArgs._HeaderRight.Widget != SNullWidget::NullWidget)
	{
		Header->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			InArgs._HeaderRight.Widget
		];
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(bInset ? MeshForgeStyle::InsetBrush() : MeshForgeStyle::SectionBrush())
		.Padding(FMargin(1.f))
		[
			SAssignNew(Area, SExpandableArea)
			.InitiallyCollapsed(!InArgs._InitiallyExpanded)
			.AllowAnimatedTransition(false)
			.BorderImage(FAppStyle::GetNoBrush())
			.BodyBorderImage(FAppStyle::GetNoBrush())
			.HeaderPadding(bInset ? FMargin(8.f, 6.f) : FMargin(10.f, 9.f))
			.Padding(bInset ? FMargin(12.f, 0.f, 10.f, 10.f) : FMargin(16.f, 0.f, 14.f, 14.f))
			.OnAreaExpansionChanged(InArgs._OnExpansionChanged)
			.HeaderContent()
			[
				Header
			]
			.BodyContent()
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
				[
					SNew(STextBlock)
					.Text(Subtitle)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
					.Visibility_Lambda([Subtitle]() { return Subtitle.Get().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				]

				+ SVerticalBox::Slot().AutoHeight()
				[
					InArgs._Content.Widget
				]
			]
		]
	];
}

void SMeshForgeSection::SetExpanded(bool bExpanded)
{
	if (Area.IsValid())
	{
		Area->SetExpanded(bExpanded);
	}
}

bool SMeshForgeSection::IsExpanded() const
{
	return Area.IsValid() && Area->IsExpanded();
}
