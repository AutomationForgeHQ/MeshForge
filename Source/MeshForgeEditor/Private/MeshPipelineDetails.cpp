#include "MeshPipelineDetails.h"

#include "MeshDef.h"
#include "MeshForgePipeline.h"
#include "MeshPostPipeline.h"
#include "MeshForgeImageSlots.h"
#include "SMeshForgeImageSlot.h"

#include "Containers/Ticker.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailPropertyRow.h"
#include "PropertyHandle.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "IPropertyUtilities.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MeshForge"

namespace MeshPipelineDetailsPrivate
{
	static constexpr float SlotSize = 84.0f;

	/**
	 * The categories that come before whatever the step itself declares, in this order: what a
	 * generator builds from, then the words it is given.
	 *
	 * **What goes in is read before how it is processed**, and a generator that can build from words
	 * or from pictures has to say which before either is shown.
	 */
	static const TCHAR* const HeadCategories[] = {
		TEXT("Generation"),
		TEXT("Prompt"),
	};

	/**
	 * The categories that come after whatever the step itself declares, in this order.
	 *
	 * **Only the ends are named, because only the ends were wrong.** Unreal decides category order
	 * itself and gets a step's own settings right - the wardrobe step drew Wardrobe above Inpainting,
	 * which is both the order they are declared and the order they are read. What it got wrong was
	 * putting Advanced, a lone "Sculpt in pose..." button, *above* the settings that button operates
	 * on. Naming every category instead would mean this list had to know what every step in the
	 * family called its own settings, and would quietly reorder one it had never heard of.
	 */
	static const TCHAR* const TailCategories[] = {
		TEXT("Advanced"),
		TEXT("Input"),
		TEXT("Pipeline"),
	};

	/**
	 * Where a mesh generator's Input goes instead: straight after the head, where the prompt would be.
	 *
	 * **The pictures a generator is sent are the first thing anybody checks**, and they sat below every
	 * texture and shape setting. A post step's Input - which mesh it works on - keeps its place in the
	 * tail; only the generator's moves.
	 */
	static const TCHAR* const GeneratorInputCategory = TEXT("Input");

	/** Below and far above anything Unreal hands out, so pinning either end cannot collide with a category left alone. */
	static constexpr int32 HeadSortBase = -10000;
	static constexpr int32 TailSortBase = 10000;
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

	const UMeshForgePipeline* Pipeline_ = Customized.Num() == 1 ? Cast<UMeshForgePipeline>(Customized[0].Get()) : nullptr;
	const bool bGenerator = Pipeline_ != nullptr && Pipeline_->GetKind() == EMeshPipelineKind::Mesh;

	Builder.SortCategories([bGenerator](const TMap<FName, IDetailCategoryBuilder*>& Categories)
	{
		auto Pin = [&Categories](FName Name, int32 Sort)
		{
			if (IDetailCategoryBuilder* const* Category = Categories.Find(Name))
			{
				(*Category)->SetSortOrder(Sort);
			}
		};

		int32 Sort = MeshPipelineDetailsPrivate::HeadSortBase;
		for (const TCHAR* Name : MeshPipelineDetailsPrivate::HeadCategories)
		{
			Pin(FName(Name), Sort);
			Sort += 10;
		}

		Sort = MeshPipelineDetailsPrivate::TailSortBase;
		for (const TCHAR* Name : MeshPipelineDetailsPrivate::TailCategories)
		{
			Pin(FName(Name), Sort);
			Sort += 10;
		}

		// Pinned last so it overrides the tail's claim on the same name.
		if (bGenerator)
		{
			Pin(FName(MeshPipelineDetailsPrivate::GeneratorInputCategory), MeshPipelineDetailsPrivate::HeadSortBase + 1000);
		}
	});

	if (Customized.Num() != 1)
	{
		// Multi-select would have to show one strip per definition, and a pipeline is edited one at
		// a time from the stage panel. Nothing is added rather than something misleading.
		return;
	}

	// A post step's source step and run are ids, meaningless as text boxes; the stage panel draws them as
	// a step list and a run list. Hidden here only - agents still set them through the property tools.
	if (Cast<UMeshPostPipeline>(Pipeline_) != nullptr)
	{
		Builder.HideProperty(GET_MEMBER_NAME_CHECKED(UMeshPostPipeline, InputStepId), UMeshPostPipeline::StaticClass());
		Builder.HideProperty(GET_MEMBER_NAME_CHECKED(UMeshPostPipeline, InputTakeId), UMeshPostPipeline::StaticClass());
	}

