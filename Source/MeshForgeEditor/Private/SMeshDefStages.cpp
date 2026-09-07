#include "SMeshDefStages.h"

#include "MeshDef.h"
#include "MeshForgePipeline.h"
#include "MeshImagePipeline.h"
#include "MeshForgeSubsystem.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "PropertyCustomizationHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/MessageDialog.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Input/SComboButton.h"
#include "UObject/UObjectIterator.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Images/SThrobber.h"
#include "Styling/SlateTypes.h"
#include "ScopedTransaction.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MeshForgeEditor"

namespace MeshDefStagesUI
{
	static const EMeshStage Order[] = {
		EMeshStage::Concept, EMeshStage::References, EMeshStage::Mesh,
		EMeshStage::Post,    EMeshStage::Import,
	};

	/** One pipeline's name, or a phrase saying there isn't one. */
	static FText Describe(const UMeshForgePipeline* Pipeline, const FText& NoneText)
	{
		if (Pipeline == nullptr)
		{
			return NoneText;
		}

		return Pipeline->bEnabled
			? FText::FromString(Pipeline->GetClass()->GetDisplayNameText().ToString())
			: FText::Format(LOCTEXT("PipelineOff", "{0} (off)"),
				Pipeline->GetClass()->GetDisplayNameText());
	}

	template <typename PipelineType>
	static FText Describe(const TArray<TObjectPtr<PipelineType>>& Pipelines, const FText& NoneText)
	{
		TArray<FString> Names;
		for (const TObjectPtr<PipelineType>& Pipeline : Pipelines)
		{
			if (Pipeline && Pipeline->bEnabled)
			{
				Names.Add(Pipeline->GetClass()->GetDisplayNameText().ToString());
			}
		}

		return Names.Num() == 0
			? NoneText
			: FText::FromString(FString::Join(Names, TEXT("  →  ")));
	}
}

void SMeshDefStages::Construct(const FArguments& InArgs)
{
	Definition  = InArgs._Definition;
	OnRunStage  = InArgs._OnRunStage;

	ChildSlot
	[
		SNew(SVerticalBox)

		// The prompt sits above the stages, outside the scroll area, because it is the thing being
		// edited while the stages below are being read.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(12.0f, 12.0f, 12.0f, 0.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PromptHeading", "Prompt"))
				.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox)
				.MinDesiredHeight(84.0f)
				[
					SAssignNew(PromptBox, SMultiLineEditableTextBox)
					.Text(this, &SMeshDefStages::GetPrompt)
					.OnTextCommitted(this, &SMeshDefStages::OnPromptCommitted)
					.AutoWrapText(true)
					.HintText(LOCTEXT("PromptHint",
						"Describe the object, not the picture. \"A dented steel ammunition crate "
						"with rope handles\" is a prop; \"a photo of a crate on white, studio "
						"lighting\" is a photograph of one - and the second puts the studio in the "
						"mesh."))
				]
			]
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			.Padding(12.0f)
			[
				SAssignNew(Rows, SVerticalBox)
			]
		]
	];

	Refresh();
}

void SMeshDefStages::Refresh()
{
	if (!Rows.IsValid())
	{
		return;
	}

	if (UMeshDef* Def = Definition.Get())
	{
		// Recomputed on every refresh rather than only when something is edited, because a pipeline
		// asset this definition points at can be changed from somewhere else entirely - and a stage
		// that went stale while the panel was open should say so when the panel comes back.
		Def->RefreshStaleness();
	}

	Rows->ClearChildren();

	for (const EMeshStage Stage : MeshDefStagesUI::Order)
	{
		Rows->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			BuildStageRow(Stage)
		];
	}
}

TSharedRef<SWidget> SMeshDefStages::BuildStageRow(EMeshStage Stage)
{
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

	Body->AddSlot().AutoHeight()
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(StageName(Stage))
			.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(this, &SMeshDefStages::StatusText, Stage)
			.ColorAndOpacity(this, &SMeshDefStages::StatusColour, Stage)
		]
	];

	// Bound rather than baked. This reads the pipeline's own settings, and the pipeline is edited
	// in this very row.
	Body->AddSlot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
	[
		SNew(STextBlock)
		.Text(this, &SMeshDefStages::StageSummary, Stage)
		.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		.AutoWrapText(true)
	];

	// A mesh somebody already has, offered here rather than only in the Settings tab. It overrides
	// this whole stage, so the place to say so is the stage it overrides.
	if (Stage == EMeshStage::Mesh)
	{
		Body->AddSlot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("SourceMeshLabel", "Or start from a mesh you have:"))
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(UStaticMesh::StaticClass())
				.AllowClear(true)
				.DisplayUseSelected(true)
				.DisplayBrowse(true)
				.ToolTipText(LOCTEXT("SourceMeshTip",
					"Set this and nothing is generated: the concept, reference and mesh stages have "
					"nothing to do, and the definition becomes a way of running post-processing over "
					"a mesh that already exists. Clear it to go back to generating."))
				.ObjectPath_Lambda([this]()
				{
					const UMeshDef* Def = Definition.Get();
					return Def ? Def->SourceMesh.ToString() : FString();
				})
				.OnObjectChanged_Lambda([this](const FAssetData& Asset)
				{
					UMeshDef* Def = Definition.Get();
					if (Def == nullptr)
					{
						return;
					}

					const FScopedTransaction Transaction(
						LOCTEXT("SetSourceMesh", "Set source mesh"));
					Def->Modify();

					Def->SourceMesh = Cast<UStaticMesh>(Asset.GetAsset());

					// Everything downstream was made from something else now.
					Def->RefreshStaleness();
					Refresh();
				})
			]
		];
	}

	// Post takes a chain rather than one pipeline, so it gets its own control: a prop is often
	// assembled from several vendors and choosing a step must not discard the one before it.
	TSharedRef<SWidget> Picker = (Stage == EMeshStage::Post)
		? BuildPostChain()
		: BuildPipelinePicker(Stage);

	if (Picker != SNullWidget::NullWidget)
	{
		Body->AddSlot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)[ Picker ];
	}

	// **Last, and across the whole width.** Everything above this line is what the stage is set to
	// do; everything in the bar is about doing it. The cost line lives there too rather than up
	// beside the summary - the number somebody checks before spending belongs next to the thing
	// they press, not four settings above it.
	Body->AddSlot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
	[
		BuildActionBar(Stage)
	];

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(12.0f, 10.0f))
		[
			Body
		];
}

