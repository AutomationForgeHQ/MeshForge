#include "SMeshDefImages.h"

#include "MeshDef.h"
#include "MeshForge.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/Texture2D.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Modules/ModuleManager.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MeshImageIngest.h"
#include "MeshForgeImageSlots.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "Input/DragAndDrop.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MeshForgeEditor"

namespace MeshDefImagesPrivate
{
	/** The filmstrip. Big enough to tell two takes apart, small enough that eight fit across. */
	static constexpr float ThumbSize = 96.0f;

	/** What the strip takes off the bottom; the preview gets everything else. */
	static constexpr float StripHeight = 150.0f;

	/**
	 * The picture's real pixel size.
	 *
	 * From the texture *source*, not GetSizeX - that reports whatever mip streaming has decided to
	 * keep resident, which for a picture nobody has looked at yet is 32 pixels. Everything here is
	 * a ratio, and a ratio computed from the wrong numbers is still a ratio, which is the sort of
	 * wrong that looks plausible.
	 */
	static FVector2D PixelSize(UTexture2D* Texture)
	{
		if (Texture == nullptr)
		{
			return FVector2D(1.0, 1.0);
		}

#if WITH_EDITORONLY_DATA
		if (Texture->Source.IsValid())
		{
			return FVector2D(
				FMath::Max(1, static_cast<int32>(Texture->Source.GetSizeX())),
				FMath::Max(1, static_cast<int32>(Texture->Source.GetSizeY())));
		}
#endif

		return FVector2D(
			FMath::Max(1, Texture->GetSizeX()),
			FMath::Max(1, Texture->GetSizeY()));
	}

	/**
	 * A brush that draws the picture at its own proportions, scaled to fit a box of this size.
	 *
	 * The size matters because SScaleBox scales the child's *desired* size, and a brush's desired
	 * size is whatever ImageSize says. Declaring a square made every reference square: a 1448x1086
	 * photograph was squeezed into a 1:1 frame and read as a different picture.
	 */
	/** How big a chosen-slot thumbnail is. Bigger than a strip thumbnail: these are the decisions. */
	static constexpr float SlotSize = 76.0f;

	static TSharedRef<FSlateBrush> MakeFittedBrush(UTexture2D* Texture, float LongestSide)
	{
		const FVector2D Pixels = PixelSize(Texture);
		const double Scale = LongestSide / FMath::Max(Pixels.X, Pixels.Y);

		TSharedRef<FSlateBrush> Brush = MakeShared<FSlateBrush>();
		Brush->SetResourceObject(Texture);
		Brush->ImageSize = Pixels * Scale;
		Brush->DrawAs = ESlateBrushDrawType::Image;
		return Brush;
	}

	/**
	 * A square brush that *crops* to fill rather than squeezing.
	 *
	 * The strip stays square, because a row of tiles at mixed proportions is much harder to scan
	 * than a row of squares - but a squeezed picture is a lie about what it is. So the middle of the
	 * picture fills the square and the overflow is cut off, which is what every gallery does.
	 */
	static TSharedRef<FSlateBrush> MakeCroppedBrush(UTexture2D* Texture, float Size)
	{
		const FVector2D Pixels = PixelSize(Texture);

		TSharedRef<FSlateBrush> Brush = MakeShared<FSlateBrush>();
		Brush->SetResourceObject(Texture);
		Brush->ImageSize = FVector2D(Size, Size);
		Brush->DrawAs = ESlateBrushDrawType::Image;

		// The centre square, in UV. Cropping here rather than with a clipping widget keeps the tile
		// one draw and means the strip does not need a scale box per thumbnail.
		if (Pixels.X > Pixels.Y)
		{
			const float Keep = static_cast<float>(Pixels.Y / Pixels.X);
			const float Edge = (1.0f - Keep) * 0.5f;
			Brush->SetUVRegion(FBox2f(FVector2f(Edge, 0.0f), FVector2f(Edge + Keep, 1.0f)));
		}
		else if (Pixels.Y > Pixels.X)
		{
			const float Keep = static_cast<float>(Pixels.X / Pixels.Y);
			const float Edge = (1.0f - Keep) * 0.5f;
			Brush->SetUVRegion(FBox2f(FVector2f(0.0f, Edge), FVector2f(1.0f, Edge + Keep)));
		}

		return Brush;
	}

	static FText OriginName(EMeshDefImageOrigin Origin)
	{
		switch (Origin)
		{
		case EMeshDefImageOrigin::Added:   return LOCTEXT("OriginAdded", "Added");
		case EMeshDefImageOrigin::Refined: return LOCTEXT("OriginRefined", "Refined");
		default:                           return LOCTEXT("OriginDrawn", "Drawn");
		}
	}

	static FString ShortName(const TSoftObjectPtr<UTexture2D>& Image)
	{
		return FPackageName::ObjectPathToObjectName(Image.ToString());
	}
}

