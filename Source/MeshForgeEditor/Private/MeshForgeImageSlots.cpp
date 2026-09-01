#include "MeshForgeImageSlots.h"

#include "MeshDef.h"
#include "MeshForgeSubsystem.h"

#include "Containers/Ticker.h"
#include "Engine/Texture2D.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateBrush.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MeshForgeEditor"

namespace MeshForgeImageSlots
{

void AssignMain(UMeshDef* Def, const TSoftObjectPtr<UTexture2D>& Image)
{
	if (Def == nullptr || Image.IsNull())
	{
		return;
	}

	// A transaction, so this is one Ctrl+Z. Choosing the wrong picture and being unable to take it
	// back is a small annoyance that happens constantly.
	const FScopedTransaction Transaction(LOCTEXT("ChooseMain", "Choose main image"));
	Def->Modify();

	Def->MainImage = Image;

	// A picture cannot be both the subject and one of its own extra angles.
	Def->ExtraViews.Remove(Image);

	// Everything downstream was made from a different picture and must say so. Outputs are kept -
	// see FMeshStageState.
	Def->RefreshStaleness();
	Def->MarkPackageDirty();

	NotifyEdited(Def);
}

void AssignView(UMeshDef* Def, const TSoftObjectPtr<UTexture2D>& Image, int32 Slot)
{
	if (Def == nullptr || Image.IsNull() || Slot < 0 || Slot >= MaxViewSlots)
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("SetViewSlot", "Set view"));
	Def->Modify();

	// Out of whatever slot it was in first, so moving a picture from view 2 to view 1 does not
	// leave it in both. This is also what makes dragging between two slots work.
	Def->ExtraViews.Remove(Image);

	if (Image == Def->ResolveMainImage())
	{
		// It cannot be the subject and one of its own angles, so taking it as a view gives up the
		// main slot rather than silently doing half of what was asked.
		Def->MainImage = nullptr;
	}

	// Grown with blanks rather than appended, so slot 3 means slot 3 even when 2 is empty. The
	// vendor reads the first image as the front view and the order after that is ours to mean
	// something.
	while (Def->ExtraViews.Num() <= Slot && Def->ExtraViews.Num() < MaxViewSlots)
	{
		Def->ExtraViews.Add(nullptr);
	}

	if (Def->ExtraViews.IsValidIndex(Slot))
	{
		Def->ExtraViews[Slot] = Image;
	}

	// Trailing blanks are not slots anybody chose.
	while (Def->ExtraViews.Num() > 0 && Def->ExtraViews.Last().IsNull())
	{
		Def->ExtraViews.Pop();
	}

	Def->RefreshStaleness();
	Def->MarkPackageDirty();

	NotifyEdited(Def);
}

void ClearSlot(UMeshDef* Def, int32 Slot)
{
	if (Def == nullptr)
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("ClearSlot", "Clear picture slot"));
	Def->Modify();

	if (Slot < 0)
	{
		Def->MainImage = nullptr;
	}
	else if (Def->ExtraViews.IsValidIndex(Slot))
	{
		Def->ExtraViews[Slot] = nullptr;

		while (Def->ExtraViews.Num() > 0 && Def->ExtraViews.Last().IsNull())
		{
			Def->ExtraViews.Pop();
		}
	}

	Def->RefreshStaleness();
	Def->MarkPackageDirty();

	NotifyEdited(Def);
}

TSoftObjectPtr<UTexture2D> InSlot(const UMeshDef* Def, int32 Slot)
{
	if (Def == nullptr)
	{
		return TSoftObjectPtr<UTexture2D>();
	}

	if (Slot < 0)
	{
		return Def->ResolveMainImage();
	}

	return Def->ExtraViews.IsValidIndex(Slot)
		? Def->ExtraViews[Slot]
		: TSoftObjectPtr<UTexture2D>();
}

void NotifyEdited(UMeshDef* Def)
{
	if (Def == nullptr)
	{
		return;
	}

	TWeakObjectPtr<UMeshDef> Weak = Def;

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Weak](float) -> bool
		{
			UMeshDef* Live = Weak.Get();
			UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get();

			if (Live != nullptr && Subsystem != nullptr)
			{
				Subsystem->OnDefinitionEdited.Broadcast(Live);
			}

			return false;   // once
		}), 0.0f);
}

TSharedRef<FSlateBrush> MakeCroppedBrush(UTexture2D* Texture, float Size)
{
	TSharedRef<FSlateBrush> Brush = MakeShared<FSlateBrush>();

	if (Texture == nullptr)
	{
		return Brush;
	}

	Brush->SetResourceObject(Texture);
	Brush->ImageSize = FVector2D(Size, Size);
	Brush->DrawAs = ESlateBrushDrawType::Image;

	const float Width  = static_cast<float>(FMath::Max(1, Texture->GetSizeX()));
	const float Height = static_cast<float>(FMath::Max(1, Texture->GetSizeY()));

	if (Width > Height)
	{
		const float Span = Height / Width;
		Brush->SetUVRegion(FBox2f(FVector2f(0.5f - Span * 0.5f, 0.f), FVector2f(0.5f + Span * 0.5f, 1.f)));
	}
	else if (Height > Width)
	{
		const float Span = Width / Height;
		Brush->SetUVRegion(FBox2f(FVector2f(0.f, 0.5f - Span * 0.5f), FVector2f(1.f, 0.5f + Span * 0.5f)));
	}

	return Brush;
}

}   // namespace MeshForgeImageSlots

TSharedRef<FMeshForgeImageDragDropOp> FMeshForgeImageDragDropOp::New(
	UMeshDef* InDefinition, const TSoftObjectPtr<UTexture2D>& InImage, const FText& InLabel)
{
	TSharedRef<FMeshForgeImageDragDropOp> Operation = MakeShared<FMeshForgeImageDragDropOp>();

	Operation->Definition = InDefinition;
	Operation->Image      = InImage;
	Operation->Label      = InLabel;

	// Built now rather than in GetDefaultDecorator, which is const and called while the drag is
	// already under way.
	Operation->Thumbnail = MeshForgeImageSlots::MakeCroppedBrush(
		InImage.IsNull() ? nullptr : InImage.LoadSynchronous(), 56.0f);

	Operation->Construct();

	return Operation;
}

TSharedPtr<SWidget> FMeshForgeImageDragDropOp::GetDefaultDecorator() const
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(56.0f)
				.HeightOverride(56.0f)
				[
					SNew(SImage).Image(Thumbnail.Get())
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(STextBlock).Text(Label)
			]
		];
}

#undef LOCTEXT_NAMESPACE