TSharedRef<SWidget> SMeshDefStages::BuildActionBar(EMeshStage Stage)
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.Padding(FMargin(10.0f, 8.0f))
		[
			SNew(SHorizontalBox)

			// The only part of the bar that is not text, and it is here because "is it running" is
			// the one state people check by glancing rather than by reading. The elapsed seconds
			// stay on the status line by the stage name.
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SCircularThrobber)
				.Radius(9.0f)
				.Visibility(this, &SMeshDefStages::BusyVisibility, Stage)
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(this, &SMeshDefStages::ActionMessage, Stage)
					.ColorAndOpacity(this, &SMeshDefStages::ActionMessageColour, Stage)
					.AutoWrapText(true)
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(this, &SMeshDefStages::CostText, Stage)
					.Visibility(this, &SMeshDefStages::CostVisibility, Stage)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
			]

			// Given a floor width so that Draw, Generate, Run and Import line up down the column
			// instead of each button being as wide as its own word.
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				.Padding(12.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.MinDesiredWidth(112.0f)
				[
					SNew(SButton)
					.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton"))
					.TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("PrimaryButtonText"))
					.HAlign(HAlign_Center)
					.ContentPadding(FMargin(10.0f, 4.0f))
					.Text(RunLabel(Stage))
					.ToolTipText(this, &SMeshDefStages::CostText, Stage)
					.Visibility(this, &SMeshDefStages::RunVisibility, Stage)
					.OnClicked(this, &SMeshDefStages::OnRunClicked, Stage)
				]
			]
		];
}

FText SMeshDefStages::ActionMessage(EMeshStage Stage) const
{
	const FText Blocked = BlockedReason(Stage);

	return Blocked.IsEmpty() ? ActionHint(Stage) : Blocked;
}

FText SMeshDefStages::ActionHint(EMeshStage Stage) const
{
	switch (Stage)
	{
	case EMeshStage::Concept:
		return LOCTEXT("HintConcept",
			"Draws a picture from the prompt and adds it to the Images tab. It does not replace the "
			"pictures already there, so drawing again gives you another one to choose from.");

	case EMeshStage::References:
		return LOCTEXT("HintRefs",
			"Runs the refinement pipelines over the pictures this definition already has, and adds "
			"what they make to the Images tab.");

	case EMeshStage::Mesh:
		return LOCTEXT("HintMesh",
			"Sends the pictures shown above to the generator and files what comes back as a new "
			"take. Nothing appears in the project until a take is imported.");

	case EMeshStage::Post:
		return LOCTEXT("HintPost",
			"Run chain executes enabled steps using their chosen inputs. Run this step uses an existing "
			"input without running earlier steps. Every completed step creates a separate output.");

	default:
		return LOCTEXT("HintImport",
			"Brings the chosen take into the project as a static mesh, with the finish settings "
			"shown above. Importing again makes a new take rather than overwriting this one.");
	}
}

FSlateColor SMeshDefStages::ActionMessageColour(EMeshStage Stage) const
{
	if (IsRunningNow(Stage))
	{
		// The same blue the status line uses for running, so the two agree at a glance.
		return FSlateColor(FLinearColor(0.40f, 0.65f, 0.95f));
	}

	// A refusal is read at full strength; a description of what the button would do is not. They
	// share a line, so weight is what separates "you cannot yet" from "here is what happens".
	return CanRun(Stage)
		? FSlateColor::UseSubduedForeground()
		: FSlateColor::UseForeground();
}

bool SMeshDefStages::IsRunningNow(EMeshStage Stage) const
{
	const UMeshDef* Def = Definition.Get();
	const UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get();

	return Def != nullptr && Subsystem != nullptr && Subsystem->IsStageRunning(Def, Stage);
}

