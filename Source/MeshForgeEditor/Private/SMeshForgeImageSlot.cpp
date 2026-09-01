#include "SMeshForgeImageSlot.h"

#include "MeshDef.h"
#include "MeshForgeImageSlots.h"

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IContentBrowserSingleton.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateBrush.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MeshForgeEditor"

void SMeshForgeImageSlot::Construct(const FArguments& InArgs)
{
	Definition = InArgs._Definition;
	SlotIndex  = InArgs._SlotIndex;
	Size       = InArgs._Size;
	bAcceptsDrops = InArgs._AcceptsDrops;

	Held = MeshForgeImageSlots::InSlot(Definition.Get(), SlotIndex);

	UTexture2D* Texture = Held.IsNull() ? nullptr : Held.LoadSynchronous();

	Thumbnail = MeshForgeImageSlots::MakeCroppedBrush(Texture, Size);

	// **An empty slot says which kind of empty it is.** A missing main image is a refusal; a missing
	// view is a smaller request. Blank squares read as the same thing and are not.
	const FText Empty = IsMain()
		? LOCTEXT("SlotNoMain", "drag a picture here\n(required)")
		: LOCTEXT("SlotNoView", "drag a picture here");

	const FText Tip = Texture
		? FText::Format(LOCTEXT("SlotFilledTip",
			"{0}: {1}\n\nDrag another picture here to replace it, or drag this one onto a different "
			"slot. Right-click to empty the slot."),
			Caption(), FText::FromString(Texture->GetName()))
		: FText::Format(LOCTEXT("SlotEmptyTip",
			"{0} is not chosen. Drag a picture from the Images tab onto this slot - or right-click "
			"it there and give it a role."), Caption());

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(this, &SMeshForgeImageSlot::Border)
			.BorderBackgroundColor(this, &SMeshForgeImageSlot::BorderColour)
			.Padding(3.0f)
			.ToolTipText(Tip)
			[
				SNew(SBox)
				.WidthOverride(Size)
				.HeightOverride(Size)
				[
					Texture
						? StaticCastSharedRef<SWidget>(SNew(SImage).Image(Thumbnail.Get()))
						: StaticCastSharedRef<SWidget>(
							SNew(STextBlock)
							.Text(Empty)
							.Justification(ETextJustify::Center)
							.AutoWrapText(true)
							.ColorAndOpacity(IsMain()
								? FSlateColor(FLinearColor(0.9f, 0.45f, 0.35f))
								: FSlateColor::UseSubduedForeground()))
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
		[
			SNew(STextBlock)
			.Text(Caption())
			.ColorAndOpacity(Texture
				? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground())
		]
	];
}

FText SMeshForgeImageSlot::Caption() const
{
	return IsMain()
		? LOCTEXT("SlotMain", "main image")
		: FText::Format(LOCTEXT("SlotView", "view {0}"), FText::AsNumber(SlotIndex + 1));
}

const FSlateBrush* SMeshForgeImageSlot::Border() const
{
	return FAppStyle::GetBrush(IsMain()
		? TEXT("ToolPanel.GroupBorder") : TEXT("ToolPanel.DarkGroupBorder"));
}

FSlateColor SMeshForgeImageSlot::BorderColour() const
{
	// The same blue an extra view is marked with in the gallery, so "this is where it will land"
	// and "this is a view" are visibly the same idea.
	return bDropHovered
		? FSlateColor(FLinearColor(0.40f, 0.65f, 0.95f))
		: FSlateColor(FLinearColor::White);
}

FReply SMeshForgeImageSlot::ShowSlotMenu(const FPointerEvent& Event)
{
	const int32 Index = SlotIndex;
	TWeakObjectPtr<UMeshDef> Weak = Definition;
	TSoftObjectPtr<UTexture2D> Picture = Held;

	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);

	Menu.AddMenuEntry(
		LOCTEXT("ClearSlotLabel", "Empty this slot"),
		LOCTEXT("ClearSlotTip",
			"Take this picture out of the slot. It stays in the gallery - this only says the "
			"generator will not be shown it."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([Weak, Index]()
		{
			MeshForgeImageSlots::ClearSlot(Weak.Get(), Index);
		})));

	Menu.AddMenuEntry(
		LOCTEXT("SlotShowLabel", "Show in Content Browser"),
		LOCTEXT("SlotShowTip", "Find this texture in the project."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([Picture]()
		{
			if (UTexture2D* Asset = Picture.LoadSynchronous())
			{
				FContentBrowserModule& Browser =
					FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
				Browser.Get().SyncBrowserToAssets({ FAssetData(Asset) });
			}
		})));

	FSlateApplication::Get().PushMenu(
		AsShared(), FWidgetPath(), Menu.MakeWidget(),
		FVector2D(Event.GetScreenSpacePosition()),
		FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));

	return FReply::Handled();
}