void SMeshDefImages::Construct(const FArguments& InArgs)
{
	Definition         = InArgs._Definition;
	OnSelectionChanged = InArgs._OnSelectionChanged;

	ChildSlot
	[
		SNew(SVerticalBox)

		// The header stays put: it holds the one sentence saying what the generator will be shown,
		// and that is the thing being checked while walking the strip.
		+ SVerticalBox::Slot().AutoHeight().Padding(12.0f, 12.0f, 12.0f, 8.0f)
		[
			BuildHeader()
		]

		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(12.0f, 0.0f, 12.0f, 0.0f)
		[
			SNew(SOverlay)

			+ SOverlay::Slot()
			[
				SNew(SBox)
				.Visibility(this, &SMeshDefImages::ContentVisibility)
				[
					BuildPreview()
				]
			]

			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SBox).MaxDesiredWidth(420.0f)
				[
					SNew(STextBlock)
					.Visibility(this, &SMeshDefImages::EmptyVisibility)
					.Text(LOCTEXT("NoImagesYet",
						"No pictures yet.\n\nDrop one in from anywhere - a file on disk or a texture "
						"from the Content Browser - or write a prompt and run the Concept stage to "
						"draw one."))
					.Justification(ETextJustify::Center)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		]

		// The strip along the bottom, scrolling sideways once there are more than fit.
		+ SVerticalBox::Slot().AutoHeight().Padding(12.0f, 8.0f, 12.0f, 12.0f)
		[
			SNew(SBox)
			.Visibility(this, &SMeshDefImages::ContentVisibility)
			.HeightOverride(MeshDefImagesPrivate::StripHeight)
			[
				SNew(SScrollBox)
				.Orientation(Orient_Horizontal)

				+ SScrollBox::Slot()
				[
					SAssignNew(Strip, SHorizontalBox)
				]
			]
		]
	];

	Refresh();
}

TSharedRef<SWidget> SMeshDefImages::BuildHeader()
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(this, &SMeshDefImages::SelectionSummary)
			.AutoWrapText(true)
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.0f, 0.0f, 0.0f, 0.0f)
		[
			SAssignNew(AddButton, SComboButton)
			.ContentPadding(FMargin(6.0f, 2.0f))
			.ToolTipText(LOCTEXT("AddImageTip",
				"Bring a texture already in the project into this definition's gallery - a "
				"photograph, a render, a frame from somewhere else. It can be chosen exactly like a "
				"picture drawn here."))
			.OnGetMenuContent(this, &SMeshDefImages::BuildAddMenu)
			.ButtonContent()
			[
				SNew(STextBlock).Text(LOCTEXT("AddImage", "Add picture..."))
			]
		];
}

TSharedRef<SWidget> SMeshDefImages::BuildPreview()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text(LOCTEXT("PrevImage", "<"))
				.ToolTipText(LOCTEXT("PrevImageTip", "The picture before this one. Left arrow does the same."))
				.OnClicked_Lambda([this]() { return Step(-1); })
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(8.0f, 0.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(4.0f)
				[
					// Scaled to fit rather than drawn at a fixed size: these are square today and
					// will not be once an aspect ratio is chosen on the Meshy pipeline, and a
					// portrait picture squashed into a square frame is worse than a small one.
					SNew(SScaleBox)
					.Stretch(EStretch::ScaleToFit)
					[
						SNew(SImage).Image(this, &SMeshDefImages::PreviewBrush)
					]
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text(LOCTEXT("NextImage", ">"))
				.ToolTipText(LOCTEXT("NextImageTip", "The picture after this one. Right arrow does the same."))
				.OnClicked_Lambda([this]() { return Step(1); })
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(this, &SMeshDefImages::PreviewCaption)
				]

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(this, &SMeshDefImages::PreviewRole)
					.ColorAndOpacity(this, &SMeshDefImages::PreviewRoleColour)
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SMeshDefImages::PreviewPosition)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(this, &SMeshDefImages::MainButtonText)
				.IsEnabled(this, &SMeshDefImages::CanSetMain)
				.ToolTipText(LOCTEXT("UseAsMainTip",
					"Make this the picture the mesh generator is shown. Marks the stages after it "
					"stale; it does not discard what they already produced."))
				.OnClicked_Lambda([this]()
				{
					if (const TSharedPtr<FMeshDefImageEntry> Entry = Current())
					{
						OnSetMain(Entry->Path);
					}
					return FReply::Handled();
				})
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(this, &SMeshDefImages::ViewButtonText)
				.Visibility(this, &SMeshDefImages::ViewButtonVisibility)
				.IsEnabled(this, &SMeshDefImages::CanToggleView)
				.ToolTipText(LOCTEXT("AddViewTip",
					"Send this as another angle of the same object. Worth being sceptical of: a "
					"model fuses contradictory views rather than averaging them, so use these only "
					"when they are a genuine orbit of one object."))
				.OnClicked_Lambda([this]()
				{
					if (const TSharedPtr<FMeshDefImageEntry> Entry = Current())
					{
						OnToggleView(Entry->Path);
					}
					return FReply::Handled();
				})
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Find", "Find"))
				.ToolTipText(LOCTEXT("FindTip", "Show this texture in the Content Browser."))
				.OnClicked_Lambda([this]()
				{
					if (const TSharedPtr<FMeshDefImageEntry> Entry = Current())
					{
						OnShowInContentBrowser(Entry->Texture);
					}
					return FReply::Handled();
				})
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("RemoveImage", "Remove"))
				.ToolTipText(LOCTEXT("RemoveImageTip",
					"Take this picture out of the gallery. The texture stays in the project - delete "
					"it from the Content Browser if you want it gone, because a reference that is "
					"wrong for this prop is often right for another."))
				.OnClicked_Lambda([this]()
				{
					if (const TSharedPtr<FMeshDefImageEntry> Entry = Current())
					{
						OnRemoveImage(Entry->Path);
					}
					return FReply::Handled();
				})
			]
		];
}