EVisibility SMeshDefStages::RunVisibility(EMeshStage Stage) const
{
	// Collapsed rather than disabled. A greyed-out button invites the click that does nothing; the
	// sentence that takes its place is the answer to why, and it needs the room.
	return CanRun(Stage) ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SMeshDefStages::BusyVisibility(EMeshStage Stage) const
{
	return IsRunningNow(Stage) ? EVisibility::Visible : EVisibility::Collapsed;
}

TArray<UClass*> SMeshDefStages::PipelineClassesFor(EMeshStage Stage) const
{
	UClass* Base = nullptr;
	EMeshPipelineKind Wanted = EMeshPipelineKind::Image;

	switch (Stage)
	{
	case EMeshStage::Concept:
		// Typed rather than filtered by kind, so a pipeline that cannot draw cannot be offered here
		// at all - the class is the guarantee, not a runtime check somebody can forget.
		Base = UMeshImagePipeline::StaticClass();
		break;

	case EMeshStage::Mesh:
		Base   = UMeshForgePipeline::StaticClass();
		Wanted = EMeshPipelineKind::Mesh;
		break;

	case EMeshStage::Post:
		// Typed, like the concept stage. A class that cannot process a mesh cannot be offered here
		// at all, which is a stronger guarantee than a runtime check somebody can forget.
		Base   = UMeshPostPipeline::StaticClass();
		Wanted = EMeshPipelineKind::Post;
		break;

	default:
		return {};
	}

	TArray<UClass*> Found;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;

		if (!Class->IsChildOf(Base) || Class == Base)
		{
			continue;
		}

		// Abstract intermediates - the shared base two Meshy modes derive from, say - are real
		// classes with real properties and would look pickable. They are not.
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}

		if (const UMeshForgePipeline* Default = Class->GetDefaultObject<UMeshForgePipeline>())
		{
			if (Default->GetKind() == Wanted)
			{
				Found.Add(Class);
			}
		}
	}

	Found.Sort([](const UClass& A, const UClass& B)
	{
		return A.GetDisplayNameText().CompareTo(B.GetDisplayNameText()) < 0;
	});

	return Found;
}

UMeshForgePipeline* SMeshDefStages::PipelineFor(EMeshStage Stage) const
{
	const UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return nullptr;
	}

	switch (Stage)
	{
	case EMeshStage::Concept: return Def->ConceptPipeline;
	case EMeshStage::Mesh:    return Def->MeshPipeline;
	default:                  return nullptr;
	}
}

void SMeshDefStages::SetPipeline(EMeshStage Stage, UClass* Class)
{
	UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("ChoosePipeline", "Choose pipeline"));
	Def->Modify();

	// Outered to the definition, which is what makes it a subobject saved inside this asset rather
	// than a shared one. A pipeline somebody wants to share across forty definitions is a separate
	// asset in the Content Browser, and both work - this is the one that needs no ceremony.
	UMeshForgePipeline* Instance = Class
		? NewObject<UMeshForgePipeline>(Def, Class, NAME_None, RF_Transactional)
		: nullptr;

	switch (Stage)
	{
	case EMeshStage::Concept:
		Def->ConceptPipeline = Cast<UMeshImagePipeline>(Instance);
		break;

	case EMeshStage::Mesh:
		Def->MeshPipeline = Instance;
		break;

	default:
		break;
	}

	// Changing what a stage would do changes what everything after it was made from.
	Def->RefreshStaleness();
	Refresh();
}

void SMeshDefStages::AddPostStep(UClass* Class)
{
	UMeshDef* Def = Definition.Get();

	if (Def == nullptr || Class == nullptr)
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("AddPostStep", "Add post-processing step"));
	Def->Modify();

	// Outered to the definition, which is what makes it a subobject saved inside this asset rather
	// than a shared one. A step somebody wants across forty definitions is a separate asset in the
	// Content Browser, and both work - this is the one that needs no ceremony.
	if (UMeshPostPipeline* Step =
			NewObject<UMeshPostPipeline>(Def, Class, NAME_None, RF_Transactional))
	{
		Def->PostPipelines.Add(Step);
	}

	Def->RefreshStaleness();
	Refresh();
}

void SMeshDefStages::RemovePostStep(int32 Index)
{
	UMeshDef* Def = Definition.Get();

	if (Def == nullptr || !Def->PostPipelines.IsValidIndex(Index))
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("RemovePostStep", "Remove post-processing step"));
	Def->Modify();

	Def->PostPipelines.RemoveAt(Index);

	// Dropped rather than re-indexed. The views are keyed by position and the positions have just
	// moved; keeping them would show step two's settings under step one's heading.
	PostDetails.Reset();

	Def->RefreshStaleness();
	Refresh();
}

