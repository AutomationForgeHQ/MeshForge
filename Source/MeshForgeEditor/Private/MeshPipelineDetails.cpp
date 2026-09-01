#include "MeshPipelineDetails.h"

#include "MeshDef.h"
#include "MeshForgePipeline.h"
#include "MeshForgeImageSlots.h"
#include "SMeshForgeImageSlot.h"

#include "Containers/Ticker.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IPropertyUtilities.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MeshForge"

namespace MeshPipelineDetailsPrivate
{
	static constexpr float SlotSize = 84.0f;
}

TSharedRef<IDetailCustomization> FMeshPipelineDetails::MakeInstance()
{
	return MakeShared<FMeshPipelineDetails>();
}

FMeshPipelineDetails::~FMeshPipelineDetails()
{
	if (PropertyChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
	}
}

void FMeshPipelineDetails::OnAnyPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	// Only this pipeline and the definition it belongs to. The delegate is global, so anything else
	// changing a property in the editor would otherwise rebuild this layout constantly.
	const bool bMine = (Object != nullptr)
		&& (Object == Pipeline.Get() || Object == Definition.Get());

	if (!bMine || !Utilities.IsValid())
	{
		return;
	}

	// Deferred by a tick rather than rebuilt inside the broadcast: tearing down the layout that is
	// currently dispatching this event destroys the widget the change came from.
	TWeakPtr<IPropertyUtilities> Weak = Utilities;

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Weak](float) -> bool
		{
			if (const TSharedPtr<IPropertyUtilities> Live = Weak.Pin())
			{
				Live->ForceRefresh();
			}

			return false;   // once
		}), 0.0f);
}

void FMeshPipelineDetails::CustomizeDetails(IDetailLayoutBuilder& Builder)
{
	TArray<TWeakObjectPtr<UObject>> Customized;
	Builder.GetObjectsBeingCustomized(Customized);

	if (Customized.Num() != 1)
	{
		// Multi-select would have to show one strip per definition, and a pipeline is edited one at
		// a time from the stage panel. Nothing is added rather than something misleading.
		return;
	}

	const UMeshForgePipeline* Pipeline_ = Cast<UMeshForgePipeline>(Customized[0].Get());

	if (Pipeline_ == nullptr || Pipeline_->GetKind() != EMeshPipelineKind::Mesh)
	{
		return;
	}

	// The definition this pipeline belongs to. A pipeline created from the stage panel is outered to
	// it; one made as a standalone asset in the Content Browser has no definition and no pictures to
	// show, which is a perfectly ordinary state.
	UMeshDef* Def = Pipeline_->GetTypedOuter<UMeshDef>();

	if (Def == nullptr)
	{
		return;
	}

	// Kept so a later change can rebuild this layout. Registered once - CustomizeDetails runs again
	// on every refresh, and a delegate added each time would multiply until the editor crawled.
	Pipeline   = Pipeline_;
	Definition = Def;
	Utilities  = Builder.GetPropertyUtilities();

	if (!PropertyChangedHandle.IsValid())
	{
		PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(
			this, &FMeshPipelineDetails::OnAnyPropertyChanged);
	}

	IDetailCategoryBuilder& Category =
		Builder.EditCategory(TEXT("Input"), LOCTEXT("InputCategory", "Input"));

	// **The pipeline answers first, and the definition answers for the pipelines that defer.**
	// GetMaxExtraViews returns -1 for a pipeline whose vendor decides - Meshy's did, until it grew
	// a switch of its own - and reading that as a count drew one slot for a model that reads four.
	// The definition resolves the same question through the provider.
	int32 Views = Pipeline_->GetMaxExtraViews();

	if (Views < 0)
	{
		Views = Def->MaxExtraViews();
	}

	Views = FMath::Clamp(Views, 0, MeshForgeImageSlots::MaxViewSlots);

	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);

	const bool bLive = !Def->IsBusy();

	Row->AddSlot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
	[
		SNew(SMeshForgeImageSlot)
		.Definition(Def)
		.SlotIndex(INDEX_NONE)
		.Size(MeshPipelineDetailsPrivate::SlotSize)
		.AcceptsDrops(bLive)
	];

	// **Only as many slots as this pipeline will actually read.** Four against a model that takes one
	// is the same silence the rest of this panel exists to remove - it invites somebody to fill three
	// slots that go nowhere.
	for (int32 View = 0; View < Views; ++View)
	{
		Row->AddSlot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SMeshForgeImageSlot)
			.Definition(Def)
			.SlotIndex(View)
			.Size(MeshPipelineDetailsPrivate::SlotSize)
			.AcceptsDrops(bLive)
		];
	}

	Category.AddCustomRow(LOCTEXT("PicturesFilter", "pictures"))
		.WholeRowContent()
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(Views > 0
					? LOCTEXT("PicturesMulti",
						"The pictures this model will be sent. Drag them here from the Images tab, "
						"or drag one slot onto another to move it.")
					: LOCTEXT("PicturesSingle",
						"The picture this model will be sent. Drag it here from the Images tab."))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				Row
			]
		];
}

#undef LOCTEXT_NAMESPACE