TSharedRef<SWidget> SMeshDefImages::BuildAddMenu()
{
	FContentBrowserModule& Browser =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	FAssetPickerConfig Config;
	Config.Filter.ClassPaths.Add(UTexture2D::StaticClass()->GetClassPathName());
	Config.Filter.bRecursiveClasses = true;
	Config.SelectionMode            = ESelectionMode::Single;
	Config.InitialAssetViewType     = EAssetViewType::Tile;
	Config.bAllowNullSelection      = false;
	Config.bAllowDragging           = false;
	Config.OnAssetSelected =
		FOnAssetSelected::CreateSP(this, &SMeshDefImages::OnAddImage);

	return SNew(SBox)
		.WidthOverride(420.0f)
		.HeightOverride(480.0f)
		[
			Browser.Get().CreateAssetPicker(Config)
		];
}

void SMeshDefImages::Refresh()
{
	const TSoftObjectPtr<UTexture2D> WasShowing =
		Entries.IsValidIndex(PreviewIndex) ? Entries[PreviewIndex]->Path : TSoftObjectPtr<UTexture2D>();

	Entries.Reset();

	UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		if (Strip.IsValid()) { Strip->ClearChildren(); }
		return;
	}

	const TSoftObjectPtr<UTexture2D> Main = Def->ResolveMainImage();

	// The pool the definition itself reports, so the gallery and the mesh stage can never disagree
	// about what pictures exist.
	for (const TSoftObjectPtr<UTexture2D>& Image : Def->GatherImagePool())
	{
		UTexture2D* Texture = Image.LoadSynchronous();
		if (Texture == nullptr)
		{
			// A picture whose asset was deleted from underneath the definition. Skipped rather than
			// drawn broken: the definition still holds the pointer, and saying so belongs in the log
			// rather than in a gallery.
			continue;
		}

		EMeshDefImageOrigin Origin = EMeshDefImageOrigin::Added;
		int32 Ordinal = 0;

		if (const int32 Index = Def->ConceptImages.IndexOfByKey(Image); Index != INDEX_NONE)
		{
			Origin  = EMeshDefImageOrigin::Drawn;
			Ordinal = Index + 1;
		}
		else if (const int32 Refined = Def->ReferenceImages.IndexOfByKey(Image); Refined != INDEX_NONE)
		{
			Origin  = EMeshDefImageOrigin::Refined;
			Ordinal = Refined + 1;
		}

		TSharedPtr<FMeshDefImageEntry> Entry = MakeShared<FMeshDefImageEntry>();
		Entry->Texture   = Texture;
		Entry->Path      = Image;
		Entry->Origin    = Origin;
		Entry->bIsMain   = (Image == Main);
		Entry->ViewIndex = Def->ExtraViews.IndexOfByKey(Image);

		Entry->Thumbnail = MeshDefImagesPrivate::MakeCroppedBrush(Texture, MeshDefImagesPrivate::ThumbSize);
		Entry->Large     = MeshDefImagesPrivate::MakeFittedBrush(Texture, 1024.0f);

		Entry->Label = (Ordinal > 0)
			? FText::Format(LOCTEXT("OriginN", "{0} {1}"),
				MeshDefImagesPrivate::OriginName(Origin), FText::AsNumber(Ordinal))
			: MeshDefImagesPrivate::OriginName(Origin);

		Entries.Add(Entry);
	}

	// Keep looking at the same picture across a refresh where we can. Drawing a new one while
	// comparing two others should not yank the preview away from what is being compared.
	PreviewIndex = Entries.IndexOfByPredicate(
		[&WasShowing](const TSharedPtr<FMeshDefImageEntry>& Entry) { return Entry->Path == WasShowing; });

	if (PreviewIndex == INDEX_NONE)
	{
		// Otherwise the chosen one, which is what somebody opening the tab wants to see.
		PreviewIndex = Entries.IndexOfByPredicate(
			[](const TSharedPtr<FMeshDefImageEntry>& Entry) { return Entry->bIsMain; });
	}

	PreviewIndex = FMath::Max(0, PreviewIndex);

	if (!Strip.IsValid())
	{
		return;
	}

	Strip->ClearChildren();

	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		Strip->AddSlot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			BuildThumbnail(Entries[Index], Index)
		];
	}
}