TSharedRef<SWidget> SMeshDefStages::BuildPostChain()
{
	UMeshDef* Def = Definition.Get();

	if (Def == nullptr)
	{
		return SNullWidget::NullWidget;
	}

	const TArray<UClass*> Classes = PipelineClassesFor(EMeshStage::Post);

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	// One row per step: what it is, its settings, and a way to take it out again. Ordered, because
	// order changes the result - unwrapping before retopology throws the UVs away.
	for (int32 Index = 0; Index < Def->PostPipelines.Num(); ++Index)
	{
		UMeshPostPipeline* Step = Def->PostPipelines[Index];

		if (Step == nullptr)
		{
			continue;
		}

		while (!PostDetails.IsValidIndex(Index))
		{
			PostDetails.Add(nullptr);
		}

		TSharedPtr<IDetailsView>& View = PostDetails[Index];

		if (!View.IsValid())
		{
			FPropertyEditorModule& PropertyEditor =
				FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

			FDetailsViewArgs Args;
			Args.bAllowSearch           = false;
			Args.bShowOptions           = false;
			Args.bHideSelectionTip      = true;
			Args.bShowScrollBar         = false;
			Args.NameAreaSettings       = FDetailsViewArgs::HideNameArea;
			Args.DefaultsOnlyVisibility = EEditDefaultsOnlyNodeVisibility::Hide;

			View = PropertyEditor.CreateDetailView(Args);

			// **Rebuilt on every edit, so a rule that has just started applying is shown at once.** The
			// status, summary and cost lines are bound attributes and update themselves, but whether a row
			// draws a Run button or a refusal is decided when the row is built - so ticking "separate
			// parts" beside "generate textures" left the Run button live on a request the vendor refuses,
			// until something else happened to refresh the panel.
			View->OnFinishedChangingProperties().AddSP(this, &SMeshDefStages::OnPipelineEdited);
		}

		View->SetObject(Step, /*bForceRefresh*/ true);
		const TWeakObjectPtr<UMeshDef> WeakDef = Def;
		const TWeakObjectPtr<UMeshPostPipeline> WeakStep = Step;
		const auto CachedReason = MakeShared<TPair<double, FString>>(-1.0, FString());
		auto Reason = [WeakDef, WeakStep, CachedReason]() -> FString
		{
			const double Now = FPlatformTime::Seconds();
			if (Now - CachedReason->Key > 1.0)
			{
				CachedReason->Key = Now;
				UMeshDef* D = WeakDef.Get();
				UMeshForgeSubsystem* S = UMeshForgeSubsystem::Get();
				CachedReason->Value = D && S ? S->PostStepBlockedReason(D, D->PostPipelines.IndexOfByKey(WeakStep.Get()))
					: TEXT("The definition is unavailable.");
			}
			return CachedReason->Value;
		};

		Box->AddSlot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
			.Padding(FMargin(8.0f, 6.0f))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::Format(LOCTEXT("PostStepName", "{0}.  {1}"),
							FText::AsNumber(Index + 1),
							Step->GetClass()->GetDisplayNameText()))
						.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
					]

					+ SHorizontalBox::Slot().FillWidth(1.0f)

					+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("RunOnlyPostStep", "Run this step"))
						.IsEnabled_Lambda([Reason]() { return Reason().IsEmpty(); })
						.ToolTipText_Lambda([Reason]() { const FString Why = Reason(); return FText::FromString(Why.IsEmpty()
							? TEXT("Run only this step using its chosen input. Save the result as a separate output.") : Why); })
						.OnClicked_Lambda([WeakDef, WeakStep]()
						{
							if (UMeshDef* D = WeakDef.Get())
								if (UMeshForgeSubsystem* S = UMeshForgeSubsystem::Get())
								{
									FString Error;
									if (!S->RunPostStep(D, D->PostPipelines.IndexOfByKey(WeakStep.Get()), Error).IsValid())
										FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
								}
							return FReply::Handled();
						})
					]

					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SButton)
						.Text(LOCTEXT("RemovePostStepLabel", "Remove"))
						.ToolTipText(LOCTEXT("RemovePostStepTip",
							"Take this step out of the chain. Its settings go with it - to compare "
							"with and without, switch it off instead."))
						.OnClicked_Lambda([this, Index]()
						{
							RemovePostStep(Index);
							return FReply::Handled();
						})
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					View.ToSharedRef()
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
				[
					SNew(SComboButton)
					.ButtonContent()[SNew(STextBlock).Text(LOCTEXT("SavedPostInput", "Choose saved output as input…"))]
					.OnGetMenuContent_Lambda([this, WeakDef, WeakStep]() -> TSharedRef<SWidget>
					{
						FMenuBuilder Menu(true, nullptr);
						UMeshForgeSubsystem* S = UMeshForgeSubsystem::Get();
						const auto Choices = S ? S->GetPostInputChoices(WeakDef.Get()) : TArray<FMeshPostOutput>();
						int32 Count = 0;
						for (const FMeshPostOutput& Output : Choices)
						{
							const FSoftObjectPath Path = Output.Asset.ToSoftObjectPath();
							const auto Data = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetByObjectPath(Path);
							if (Data.AssetClassPath != UStaticMesh::StaticClass()->GetClassPathName()) continue;
							++Count;
							Menu.AddMenuEntry(FText::FromString(FString::Printf(TEXT("%s | %s | %s"), *Output.Step,
								*Output.CreatedUtc.ToString(), *Output.TakeId.Left(8))), FText::FromString(Path.ToString()), FSlateIcon(),
								FUIAction(FExecuteAction::CreateLambda([this, WeakDef, WeakStep, Path]()
								{
									if (!WeakDef.IsValid() || !WeakStep.IsValid()) return;
									const FScopedTransaction Transaction(LOCTEXT("SelectPostInput", "Select post-processing input"));
									WeakDef->Modify(); WeakStep->Modify();
									WeakStep->InputSource = EMeshPostInputSource::SelectedMesh;
									WeakStep->InputMesh = TSoftObjectPtr<UStaticMesh>(Path);
									WeakDef->MarkPackageDirty(); Refresh();
								})));
						}
						if (!Count) Menu.AddMenuEntry(LOCTEXT("NoSavedPostInput", "No saved static outputs yet"), FText(), FSlateIcon(), FUIAction());
						return Menu.MakeWidget();
					})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
				[
					SNew(STextBlock).AutoWrapText(true)
					.Text_Lambda([Reason]() { return FText::FromString(Reason()); })
				]
			]
		];
	}

	if (Classes.Num() == 0)
	{
		// Not an error. MeshForge ships no post steps of its own; they arrive with add-ons, and an
		// add-on being absent has to stay perfectly ordinary.
		Box->AddSlot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("NoPostClasses",
				"No post-processing steps are installed. They come with provider add-ons - "
				"MeshForge Cloud brings Meshy's retexture."))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		];

		return Box;
	}

	Box->AddSlot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
	[
		SNew(SComboButton)
		.ContentPadding(FMargin(6.0f, 2.0f))
		.ToolTipText(LOCTEXT("AddPostStepTip",
			"Add a step to the chain. Steps run in the order shown and each is handed what the one "
			"before it produced."))
		.ButtonContent()
		[
			SNew(STextBlock).Text(LOCTEXT("AddPostStepLabel", "Add a step..."))
		]
		.OnGetMenuContent_Lambda([this, Classes]()
		{
			FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);

			for (UClass* Class : Classes)
			{
				const UMeshForgePipeline* Default = Class->GetDefaultObject<UMeshForgePipeline>();
				const FString Cost = Default ? Default->DescribeCost() : FString();

				Menu.AddMenuEntry(
					Class->GetDisplayNameText(),
					Cost.IsEmpty()
						? Class->GetToolTipText()
						: FText::Format(LOCTEXT("PostEntryTip", "Costs {0}.\n\n{1}"),
							FText::FromString(Cost), Class->GetToolTipText()),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateSP(
						this, &SMeshDefStages::AddPostStep, Class)));
			}

			return Menu.MakeWidget();
		})
	];

	return Box;
}