	// **The prompt is shown where it is read, first, and given room.** Every pipeline has one (it lives on
	// the base class), but only one that works from words reads it: an image pipeline that draws, a mesh
	// generator whose provider takes text. Elsewhere it is hidden rather than left as a box that does
	// nothing. A retexture's words are its own Style Prompt, not this.
	const TSharedRef<IPropertyHandle> PromptHandle =
		Builder.GetProperty(GET_MEMBER_NAME_CHECKED(UMeshForgePipeline, Prompt), UMeshForgePipeline::StaticClass());

	if (Pipeline_ == nullptr || !Pipeline_->ReadsPrompt())
	{
		Builder.HideProperty(PromptHandle);
	}
	else
	{
		const bool bDraws = Pipeline_->GetKind() != EMeshPipelineKind::Mesh;
		const FText Hint = bDraws
			? LOCTEXT("ImagePromptHint",
				"What to draw. Describe the object, not the picture: \"a dented steel ammunition crate with rope "
				"handles\" is a prop; \"a photo of a crate on white, studio lighting\" is a photograph of one - "
				"and the second puts the studio in the mesh.")
			: Pipeline_->GetInputMode() == EMeshPipelineInput::Text
			? LOCTEXT("MeshTextPromptHint",
				"What to build, in words - the only thing this generator is sent. Describe the object: its "
				"shape, what it is made of, its condition and scale. Not a picture of it.")
			: LOCTEXT("MeshPromptHint",
				"What to build, in words, for a generator that reads text: alone when there is no picture, or "
				"beside one where the generator takes both. Describe the object, not a picture of it.");

		IDetailCategoryBuilder& PromptCategory =
			Builder.EditCategory(TEXT("Prompt"), LOCTEXT("PromptCategory", "Prompt"), ECategoryPriority::Important);

		// Committed on focus loss, so a rewrite is one undo rather than forty; through the handle, so it is
		// transacted and the stage panel hears about it like any other setting.
		PromptCategory.AddProperty(PromptHandle).CustomWidget()
			.WholeRowContent()
			[
				SNew(SBox)
				.MinDesiredHeight(72.0f)
				.Padding(FMargin(0.0f, 4.0f))
				[
					SNew(SMultiLineEditableTextBox)
					.AutoWrapText(true)
					.HintText(Hint)
					.Text_Lambda([PromptHandle]()
					{
						FString Value;
						PromptHandle->GetValue(Value);
						return FText::FromString(Value);
					})
					.OnTextCommitted_Lambda([PromptHandle](const FText& Text, ETextCommit::Type)
					{
						FString Value;
						PromptHandle->GetValue(Value);
						if (Value != Text.ToString())
						{
							PromptHandle->SetValue(Text.ToString());
						}
					})
				]
			];
	}

	if (!bGenerator)
	{
		return;
	}

	// The definition this pipeline belongs to. A pipeline created from the stage panel is outered to
	// it; one made as a standalone asset in the Content Browser has no definition and no pictures to
	// show, which is a perfectly ordinary state.
	UMeshDef* Def = Pipeline_->GetTypedOuter<UMeshDef>();

	// Kept so a later change can rebuild this layout. Registered once - CustomizeDetails runs again
	// on every refresh, and a delegate added each time would multiply until the editor crawled. Before
	// the definition is required, because switching a standalone generator between words and pictures
	// has to swap the prompt for the pictures there too.
	Pipeline   = Pipeline_;
	Definition = Def;
	Utilities  = Builder.GetPropertyUtilities();

	if (!PropertyChangedHandle.IsValid())
	{
		PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(
			this, &FMeshPipelineDetails::OnAnyPropertyChanged);
	}

	// **A generator set to words is shown no pictures at all**, nor anything in its Input, such as a
	// multi-view switch: none of it would be sent, and a strip of pictures beside a text request reads
	// as though they were.
	if (!Pipeline_->ReadsImages())
	{
		Builder.HideCategory(TEXT("Input"));
		return;
	}

	if (Def == nullptr)
	{
		return;
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