TSharedRef<SWidget> SMeshDefImages::BuildThumbnail(TSharedPtr<FMeshDefImageEntry> Entry, int32 Index)
{
	// A coloured edge rather than a badge: the role of a picture is the thing being scanned for
	// along a strip of twenty, and a colour reads at a glance where a word does not.
	const bool bHasRole = Entry->bIsMain || (Entry->ViewIndex != INDEX_NONE);
	const TSoftObjectPtr<UTexture2D> Path = Entry->Path;

	const FLinearColor Edge = Entry->bIsMain
		? FLinearColor(0.35f, 0.75f, 0.40f)
		: (Entry->ViewIndex != INDEX_NONE
			? FLinearColor(0.40f, 0.65f, 0.95f)
			: FLinearColor(1.0f, 1.0f, 1.0f, 0.15f));

	return SNew(SBox)
		.WidthOverride(MeshDefImagesPrivate::ThumbSize + 8.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight()
			[
				// A border rather than a button, because a button only answers the left mouse. Left
				// previews, right opens the role menu - which is where assigning a view belongs: a
				// picture's job is a property of the picture, not of whichever one is on screen.
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.BorderBackgroundColor(Edge)
				.Padding(3.0f)
				.ToolTipText(FText::Format(
					LOCTEXT("ThumbTip",
						"{0}\nLeft-click to see it large, right-click for its role - or drag it onto "
						"a slot on the Mesh stage."),
					Entry->Label))
				.OnMouseButtonDown_Lambda(
					[this, Index, Path, Label = Entry->Label](
						const FGeometry& Geometry, const FPointerEvent& Event) -> FReply
				{
					PreviewIndex = Index;

					if (Event.GetEffectingButton() == EKeys::RightMouseButton)
					{
						FSlateApplication::Get().PushMenu(
							AsShared(), FWidgetPath(), BuildRoleMenu(Path),
							FVector2D(Event.GetScreenSpacePosition()),
							FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));

						return FReply::Handled();
					}

					// **Armed here, answered by the panel.** DetectDrag needs a widget that will be asked
					// for the operation, and a border built inside a loop is not one - so the panel
					// remembers which picture the press was on and speaks for it.
					DragCandidate = Path;
					DragCandidateLabel = Label;

					return FReply::Handled().DetectDrag(SharedThis(this), EKeys::LeftMouseButton);
				})
				[
					SNew(SBox)
					.WidthOverride(MeshDefImagesPrivate::ThumbSize)
					.HeightOverride(MeshDefImagesPrivate::ThumbSize)
					[
						SNew(SImage).Image(Entry->Thumbnail.Get())
					]
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(Entry->Label)
				.Justification(ETextJustify::Center)
				// Not the edge colour. An unused picture's edge is a barely-there 15% white, which
				// is right for a border and unreadable as text.
				.ColorAndOpacity(bHasRole ? FSlateColor(Edge) : FSlateColor::UseForeground())
			]
		];
}

TSharedPtr<FMeshDefImageEntry> SMeshDefImages::Current() const
{
	return Entries.IsValidIndex(PreviewIndex) ? Entries[PreviewIndex] : nullptr;
}


FReply SMeshDefImages::Step(int32 Delta)
{
	if (Entries.Num() > 0)
	{
		// Wraps, so one arrow walks the whole gallery rather than stopping at an end nobody can see
		// from the middle of a strip.
		PreviewIndex = (PreviewIndex + Delta + Entries.Num()) % Entries.Num();
	}

	return FReply::Handled();
}

FReply SMeshDefImages::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
{
	if (Event.GetKey() == EKeys::Left)  { return Step(-1); }
	if (Event.GetKey() == EKeys::Right) { return Step(1); }

	return SCompoundWidget::OnKeyDown(Geometry, Event);
}

const FSlateBrush* SMeshDefImages::PreviewBrush() const
{
	const TSharedPtr<FMeshDefImageEntry> Entry = Current();

	if (!Entry.IsValid())
	{
		return FAppStyle::GetBrush("NoBorder");
	}

	return Entry->Large.Get();
}

FText SMeshDefImages::PreviewCaption() const
{
	const TSharedPtr<FMeshDefImageEntry> Entry = Current();
	if (!Entry.IsValid())
	{
		return FText::GetEmpty();
	}

	UTexture2D* Texture = Entry->Texture.Get();

	if (Texture == nullptr)
	{
		return Entry->Label;
	}

	// The same helper the brushes use, so the number shown and the shape drawn cannot disagree.
	const FVector2D Pixels = MeshDefImagesPrivate::PixelSize(Texture);

	return FText::Format(LOCTEXT("PreviewCaption", "{0}  -  {1} × {2}"),
		Entry->Label,
		FText::AsNumber(FMath::RoundToInt(Pixels.X)),
		FText::AsNumber(FMath::RoundToInt(Pixels.Y)));
}