TSharedRef<SWidget> SMeshDefStages::BuildPipelinePicker(EMeshStage Stage)
{
	const TArray<UClass*> Classes = PipelineClassesFor(Stage);

	if (Classes.Num() == 0)
	{
		return SNullWidget::NullWidget;
	}

	UMeshForgePipeline* Current = PipelineFor(Stage);

	const FText Label = Current
		? Current->GetClass()->GetDisplayNameText()
		: LOCTEXT("PickPipeline", "Choose...");

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	Box->AddSlot().AutoHeight()
	[
		SNew(SComboButton)
		.ContentPadding(FMargin(6.0f, 2.0f))
		.ToolTipText(LOCTEXT("PickPipelineTip",
			"Which building block this stage uses. Each one brings its own settings, because what "
			"matters to a local diffusion model and what matters to a hosted one have almost "
			"nothing in common."))
		.ButtonContent()
		[
			SNew(STextBlock).Text(Label)
		]
		.OnGetMenuContent_Lambda([this, Stage, Classes]()
		{
			FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);

			for (UClass* Class : Classes)
			{
				const UMeshForgePipeline* Default = Class->GetDefaultObject<UMeshForgePipeline>();
				const FString Cost = Default ? Default->DescribeCost() : FString();

				Menu.AddMenuEntry(
					Class->GetDisplayNameText(),
					Cost.IsEmpty()
						? Class->GetToolTipText()
						: FText::Format(LOCTEXT("PipelineEntryTip", "Costs {0}.\n\n{1}"),
							FText::FromString(Cost), Class->GetToolTipText()),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateSP(
						this, &SMeshDefStages::SetPipeline, Stage, Class)));
			}

			Menu.AddMenuSeparator();

			Menu.AddMenuEntry(
				LOCTEXT("ClearPipeline", "None"),
				LOCTEXT("ClearPipelineTip", "Leave this stage empty."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateSP(
					this, &SMeshDefStages::SetPipeline, Stage, static_cast<UClass*>(nullptr))));

			return Menu.MakeWidget();
		})
	];

	if (Current == nullptr)
	{
		return Box;
	}

	// The chosen pipeline's own settings, drawn from its reflected properties. Kept between
	// refreshes so that running a stage does not collapse categories somebody had opened.
	TSharedPtr<IDetailsView>& View = PipelineDetails.FindOrAdd(Stage);

	if (!View.IsValid())
	{
		FPropertyEditorModule& PropertyEditor =
			FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

		FDetailsViewArgs Args;
		Args.bAllowSearch              = false;
		Args.bShowOptions              = false;
		Args.bHideSelectionTip         = true;
		Args.bShowScrollBar            = false;
		Args.NameAreaSettings          = FDetailsViewArgs::HideNameArea;
		Args.DefaultsOnlyVisibility    = EEditDefaultsOnlyNodeVisibility::Hide;

		View = PropertyEditor.CreateDetailView(Args);
	}

	View->SetObject(Current, /*bForceRefresh*/ true);

	Box->AddSlot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
	[
		View.ToSharedRef()
	];

	return Box;
}