FReply SMeshForgeImageSlot::OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
	// Handled here rather than on the border inside, so there is one place that decides what a
	// click on a slot means and no dependence on an unbound child staying transparent.
	if (Held.IsNull())
	{
		return FReply::Unhandled();
	}

	if (Event.GetEffectingButton() == EKeys::RightMouseButton)
	{
		return ShowSlotMenu(Event);
	}

	// Arms the drag rather than starting it, so a plain click still does nothing surprising.
	if (Event.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		return FReply::Handled().DetectDrag(SharedThis(this), EKeys::LeftMouseButton);
	}

	return FReply::Unhandled();
}

FReply SMeshForgeImageSlot::OnDragDetected(const FGeometry& Geometry, const FPointerEvent& Event)
{
	UMeshDef* Def = Definition.Get();

	if (Def == nullptr || Held.IsNull())
	{
		return FReply::Unhandled();
	}

	return FReply::Handled().BeginDragDrop(
		FMeshForgeImageDragDropOp::New(Def, Held, Caption()));
}

bool SMeshForgeImageSlot::WillTake(const FDragDropEvent& Event) const
{
	if (!bAcceptsDrops)
	{
		return false;
	}

	const TSharedPtr<FMeshForgeImageDragDropOp> Operation =
		Event.GetOperationAs<FMeshForgeImageDragDropOp>();

	// Same definition, and not the picture already sitting here - dropping a picture back where it
	// came from should be a no-op rather than a transaction on the undo stack.
	return Operation.IsValid()
		&& Operation->Definition.Get() != nullptr
		&& Operation->Definition.Get() == Definition.Get()
		&& !Operation->Image.IsNull()
		&& Operation->Image != Held;
}

void SMeshForgeImageSlot::OnDragEnter(const FGeometry& Geometry, const FDragDropEvent& Event)
{
	bDropHovered = WillTake(Event);
}

void SMeshForgeImageSlot::OnDragLeave(const FDragDropEvent& Event)
{
	bDropHovered = false;
}

FReply SMeshForgeImageSlot::OnDragOver(const FGeometry& Geometry, const FDragDropEvent& Event)
{
	return WillTake(Event) ? FReply::Handled() : FReply::Unhandled();
}

FReply SMeshForgeImageSlot::OnDrop(const FGeometry& Geometry, const FDragDropEvent& Event)
{
	bDropHovered = false;

	if (!WillTake(Event))
	{
		return FReply::Unhandled();
	}

	const TSharedPtr<FMeshForgeImageDragDropOp> Operation =
		Event.GetOperationAs<FMeshForgeImageDragDropOp>();

	UMeshDef* Def = Definition.Get();

	// The write happens now; the panels are told on the next tick. Rebuilding the strip from inside
	// the drop handler would destroy this widget while it is still dispatching.
	if (IsMain())
	{
		MeshForgeImageSlots::AssignMain(Def, Operation->Image);
	}
	else
	{
		MeshForgeImageSlots::AssignView(Def, Operation->Image, SlotIndex);
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