FText SMeshDefImages::PreviewRole() const
{
	const TSharedPtr<FMeshDefImageEntry> Entry = Current();
	if (!Entry.IsValid())
	{
		return FText::GetEmpty();
	}

	if (Entry->bIsMain)
	{
		return LOCTEXT("RoleMain", "Main image - this is what the generator is shown");
	}

	if (Entry->ViewIndex != INDEX_NONE)
	{
		return FText::Format(LOCTEXT("RoleView", "Extra view {0}"), FText::AsNumber(Entry->ViewIndex + 1));
	}

	return LOCTEXT("RoleNone", "Not used by the generator");
}

FSlateColor SMeshDefImages::PreviewRoleColour() const
{
	const TSharedPtr<FMeshDefImageEntry> Entry = Current();

	if (Entry.IsValid() && Entry->bIsMain)
	{
		return FSlateColor(FLinearColor(0.35f, 0.75f, 0.40f));
	}

	if (Entry.IsValid() && Entry->ViewIndex != INDEX_NONE)
	{
		return FSlateColor(FLinearColor(0.40f, 0.65f, 0.95f));
	}

	return FSlateColor::UseSubduedForeground();
}

FText SMeshDefImages::PreviewPosition() const
{
	return Entries.Num() > 0
		? FText::Format(LOCTEXT("PreviewPos", "{0} of {1}"),
			FText::AsNumber(PreviewIndex + 1), FText::AsNumber(Entries.Num()))
		: FText::GetEmpty();
}

FText SMeshDefImages::MainButtonText() const
{
	const TSharedPtr<FMeshDefImageEntry> Entry = Current();

	return (Entry.IsValid() && Entry->bIsMain)
		? LOCTEXT("IsMain", "Is the main image")
		: LOCTEXT("UseAsMain", "Use as main");
}

bool SMeshDefImages::CanSetMain() const
{
	const TSharedPtr<FMeshDefImageEntry> Entry = Current();
	return Entry.IsValid() && !Entry->bIsMain;
}

FText SMeshDefImages::ViewButtonText() const
{
	const TSharedPtr<FMeshDefImageEntry> Entry = Current();

	return (Entry.IsValid() && Entry->ViewIndex != INDEX_NONE)
		? LOCTEXT("DropView", "Remove view")
		: LOCTEXT("AddView", "Add as view");
}

EVisibility SMeshDefImages::ViewButtonVisibility() const
{
	// Always offered now. Hiding it until a generator had been chosen was backwards: which pictures
	// belong together is a decision about the reference material, and it is often made *before*
	// anybody picks a generator. Whether they are read is the generator's business, and the header
	// says so plainly rather than the control disappearing.
	return EVisibility::Visible;
}

bool SMeshDefImages::CanToggleView() const
{
	const UMeshDef* Def = Definition.Get();
	const TSharedPtr<FMeshDefImageEntry> Entry = Current();

	if (Def == nullptr || !Entry.IsValid() || Entry->bIsMain)
	{
		// A picture cannot be both the subject and one of its own extra angles.
		return false;
	}

	return (Entry->ViewIndex != INDEX_NONE) || (Def->ExtraViews.Num() < MaxViewSlots);
}

bool SMeshDefImages::CarriesPictures(const FDragDropEvent& Event)
{
	if (const TSharedPtr<FExternalDragOperation> Files = Event.GetOperationAs<FExternalDragOperation>())
	{
		return Files->HasFiles();
	}

	if (const TSharedPtr<FAssetDragDropOp> Assets = Event.GetOperationAs<FAssetDragDropOp>())
	{
		for (const FAssetData& Asset : Assets->GetAssets())
		{
			if (Asset.IsInstanceOf(UTexture2D::StaticClass()))
			{
				return true;
			}
		}
	}

	return false;
}

FReply SMeshDefImages::OnDragDetected(const FGeometry& Geometry, const FPointerEvent& Event)
{
	UMeshDef* Def = Definition.Get();

	if (Def == nullptr || DragCandidate.IsNull())
	{
		return FReply::Unhandled();
	}

	return FReply::Handled().BeginDragDrop(
		FMeshForgeImageDragDropOp::New(Def, DragCandidate, DragCandidateLabel));
}

void SMeshDefImages::OnDragEnter(const FGeometry& Geometry, const FDragDropEvent& Event)
{
	bDragHovered = CarriesPictures(Event);
}

void SMeshDefImages::OnDragLeave(const FDragDropEvent& Event)
{
	bDragHovered = false;
}

FReply SMeshDefImages::OnDragOver(const FGeometry& Geometry, const FDragDropEvent& Event)
{
	return CarriesPictures(Event) ? FReply::Handled() : FReply::Unhandled();
}