FText SMeshDefStages::RunLabel(EMeshStage Stage) const
{
	switch (Stage)
	{
	case EMeshStage::Concept: return LOCTEXT("RunDraw",     "Draw");
	case EMeshStage::Mesh:    return LOCTEXT("RunGenerate", "Generate");
	case EMeshStage::Post:    return LOCTEXT("RunPost",     "Run chain");
	case EMeshStage::Import:  return LOCTEXT("RunImport",   "Import");
	default:                  return LOCTEXT("RunStage",    "Run");
	}
}

FText SMeshDefStages::StageName(EMeshStage Stage) const
{
	switch (Stage)
	{
	case EMeshStage::Concept:    return LOCTEXT("StageConcept", "1  Concept image");
	case EMeshStage::References: return LOCTEXT("StageRefs",    "2  References");
	case EMeshStage::Mesh:       return LOCTEXT("StageMesh",    "3  Mesh");
	case EMeshStage::Post:       return LOCTEXT("StagePost",    "4  Post-processing");
	default:                     return LOCTEXT("StageImport",  "5  Import");
	}
}

FText SMeshDefStages::StageSummary(EMeshStage Stage) const
{
	const UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return FText::GetEmpty();
	}

	switch (Stage)
	{
	case EMeshStage::Concept:
		return MeshDefStagesUI::Describe(Def->ConceptPipeline,
			LOCTEXT("NoConceptPipeline",
				"Nothing chosen. Pick one below, or skip this stage and add your own picture in the "
				"Images tab."));

	case EMeshStage::References:
	{
		const TSoftObjectPtr<UTexture2D> Main = Def->ResolveMainImage();

		if (Main.IsNull())
		{
			return LOCTEXT("NoMainYet",
				"No picture chosen. Draw one above, or add your own in the Images tab.");
		}

		const int32 Allowed = Def->MaxExtraViews();
		const int32 Chosen  = Def->ExtraViews.Num();
		const int32 Used    = FMath::Min(Chosen, Allowed);
		const int32 Dropped = Chosen - Used;

		const FText Name = FText::FromString(
			FPackageName::ObjectPathToObjectName(Main.ToString()));

		// **The dropped case is stated first and in full, because it is the one that costs money.**
		// Somebody who has drawn four views and is told "3 of 3" will not notice that the generator
		// takes one; they will notice that the back of the object is wrong, hours later, and blame
		// the model. Silence here is the expensive kind.
		if (Dropped > 0)
		{
			return FText::Format(
				LOCTEXT("RefsDropping",
					"{0} extra view(s) chosen and this generator will read {1}. The other {2} will "
					"NOT be sent - it reconstructs from what it is given, so the result is made "
					"from {3} picture(s). Change the generator, or remove the extra views."),
				FText::AsNumber(Chosen), FText::AsNumber(Allowed), FText::AsNumber(Dropped),
				FText::AsNumber(Used + 1));
		}

		if (Allowed == 0)
		{
			return FText::Format(
				LOCTEXT("RefsSingleView",
					"Showing {0}. This generator reconstructs from one picture, so extra views "
					"would not be sent."),
				Name);
		}

		return FText::Format(
			LOCTEXT("RefsSummary",
				"Showing {0}, plus {1} of {2} extra view(s) this generator can read. "
				"Choose in the Images tab."),
			Name, FText::AsNumber(Used), FText::AsNumber(Allowed));
	}

	case EMeshStage::Mesh:
		if (!Def->SourceMesh.IsNull())
		{
			// Said here rather than left to be inferred from a greyed-out button. A definition that
			// supplies its own mesh is not a broken one, and it should not read as one.
			return FText::Format(
				LOCTEXT("SuppliedMesh",
					"Supplied: {0}. Generation is skipped - clear Source Mesh to generate instead."),
				FText::FromString(FPackageName::ObjectPathToObjectName(Def->SourceMesh.ToString())));
		}

		return MeshDefStagesUI::Describe(Def->MeshPipeline,
			Def->ProviderId.IsNone()
				? LOCTEXT("NoMeshPipeline", "No generator chosen.")
				: FText::Format(LOCTEXT("LegacyProvider", "{0} (provider settings, no pipeline)"),
					FText::FromName(Def->ProviderId)));

	case EMeshStage::Post:
		return MeshDefStagesUI::Describe(Def->PostPipelines,
			LOCTEXT("NoPost", "None - the mesh is imported as generated."));

	default:
	{
		const FMeshFinishSettings& Finish = Def->Finish;
		return FText::Format(
			LOCTEXT("ImportSummary", "{0} collision · {1} · {2} cm · {3}"),
			FText::FromString(StaticEnum<EMeshCollisionMode>()->GetDisplayNameTextByValue(
				static_cast<int64>(Finish.Collision)).ToString()),
			Finish.Nanite == EMeshNaniteMode::Off
				? LOCTEXT("NoNanite", "no Nanite")
				: LOCTEXT("WithNanite", "Nanite"),
			FText::AsNumber(FMath::RoundToInt(Finish.TargetSizeCm)),
			Finish.LodCount > 0
				? FText::Format(LOCTEXT("WithLods", "{0} LODs"), FText::AsNumber(Finish.LodCount))
				: LOCTEXT("NoLods", "no LODs"));
	}
	}
}

FText SMeshDefStages::StatusText(EMeshStage Stage) const
{
	const UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return FText::GetEmpty();
	}

	const FMeshStageState State = Def->GetStageState(Stage);

	switch (State.Status)
	{
	case EMeshStageStatus::Ready:   return LOCTEXT("StatusReady",   "ready");
	case EMeshStageStatus::Running:
	{
		const UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get();
		const float Elapsed = Subsystem ? Subsystem->StageElapsed(Def, Stage) : 0.0f;

		return (Elapsed > 0.0f)
			? FText::Format(LOCTEXT("StatusRunningFor", "running - {0}s"),
				FText::AsNumber(FMath::FloorToInt(Elapsed)))
			: LOCTEXT("StatusRunning", "running");
	}
	case EMeshStageStatus::Stale:   return LOCTEXT("StatusStale",   "stale - inputs changed since this ran");
	case EMeshStageStatus::Failed:  return FText::Format(LOCTEXT("StatusFailed", "failed - {0}"),
										FText::FromString(State.Error));
	default:                        return LOCTEXT("StatusEmpty",   "not run");
	}
}

FSlateColor SMeshDefStages::StatusColour(EMeshStage Stage) const
{
	const UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return FSlateColor::UseSubduedForeground();
	}

	switch (Def->GetStageState(Stage).Status)
	{
	case EMeshStageStatus::Ready:   return FSlateColor(FLinearColor(0.35f, 0.75f, 0.40f));
	case EMeshStageStatus::Running: return FSlateColor(FLinearColor(0.40f, 0.65f, 0.95f));
	case EMeshStageStatus::Stale:   return FSlateColor(FLinearColor(0.90f, 0.70f, 0.25f));
	case EMeshStageStatus::Failed:  return FSlateColor(FLinearColor(0.90f, 0.35f, 0.30f));
	default:                        return FSlateColor::UseSubduedForeground();
	}
}

FText SMeshDefStages::CostText(EMeshStage Stage) const
{
	const UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return FText::GetEmpty();
	}

	// Asked of the pipelines rather than guessed here, because only they know their vendor's
	// pricing - and a chain that spends on four providers should say what it is about to spend
	// before anybody presses the button.
	TArray<FString> Costs;

	auto Add = [&Costs](const UMeshForgePipeline* Pipeline)
	{
		if (Pipeline && Pipeline->bEnabled)
		{
			const FString Cost = Pipeline->DescribeCost();
			if (!Cost.IsEmpty()) { Costs.Add(Cost); }
		}
	};

	switch (Stage)
	{
	case EMeshStage::Concept: Add(Def->ConceptPipeline); break;
	case EMeshStage::Mesh:    Add(Def->MeshPipeline);    break;

	case EMeshStage::References:
		for (const TObjectPtr<UMeshForgePipeline>& P : Def->RefinementPipelines) { Add(P); }
		break;

	case EMeshStage::Post:
		for (const TObjectPtr<UMeshPostPipeline>& P : Def->PostPipelines) { Add(P); }
		break;

	default: break;
	}

	return Costs.Num() == 0
		? FText::GetEmpty()
		: FText::Format(LOCTEXT("CostLine", "Costs {0}"),
			FText::FromString(FString::Join(Costs, TEXT(" + "))));
}