FReply SMeshDefImages::OnDrop(const FGeometry& Geometry, const FDragDropEvent& Event)
{
	bDragHovered = false;

	int32 Added = 0;

	if (const TSharedPtr<FExternalDragOperation> Files = Event.GetOperationAs<FExternalDragOperation>())
	{
		Added = AddFiles(Files->GetFiles());
	}
	else if (const TSharedPtr<FAssetDragDropOp> Assets = Event.GetOperationAs<FAssetDragDropOp>())
	{
		TArray<UTexture2D*> Textures;

		for (const FAssetData& Asset : Assets->GetAssets())
		{
			if (UTexture2D* Texture = Cast<UTexture2D>(Asset.GetAsset()))
			{
				Textures.Add(Texture);
			}
		}

		Added = AddTextures(Textures);
	}

	return (Added > 0) ? FReply::Handled() : FReply::Unhandled();
}

int32 SMeshDefImages::AddFiles(const TArray<FString>& Paths)
{
	UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return 0;
	}

	// The same ingest the concept stage uses, so a dropped picture and a drawn one are the same kind
	// of thing afterwards: a real texture, in this definition's own folder, with its original pixels
	// kept in the asset so sending it back to a provider costs no quality.
	const FString Folder = FMeshImageIngest::ImageFolderFor(Def);

	TArray<UTexture2D*> Imported;

	for (const FString& Path : Paths)
	{
		const FString Extension = FPaths::GetExtension(Path, /*bIncludeDot*/ false).ToLower();

		// Whatever the ingest can decode. A dropped .txt is not an error worth a dialogue - it is
		// simply not a picture, and saying nothing is the right amount of fuss.
		if (Extension != TEXT("png") && Extension != TEXT("jpg") && Extension != TEXT("jpeg")
			&& Extension != TEXT("webp") && Extension != TEXT("bmp") && Extension != TEXT("tga"))
		{
			continue;
		}

		TArray<uint8> Bytes;

		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() == 0)
		{
			UE_LOG(LogMeshForge, Warning, TEXT("Could not read '%s'."), *Path);
			continue;
		}

		FString Error;

		UTexture2D* Texture = FMeshImageIngest::CreateTextureAsset(
			Bytes, Folder,
			FString::Printf(TEXT("T_%s_%s"), *Def->GetName(),
				*FPaths::GetBaseFilename(Path).Replace(TEXT(" "), TEXT("_"))),
			Error);

		if (Texture == nullptr)
		{
			UE_LOG(LogMeshForge, Warning, TEXT("Could not import '%s': %s"), *Path, *Error);
			continue;
		}

		Imported.Add(Texture);
	}

	return AddTextures(Imported);
}

int32 SMeshDefImages::AddTextures(const TArray<UTexture2D*>& Textures)
{
	UMeshDef* Def = Definition.Get();

	if (Def == nullptr || Textures.Num() == 0)
	{
		return 0;
	}

	const FScopedTransaction Transaction(LOCTEXT("DropImages", "Add pictures"));
	Def->Modify();

	int32 Added = 0;

	for (UTexture2D* Texture : Textures)
	{
		const int32 Before = Def->AddedImages.Num();
		Def->AddedImages.AddUnique(Texture);

		Added += (Def->AddedImages.Num() > Before) ? 1 : 0;
	}

	// The first picture a definition gets becomes its main one, exactly as with Add picture.
	if (Added > 0 && Def->ResolveMainImage().IsNull())
	{
		Def->MainImage = Def->AddedImages[0];
	}

	if (Added > 0)
	{
		Def->RefreshStaleness();
		Refresh();

		OnSelectionChanged.ExecuteIfBound();

		UE_LOG(LogMeshForge, Log, TEXT("%s: added %d picture(s) to the gallery."),
			*Def->GetName(), Added);
	}

	return Added;
}

void SMeshDefImages::OnAddImage(const FAssetData& Asset)
{
	UMeshDef* Def = Definition.Get();
	UTexture2D* Texture = Cast<UTexture2D>(Asset.GetAsset());

	if (Def == nullptr || Texture == nullptr)
	{
		return;
	}

	if (AddButton.IsValid())
	{
		AddButton->SetIsOpen(false);
	}

	const FScopedTransaction Transaction(LOCTEXT("AddImageAction", "Add picture"));
	Def->Modify();

	Def->AddedImages.AddUnique(Texture);

	// The first picture a definition gets becomes its main one. Adding a reference and then having
	// to press a second button to say "yes, use it" is a step that is right every time.
	if (Def->ResolveMainImage().IsNull())
	{
		Def->MainImage = Texture;
	}

	Def->RefreshStaleness();
	Refresh();

	OnSelectionChanged.ExecuteIfBound();
}

void SMeshDefImages::OnSetMain(TSoftObjectPtr<UTexture2D> Image)
{
	// **The rules live in one place now.** The slot strip on the generator assigns pictures too,
	// and two implementations of "a picture cannot be both the subject and one of its own angles"
	// is the kind of thing that drifts until a generation sends the same picture twice.
	MeshForgeImageSlots::AssignMain(Definition.Get(), Image);

	// The shared write tells every panel on the next tick; this one repaints at once, so the strip
	// answers the click that caused it rather than a frame later.
	Refresh();
	OnSelectionChanged.ExecuteIfBound();
}

void SMeshDefImages::OnToggleView(TSoftObjectPtr<UTexture2D> Image)
{
	const UMeshDef* Def = Definition.Get();

	if (Def == nullptr || Image.IsNull())
	{
		return;
	}

	const int32 Existing = ViewSlotOf(Image);

	if (Existing != INDEX_NONE)
	{
		MeshForgeImageSlots::ClearSlot(Definition.Get(), Existing);
	}
	else
	{
		// The first empty slot, so "add as a view" fills a hole somebody made rather than always
		// appending and leaving view 2 blank forever.
		int32 Free = INDEX_NONE;

		for (int32 Index = 0; Index < MaxViewSlots; ++Index)
		{
			if (MeshForgeImageSlots::InSlot(Def, Index).IsNull())
			{
				Free = Index;
				break;
			}
		}

		if (Free == INDEX_NONE)
		{
			return;
		}

		MeshForgeImageSlots::AssignView(Definition.Get(), Image, Free);
	}

	Refresh();
	OnSelectionChanged.ExecuteIfBound();
}

int32 SMeshDefImages::ViewSlotOf(const TSoftObjectPtr<UTexture2D>& Image) const
{
	const UMeshDef* Def = Definition.Get();
	return Def ? Def->ExtraViews.IndexOfByKey(Image) : INDEX_NONE;
}

void SMeshDefImages::OnSetView(TSoftObjectPtr<UTexture2D> Image, int32 Slot)
{
	if (Slot < 0)
	{
		// The menu offers "none" as a slot below zero, which means take it out of whichever it is in.
		const int32 Existing = ViewSlotOf(Image);

		if (Existing != INDEX_NONE)
		{
			MeshForgeImageSlots::ClearSlot(Definition.Get(), Existing);
		}
	}
	else
	{
		MeshForgeImageSlots::AssignView(Definition.Get(), Image, Slot);
	}

	Refresh();
	OnSelectionChanged.ExecuteIfBound();
}

TSharedRef<SWidget> SMeshDefImages::BuildRoleMenu(TSoftObjectPtr<UTexture2D> Image)
{
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);

	const UMeshDef* Def = Definition.Get();
	const bool bIsMain = Def && (Image == Def->ResolveMainImage());
	const int32 Slot = ViewSlotOf(Image);

	Menu.BeginSection(NAME_None, LOCTEXT("RoleSection", "This picture"));

	Menu.AddMenuEntry(
		LOCTEXT("MenuMain", "Main image"),
		LOCTEXT("MenuMainTip", "The picture the generator reconstructs from. Every model reads this one."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateSP(this, &SMeshDefImages::OnSetMain, Image),
			FCanExecuteAction::CreateLambda([bIsMain]() { return !bIsMain; }),
			FIsActionChecked::CreateLambda([bIsMain]() { return bIsMain; })),
		NAME_None, EUserInterfaceActionType::Check);

	for (int32 Index = 0; Index < MaxViewSlots; ++Index)
	{
		const bool bHere = (Slot == Index);

		Menu.AddMenuEntry(
			FText::Format(LOCTEXT("MenuView", "View {0}"), FText::AsNumber(Index + 1)),
			LOCTEXT("MenuViewTip",
				"Another angle of the same object. Only a generator that reconstructs from several "
				"pictures reads these - the line above the gallery says whether this one does."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SMeshDefImages::OnSetView, Image, Index),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([bHere]() { return bHere; })),
			NAME_None, EUserInterfaceActionType::Check);
	}

	if (bIsMain || Slot != INDEX_NONE)
	{
		Menu.AddMenuEntry(
			LOCTEXT("MenuUnused", "Not used"),
			LOCTEXT("MenuUnusedTip", "Keep it in the gallery, but do not send it to the generator."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, Slot, bIsMain]()
			{
				// Through the same function the slot strip empties a slot with, so clearing the main
				// picture is one undo step here as well - it used to Modify without a transaction.
				MeshForgeImageSlots::ClearSlot(Definition.Get(), bIsMain ? INDEX_NONE : Slot);

				Refresh();
				OnSelectionChanged.ExecuteIfBound();
			})));
	}

	Menu.EndSection();

	Menu.BeginSection(NAME_None, LOCTEXT("MenuAsset", "Asset"));

	Menu.AddMenuEntry(
		LOCTEXT("MenuFind", "Show in Content Browser"),
		FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this, Image]()
		{
			OnShowInContentBrowser(Image.LoadSynchronous());
		})));

	Menu.AddMenuEntry(
		LOCTEXT("MenuRemove", "Remove from gallery"),
		LOCTEXT("MenuRemoveTip", "Takes it out of this definition. The texture stays in the project."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SMeshDefImages::OnRemoveImage, Image)));

	Menu.EndSection();

	return Menu.MakeWidget();
}