EVisibility SMeshDefStages::CostVisibility(EMeshStage Stage) const
{
	// Collapsed rather than blank, so a free stage does not leave a gap where a price would be.
	return CostText(Stage).IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

FText SMeshDefStages::BlockedReason(EMeshStage Stage) const
{
	const UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return LOCTEXT("NoDef", "No definition.");
	}

	if (const UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get())
	{
		if (Subsystem->IsStageRunning(Def, Stage))
		{
			// In place of the button, so a stage cannot be started twice. The elapsed time is on the
			// status line beside the stage name, where it updates every frame.
			return LOCTEXT("StageRunningNow", "Running. It will land here when it is done.");
		}
	}

	if (Def->IsBusy())
	{
		return LOCTEXT("BusyNow", "Something is already running for this definition.");
	}

	// A pipeline that contradicts itself is the vendor's business and it says so itself. Catching it
	// here is the difference between a warning and an invoice.
	auto FirstComplaint = [](const UMeshForgePipeline* Pipeline) -> FText
	{
		if (Pipeline == nullptr || !Pipeline->bEnabled) { return FText::GetEmpty(); }
		const FString Complaint = Pipeline->Validate();
		return Complaint.IsEmpty() ? FText::GetEmpty() : FText::FromString(Complaint);
	};

	switch (Stage)
	{
	case EMeshStage::Concept:
		if (Def->ConceptPipeline == nullptr)
		{
			// A pipeline is genuinely required now that each one carries its own model, size and
			// billing. There is no sensible default: drawing free on this machine and drawing for
			// credits on somebody's account are not variations of one thing.
			return LOCTEXT("NeedImagePipeline", "Choose an image pipeline below.");
		}
		if (!Def->ConceptPipeline->bEnabled)
		{
			return LOCTEXT("ConceptOff", "The image pipeline is switched off.");
		}
		if (Def->Prompt.IsEmpty())
		{
			return LOCTEXT("NeedPrompt", "Write a prompt first.");
		}
		return FirstComplaint(Def->ConceptPipeline);

	case EMeshStage::References:
		if (Def->RefinementPipelines.Num() == 0)
		{
			return LOCTEXT("NothingToRefine",
				"Nothing to run here - choosing which pictures the model sees happens in the "
				"Images tab. Refinement pipelines are for making more pictures to choose from.");
		}
		if (!Def->HasInput() && Def->ConceptImages.Num() == 0)
		{
			return LOCTEXT("NeedAPicture", "No picture to refine yet.");
		}
		for (const TObjectPtr<UMeshForgePipeline>& P : Def->RefinementPipelines)
		{
			const FText Complaint = FirstComplaint(P);
			if (!Complaint.IsEmpty()) { return Complaint; }
		}
		return FText::GetEmpty();

	case EMeshStage::Mesh:
		if (!Def->SourceMesh.IsNull())
		{
			return LOCTEXT("MeshSupplied",
				"This definition supplies its own mesh, so there is nothing to generate.");
		}
		if (Def->MeshPipeline == nullptr && Def->ProviderId.IsNone())
		{
			return LOCTEXT("NeedGenerator", "Choose a mesh pipeline.");
		}
		if (!Def->HasInput() && Def->ConceptImages.Num() == 0 && Def->ReferenceImages.Num() == 0)
		{
			return LOCTEXT("NeedReference", "No reference image yet.");
		}

		// **The same two refusals the subsystem makes, said before the button rather than after.**
		// A main picture is required by every image-reconstructing model, and a multi-view request
		// with nothing but the main picture is multi-view in name only - it comes back looking like
		// an ordinary result that nobody can account for.
		if (Def->ResolveMainImage().IsNull() && Def->SourceImagePath.IsEmpty())
		{
			return LOCTEXT("NeedMainImage",
				"No main image chosen. Pick one in the Images tab - right-click a picture and "
				"choose Main image.");
		}

		if (Def->MeshPipeline != nullptr && Def->MeshPipeline->GetMaxExtraViews() > 0
			&& Def->ExtraViews.Num() == 0)
		{
			return LOCTEXT("NeedAView",
				"This generator is set to send several views and only the main picture is chosen. "
				"Give at least one more picture a view slot, or turn multiview_to_model off in the "
				"generator's settings.");
		}

		return FirstComplaint(Def->MeshPipeline);

	case EMeshStage::Post:
		if (Def->PostPipelines.Num() == 0)
		{
			return LOCTEXT("NothingToPost", "No post-processing pipelines - nothing to run.");
		}
		// A supplied mesh is the whole reason this stage is interesting, so it counts here first:
		// a definition made to retexture a corridor has no candidates and never will.
		if (Def->SourceMesh.IsNull() && Def->CountUsableCandidates() == 0)
		{
			return LOCTEXT("NeedAMesh",
				"No mesh to work on yet. Generate one, or set a Source Mesh on the Mesh stage to "
				"post-process a mesh you already have.");
		}
		for (const TObjectPtr<UMeshPostPipeline>& P : Def->PostPipelines)
		{
			const FText Complaint = FirstComplaint(P);
			if (!Complaint.IsEmpty()) { return Complaint; }
		}
		return FText::GetEmpty();

	default:
		if (Def->CountUsableCandidates() == 0)
		{
			return LOCTEXT("NothingToImport", "Nothing generated to import yet.");
		}
		return FText::GetEmpty();
	}
}

bool SMeshDefStages::CanRun(EMeshStage Stage) const
{
	return BlockedReason(Stage).IsEmpty();
}

FText SMeshDefStages::GetPrompt() const
{
	const UMeshDef* Def = Definition.Get();
	return Def ? FText::FromString(Def->Prompt) : FText::GetEmpty();
}

void SMeshDefStages::OnPromptCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	UMeshDef* Def = Definition.Get();
	if (Def == nullptr || Def->Prompt == NewText.ToString())
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("EditPrompt", "Edit prompt"));
	Def->Modify();
	Def->Prompt = NewText.ToString();

	// Everything downstream was made from a different brief and must say so. Outputs are kept.
	Def->RefreshStaleness();
	Refresh();
}

void SMeshDefStages::OnPipelineEdited(const FPropertyChangedEvent&)
{
	// A pipeline's settings decide what the whole definition would do, so everything downstream of
	// it was made from different inputs the moment one changes.
	if (UMeshDef* Def = Definition.Get())
	{
		Def->RefreshStaleness();
	}

	Refresh();
}

FReply SMeshDefStages::OnRunClicked(EMeshStage Stage)
{
	OnRunStage.ExecuteIfBound(Stage);
	Refresh();
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