void SMeshDefImages::OnRemoveImage(TSoftObjectPtr<UTexture2D> Image)
{
	UMeshDef* Def = Definition.Get();

	if (Def == nullptr || Image.IsNull())
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("RemoveImageAction", "Remove picture"));
	Def->Modify();

	// Every list it could be in. A picture does not know where it came from, and removing it from
	// the gallery has to mean removing it from the gallery - not from whichever list happened to be
	// checked first.
	Def->AddedImages.Remove(Image);
	Def->ConceptImages.Remove(Image);
	Def->ReferenceImages.Remove(Image);
	Def->ExtraViews.Remove(Image);

	if (Def->SourceImage == Image)
	{
		Def->SourceImage = nullptr;
	}

	if (Def->MainImage == Image)
	{
		// Cleared rather than repointed. ResolveMainImage falls back to whatever is left, and
		// choosing a replacement on somebody's behalf is how a mesh gets made from a picture nobody
		// picked.
		Def->MainImage = nullptr;
	}

	// The legacy index means a different picture now that the list is shorter, and it is only read
	// as a fallback - so it is retired here rather than left to point somewhere arbitrary.
	Def->SelectedConcept = -1;

	Def->RefreshStaleness();
	Def->MarkPackageDirty();

	Refresh();

	OnSelectionChanged.ExecuteIfBound();

	UE_LOG(LogMeshForge, Log, TEXT("%s: removed '%s' from the gallery. The texture is untouched."),
		*Def->GetName(), *MeshDefImagesPrivate::ShortName(Image));
}

void SMeshDefImages::OnShowInContentBrowser(TWeakObjectPtr<UTexture2D> Texture)
{
	if (UTexture2D* Asset = Texture.Get())
	{
		FContentBrowserModule& Browser =
			FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
		Browser.Get().SyncBrowserToAssets({ FAssetData(Asset) });
	}
}

FText SMeshDefImages::SelectionSummary() const
{
	const UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return FText::GetEmpty();
	}

	const TSoftObjectPtr<UTexture2D> Main = Def->ResolveMainImage();

	if (Main.IsNull())
	{
		return LOCTEXT("NothingChosen", "Nothing chosen yet - the mesh stage has no picture to work from.");
	}

	const int32 Allowed = Def->MaxExtraViews();
	const int32 Chosen  = Def->ExtraViews.Num();

	// **Says where the switch is rather than blaming the model.** Allowed is zero both when the
	// model cannot read extra views and when multiview_to_model is simply off in the generator's
	// settings, and telling somebody "this generator reconstructs from one picture" in the second
	// case sends them off to change generators for a setting they own.
	if (Allowed == 0 && Chosen > 0)
	{
		return FText::Format(
			LOCTEXT("SummaryMultiViewOff",
				"The generator will be shown {0} only. The {1} view(s) you have chosen are kept on "
				"the asset and are NOT sent - turn multiview_to_model on in the generator's "
				"settings, on the Mesh stage."),
			FText::FromString(MeshDefImagesPrivate::ShortName(Main)), FText::AsNumber(Chosen));
	}

	if (Chosen == 0)
	{
		return FText::Format(
			LOCTEXT("SummarySingle",
				"The generator will be shown {0}. Right-click a picture to give it a view slot."),
			FText::FromString(MeshDefImagesPrivate::ShortName(Main)));
	}

	// Said here rather than by hiding the control. Somebody who has chosen three angles and picked a
	// single-view generator needs to be told, not left with a gallery that silently ignores two of
	// them - which is the same failure as three pictures used badly.
	if (Allowed == 0)
	{
		return FText::Format(
			LOCTEXT("SummaryIgnored",
				"The generator will be shown {0}. The {1} view(s) you have chosen are kept on the "
				"asset but this generator reconstructs from one picture and will NOT read them - "
				"choose a generator with a multi-view model on the Mesh stage to use them."),
			FText::FromString(MeshDefImagesPrivate::ShortName(Main)), FText::AsNumber(Chosen));
	}

	// **The partial drop, which was the case nobody was told about.** Four views chosen against a
	// three-view model read as "3 of 3" - true, and it never mentions the fourth going nowhere. The
	// picture was paid for and the reconstruction is made without it.
	if (Chosen > Allowed)
	{
		return FText::Format(
			LOCTEXT("SummaryDropping",
				"The generator will be shown {0}, plus {1} of the {2} extra views you have chosen. "
				"The other {3} will NOT be sent - this model reads {1}. Remove them, or choose a "
				"generator that reads more."),
			FText::FromString(MeshDefImagesPrivate::ShortName(Main)),
			FText::AsNumber(Allowed), FText::AsNumber(Chosen), FText::AsNumber(Chosen - Allowed));
	}

	return FText::Format(
		LOCTEXT("SummaryMulti",
			"The generator will be shown {0}, plus {1} extra view(s). It can read up to {2}."),
		FText::FromString(MeshDefImagesPrivate::ShortName(Main)),
		FText::AsNumber(Chosen),
		FText::AsNumber(Allowed));
}

EVisibility SMeshDefImages::ContentVisibility() const
{
	return Entries.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SMeshDefImages::EmptyVisibility() const
{
	return Entries.Num() > 0 ? EVisibility::Collapsed : EVisibility::Visible;
}

#undef LOCTEXT_NAMESPACE
