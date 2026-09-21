#include "SMeshDefStages.h"

#include "MeshForgeEditorStyle.h"
#include "SMeshForgeSection.h"
#include "Widgets/Images/SImage.h"

#include "MeshDef.h"
#include "MeshForgePipeline.h"
#include "MeshImagePipeline.h"
#include "MeshForgeSubsystem.h"
#include "MeshForge.h"
#include "MeshRegisteredPostStep.h"
#include "MeshWorkflow.h"
#include "MeshForgeSettings.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Widgets/Layout/SBox.h"
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
#include "Widgets/Input/SCheckBox.h"
#include "Containers/Ticker.h"
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

	/** A post step names itself - a registered step shares one class with every other one. */
	static FString NameOf(const UMeshForgePipeline* Pipeline)
	{
		const UMeshPostPipeline* Post = Cast<UMeshPostPipeline>(Pipeline);
		return Post ? Post->GetStepDisplayName().ToString() : Pipeline->GetClass()->GetDisplayNameText().ToString();
	}

	template <typename PipelineType>
	static FText Describe(const TArray<TObjectPtr<PipelineType>>& Pipelines, const FText& NoneText)
	{
		TArray<FString> Names;
		for (const TObjectPtr<PipelineType>& Pipeline : Pipelines)
		{
			if (Pipeline && Pipeline->bEnabled)
			{
				Names.Add(NameOf(Pipeline));
			}
		}

		return Names.Num() == 0
			? NoneText
			: FText::FromString(FString::Join(Names, TEXT("  →  ")));
	}

	/** "2.  Wardrobe skinning" - a step as the chain numbers it. */
	static FText PostStepLabel(const UMeshDef* Def, const UMeshPostPipeline* Step)
	{
		if (Def == nullptr || Step == nullptr)
		{
			return FText::GetEmpty();
		}
		return FText::Format(LOCTEXT("PostStepChoice", "{0}.  {1}{2}"),
			FText::AsNumber(Def->PostPipelines.IndexOfByKey(Step) + 1), Step->GetStepDisplayName(),
			Step->bEnabled ? FText::GetEmpty() : LOCTEXT("PostStepDisabled", "  (off)"));
	}

	/** One run of a step: when it finished, in local time, its take id and what it made. */
	static FText PostOutputLabel(const FMeshPostOutput& Output)
	{
		const FDateTime Local = Output.CreatedUtc + (FDateTime::Now() - FDateTime::UtcNow());
		return FText::FromString(FString::Printf(TEXT("%s  ·  %s  ·  %s"),
			*Local.ToString(TEXT("%Y-%m-%d %H:%M")), *Output.TakeId.Left(8),
			*FPackageName::GetShortName(Output.Asset.ToSoftObjectPath().GetAssetName())));
	}

	/** The Mesh stage as a source in "A step's output": its Source Mesh where it has one, else its takes. */
	static FText MeshStageSourceLabel(const UMeshDef* Def)
	{
		return Def != nullptr && !Def->SourceMesh.IsNull()
			? FText::Format(LOCTEXT("MeshStageSourceSupplied", "Mesh stage  (Source Mesh: {0})"),
				FText::FromString(FPackageName::ObjectPathToObjectName(Def->SourceMesh.ToString())))
			: LOCTEXT("MeshStageSource", "Mesh stage  (a generated take)");
	}

	/** One generated take: when it finished, in local time, who made it, how dense it is and its id. */
	static FText TakeLabel(const FMeshCandidate& Take)
	{
		TArray<FString> Parts;
		if (Take.GeneratedAt.GetTicks() > 0)
		{
			const FDateTime Local = Take.GeneratedAt + (FDateTime::Now() - FDateTime::UtcNow());
			Parts.Add(Local.ToString(TEXT("%Y-%m-%d %H:%M")));
		}
		Parts.Add(Take.ModelId.IsEmpty() ? Take.ProviderId.ToString()
			: FString::Printf(TEXT("%s %s"), *Take.ProviderId.ToString(), *Take.ModelId));
		if (Take.TriangleCount > 0)
		{
			Parts.Add(FString::Printf(TEXT("%s tris"), *FText::AsNumber(Take.TriangleCount).ToString()));
		}
		Parts.Add(Take.MeshId.Left(8));
		return FText::FromString(FString::Join(Parts, TEXT("  ·  ")));
	}

	/** Whether a step can start from this asset: what it accepts if it makes assets, a static mesh if not. */
	static bool CanStartFrom(const UMeshPostPipeline* Step, const FSoftObjectPath& Path)
	{
		const FAssetData Data = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetByObjectPath(Path);
		if (Step == nullptr || !Data.IsValid())
		{
			return false;
		}
		return Step->IsNativeStep()
			? Step->AcceptsNativeInput(Data.GetClass(EResolveClass::Yes))
			: Data.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName();
	}
}

void SMeshDefStages::Construct(const FArguments& InArgs)
{
	Definition  = InArgs._Definition;
	OnRunStage  = InArgs._OnRunStage;

	// No prompt up here. Each pipeline that reads words carries its own, drawn in its stage's settings -
	// the picture's on Concept image, the generator's on Mesh, a retexture's on its step - and a
	// definition that starts from a mesh shows none at all.
	ChildSlot
	[
		SNew(SVerticalBox)

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

	const UMeshDef* Def = Definition.Get();

	Rows->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
	[
		BuildWorkflowBar()
	];

	if (Def != nullptr && !Def->StageSwitches.bMesh)
	{
		Rows->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			BuildStartsFrom()
		];
	}

	int32 Number = 0;
	for (const EMeshStage Stage : MeshDefStagesUI::Order)
	{
		// Not drawn at all. The switches above say it is off; a row saying "skipped" under every one of
		// them would be the panel describing a pipeline this definition does not run.
		if (Def != nullptr && !Def->StageSwitches.IsOn(Stage))
		{
			continue;
		}

		// Numbered among the stages this definition uses, so a definition that starts from a mesh reads
		// 1 Post-processing, 2 Import rather than 4 and 5 after three it does not have.
		Rows->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			BuildStageRow(Stage, ++Number)
		];
	}
}

TSharedRef<SWidget> SMeshDefStages::BuildWorkflowBar()
{
	TSharedRef<SHorizontalBox> Switches = SNew(SHorizontalBox);

	Switches->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 12.0f, 0.0f)
	[
		SNew(STextBlock)
		.Text(LOCTEXT("StagesLabel", "Stages"))
		.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
	];

	for (const EMeshStage Stage : MeshDefStagesUI::Order)
	{
		Switches->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 14.0f, 0.0f)
		[
			SNew(SCheckBox)
			.ToolTipText(FText::Format(LOCTEXT("StageSwitchTip",
				"Use the {0} stage. Off hides it here and in Settings, and it is refused rather than run."),
				StaticEnum<EMeshStage>()->GetDisplayNameTextByValue(static_cast<int64>(Stage))))
			.IsChecked_Lambda([this, Stage]()
			{
				const UMeshDef* D = Definition.Get();
				return D && D->StageSwitches.IsOn(Stage) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.IsEnabled_Lambda([this]()
			{
				const UMeshDef* D = Definition.Get();
				return D != nullptr && !D->IsBusy();
			})
			.OnCheckStateChanged_Lambda([this, Stage](ECheckBoxState State)
			{
				SetStageSwitch(Stage, State == ECheckBoxState::Checked);
			})
			[
				SNew(STextBlock).Text(StaticEnum<EMeshStage>()->GetDisplayNameTextByValue(static_cast<int64>(Stage)))
			]
		];
	}

	TSharedRef<SHorizontalBox> Picker = SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 12.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("WorkflowLabel", "Workflow"))
			.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SBox)
			.MinDesiredWidth(220.0f)
			[
				SNew(SComboButton)
				.ContentPadding(FMargin(6.0f, 2.0f))
				.IsEnabled_Lambda([this]()
				{
					const UMeshDef* D = Definition.Get();
					return D != nullptr && !D->IsBusy();
				})
				.ToolTipText(LOCTEXT("WorkflowPickerTip",
					"Set this definition up from a Mesh Workflow: its stage switches, pipelines and post steps are "
					"copied in. The prompt, pictures, source mesh, takes and outputs are kept."))
				.ButtonContent()
				[
					SNew(STextBlock)
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.Text_Lambda([this]()
					{
						const UMeshDef* D = Definition.Get();
						if (D == nullptr || D->Workflow.IsNull())
						{
							return LOCTEXT("CustomWorkflow", "Custom - set up by hand");
						}
						const UMeshWorkflow* W = D->Workflow.LoadSynchronous();
						return W ? W->GetShownName() : FText::FromString(D->Workflow.GetAssetName());
					})
				]
				.OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
				{
					FMenuBuilder Menu(true, nullptr);

					TArray<FAssetData> Workflows;
					FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get()
						.GetAssetsByClass(UMeshWorkflow::StaticClass()->GetClassPathName(), Workflows, true);
					Workflows.Sort([](const FAssetData& A, const FAssetData& B) { return A.AssetName.LexicalLess(B.AssetName); });

					for (const FAssetData& Asset : Workflows)
					{
						const UMeshWorkflow* W = Cast<UMeshWorkflow>(Asset.GetAsset());
						if (W == nullptr)
						{
							continue;
						}
						const FSoftObjectPath Path = Asset.GetSoftObjectPath();
						Menu.AddMenuEntry(W->GetShownName(),
							FText::Format(LOCTEXT("WorkflowEntryTip", "{0}\n\n{1}"), W->Description, FText::FromString(Path.ToString())),
							FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([this, Path]() { ApplyWorkflow(Path, false); })));
					}

					if (Workflows.Num() == 0)
					{
						Menu.AddMenuEntry(LOCTEXT("NoWorkflows", "No Mesh Workflows in this project yet - use Save as Workflow"),
							FText(), FSlateIcon(), FUIAction());
					}

					return Menu.MakeWidget();
				})
			]
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("ReapplyWorkflow", "Reapply"))
			.ToolTipText(LOCTEXT("ReapplyWorkflowTip",
				"Copy the workflow in again, replacing this definition's changes to it. Post steps that are "
				"still the same kind keep their history."))
			.Visibility_Lambda([this]()
			{
				const UMeshDef* D = Definition.Get();
				return D && !D->Workflow.IsNull() ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.IsEnabled_Lambda([this]()
			{
				const UMeshDef* D = Definition.Get();
				return D != nullptr && !D->IsBusy();
			})
			.OnClicked_Lambda([this]()
			{
				if (const UMeshDef* D = Definition.Get())
				{
					ApplyWorkflow(D->Workflow.ToSoftObjectPath(), true);
				}
				return FReply::Handled();
			})
		]

		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(4.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(this, &SMeshDefStages::WorkflowChangesText)
			.ToolTipText(this, &SMeshDefStages::WorkflowChangesTooltip)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton)
			.Text(LOCTEXT("SaveAsWorkflow", "Save as Workflow..."))
			.ToolTipText(LOCTEXT("SaveAsWorkflowTip",
				"Make a Mesh Workflow from this definition as it stands - its stage switches, pipelines, post "
				"steps and import settings - to set other definitions up the same way."))
			.OnClicked_Lambda([this]() { SaveAsWorkflow(); return FReply::Handled(); })
		];

	return SNew(SBorder)
		.BorderImage(MeshForgeStyle::SectionBrush())
		.Padding(FMargin(12.0f, 8.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ Picker ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)[ Switches ]
		];
}

void SMeshDefStages::ApplyWorkflow(const FSoftObjectPath& WorkflowPath, bool bReapply)
{
	UMeshDef* Def = Definition.Get();
	const UMeshWorkflow* Workflow = Cast<UMeshWorkflow>(WorkflowPath.TryLoad());

	if (Def == nullptr || Workflow == nullptr)
	{
		return;
	}

	// Asked only when something of the definition's own would be replaced: switching workflow on a
	// definition that has pipelines, or reapplying over changes made since. Undo restores either way.
	const bool bHasOwnSetup = Def->ConceptPipeline || Def->MeshPipeline || Def->RefinementPipelines.Num() > 0 || Def->PostPipelines.Num() > 0;
	const bool bSameWorkflow = Def->Workflow.ToSoftObjectPath() == WorkflowPath;
	const TArray<FString> Changes = bSameWorkflow ? Def->DescribeWorkflowChanges() : TArray<FString>();

	if ((!bSameWorkflow && bHasOwnSetup) || (bSameWorkflow && Changes.Num() > 0))
	{
		const FText Question = bSameWorkflow
			? FText::Format(LOCTEXT("ConfirmReapply",
				"Reapply '{0}'? These changes made since it was applied are replaced:\n\n{1}\n\nCtrl+Z restores them."),
				Workflow->GetShownName(), FText::FromString(FString::Join(Changes, TEXT("\n"))))
			: FText::Format(LOCTEXT("ConfirmApply",
				"Apply '{0}'? This definition's stage switches, pipelines and post steps are replaced by the "
				"workflow's. Its prompt, pictures, source mesh, takes and outputs are kept. Ctrl+Z restores it."),
				Workflow->GetShownName());

		if (FMessageDialog::Open(EAppMsgType::YesNo, Question) != EAppReturnType::Yes)
		{
			return;
		}
	}

	TArray<FString> Notes;
	FString Error;
	{
		const FScopedTransaction Transaction(bReapply
			? LOCTEXT("ReapplyWorkflowTransaction", "Reapply workflow")
			: LOCTEXT("ApplyWorkflowTransaction", "Apply workflow"));

		if (!Def->ApplyWorkflow(Workflow, Notes, Error))
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
			return;
		}

		FProperty* Property = UMeshDef::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UMeshDef, StageSwitches));
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Def->PostEditChangeProperty(Event);
	}

	if (Notes.Num() > 0)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
			LOCTEXT("WorkflowNotes", "'{0}' was applied, with these slots left empty:\n\n{1}"),
			Workflow->GetShownName(), FText::FromString(FString::Join(Notes, TEXT("\n")))));
	}

	WorkflowChangesAt = -1.0;
	PostDetails.Reset();
	PipelineDetails.Reset();

	TWeakPtr<SMeshDefStages> WeakThis = SharedThis(this);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis](float) -> bool
	{
		if (const TSharedPtr<SMeshDefStages> Live = WeakThis.Pin())
		{
			Live->Refresh();
		}
		return false;
	}), 0.0f);
}

void SMeshDefStages::SaveAsWorkflow()
{
	UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return;
	}

	FSaveAssetDialogConfig Config;
	Config.DialogTitleOverride = LOCTEXT("SaveWorkflowTitle", "Save as Mesh Workflow");
	Config.DefaultPath = UMeshForgeSettings::Get()->GetWorkflowsPath();
	Config.DefaultAssetName = TEXT("WF_") + Def->GetName().Replace(TEXT("MSD_"), TEXT(""));
	Config.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::Disallow;

	IContentBrowserSingleton& ContentBrowser =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get();
	const FString ObjectPath = ContentBrowser.CreateModalSaveAssetDialog(Config);

	if (ObjectPath.IsEmpty())
	{
		return;
	}

	FString Error;
	UMeshWorkflow* Workflow = UMeshWorkflow::CreateFromDefinition(Def, ObjectPath, Error);

	if (Workflow == nullptr)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
		return;
	}

	// The definition now matches the workflow it just became, so it is recorded as coming from it.
	{
		const FScopedTransaction Transaction(LOCTEXT("SaveAsWorkflowTransaction", "Save as workflow"));
		Def->Modify();
		Def->Workflow = Workflow;
		Def->MarkPackageDirty();
	}

	if (!Error.IsEmpty())
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
	}

	WorkflowChangesAt = -1.0;
}

void SMeshDefStages::RefreshWorkflowChanges() const
{
	const double Now = FPlatformTime::Seconds();
	if (Now - WorkflowChangesAt < 1.0)
	{
		return;
	}

	WorkflowChangesAt = Now;
	const UMeshDef* Def = Definition.Get();
	WorkflowChanges = Def ? Def->DescribeWorkflowChanges() : TArray<FString>();
}

FText SMeshDefStages::WorkflowChangesText() const
{
	const UMeshDef* Def = Definition.Get();
	if (Def == nullptr || Def->Workflow.IsNull())
	{
		return FText::GetEmpty();
	}

	RefreshWorkflowChanges();
	return WorkflowChanges.Num() == 0
		? LOCTEXT("MatchesWorkflow", "Matches its workflow")
		: FText::Format(LOCTEXT("ChangedSinceWorkflow", "{0} {0}|plural(one=change,other=changes) since it was applied"),
			FText::AsNumber(WorkflowChanges.Num()));
}

FText SMeshDefStages::WorkflowChangesTooltip() const
{
	RefreshWorkflowChanges();
	return WorkflowChanges.Num() == 0
		? FText::GetEmpty()
		: FText::FromString(FString::Join(WorkflowChanges, TEXT("\n")));
}

void SMeshDefStages::SetStageSwitch(EMeshStage Stage, bool bOn)
{
	UMeshDef* Def = Definition.Get();
	if (Def == nullptr || Def->StageSwitches.IsOn(Stage) == bOn)
	{
		return;
	}

	const FScopedTransaction Transaction(bOn
		? LOCTEXT("StageOn", "Switch a stage on")
		: LOCTEXT("StageOff", "Switch a stage off"));
	Def->Modify();
	Def->StageSwitches.Set(Stage, bOn);

	// Broadcast as an edit of the switches, so the Settings tab rebuilds its categories too.
	FProperty* Property = UMeshDef::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UMeshDef, StageSwitches));
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Def->PostEditChangeProperty(Event);

	Def->RefreshStaleness();
	Def->MarkPackageDirty();

	// Deferred: the checkbox that sent this lives in the rows being rebuilt.
	TWeakPtr<SMeshDefStages> WeakThis = SharedThis(this);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis](float) -> bool
	{
		if (const TSharedPtr<SMeshDefStages> Live = WeakThis.Pin())
		{
			Live->Refresh();
		}
		return false;
	}), 0.0f);
}

TSharedRef<SWidget> SMeshDefStages::BuildStartsFrom()
{
	return SNew(SBorder)
		.BorderImage(MeshForgeStyle::SectionBrush())
		.Padding(FMargin(12.0f, 10.0f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("StartsFromHeading", "Starts from"))
				.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([this]()
				{
					const UMeshDef* D = Definition.Get();
					return D && D->SourceMesh.IsNull()
						? LOCTEXT("StartsFromEmpty",
							"Mesh is switched off, so this definition works on a mesh you already have. Choose it here.")
						: LOCTEXT("StartsFromSet",
							"Mesh is switched off: post-processing and import work on this mesh.");
				})
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				BuildSourceMeshPicker()
			]
		];
}

TSharedRef<SWidget> SMeshDefStages::BuildSourceMeshPicker()
{
	return SNew(SObjectPropertyEntryBox)
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
		});
}

TSharedRef<SWidget> SMeshDefStages::BuildStageRow(EMeshStage Stage, int32 Number)
{
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

	// The header's status line is one line and cut short; a failure's reason, or what went stale, is
	// said here in full and in its colour. Nothing when the stage is fine.
	Body->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
	[
		SNew(STextBlock)
		.Text(this, &SMeshDefStages::StatusText, Stage)
		.ColorAndOpacity(this, &SMeshDefStages::StatusColour, Stage)
		.AutoWrapText(true)
		.Visibility_Lambda([this, Stage]()
		{
			const UMeshDef* Def = Definition.Get();
			const EMeshStageStatus Status = Def ? Def->GetStageState(Stage).Status : EMeshStageStatus::Empty;
			return Status == EMeshStageStatus::Failed || Status == EMeshStageStatus::Stale ? EVisibility::Visible : EVisibility::Collapsed;
		})
	];

	// A mesh somebody already has, offered here rather than only in the Settings tab. It overrides
	// this whole stage, so the place to say so is the stage it overrides.
	// A definition made before mesh pipelines has nowhere else for its generator's words: they are still
	// the definition's own, so they are edited here. Every other prompt is in its pipeline's settings.
	const UMeshDef* Current = Definition.Get();
	if (Stage == EMeshStage::Mesh && Current != nullptr && Current->MeshPipeline == nullptr
		&& !Current->ProviderId.IsNone() && Current->SourceMesh.IsNull())
	{
		Body->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SBox)
			.MinDesiredHeight(72.0f)
			[
				SNew(SMultiLineEditableTextBox)
				.Text(this, &SMeshDefStages::GetPrompt)
				.OnTextCommitted(this, &SMeshDefStages::OnPromptCommitted)
				.AutoWrapText(true)
				.HintText(LOCTEXT("LegacyMeshPromptHint", "What to build, in words, for a generator that reads text."))
			]
		];
	}

	if (Stage == EMeshStage::Mesh)
	{
		Body->AddSlot().AutoHeight()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("SourceMeshLabel", "Or start from a mesh you have:"))
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				BuildSourceMeshPicker()
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

	const bool* Remembered = StageExpanded.Find(Stage);
	const bool bOpen = Remembered ? *Remembered : StageSectionState(Stage) != EMeshSectionState::Done;

	// Status in the header, where it is read with the section closed; what the stage holds as the
	// body's first line. Both bound, because the stage's own settings are edited inside it.
	return SNew(SMeshForgeSection)
		.Number(Number)
		.Title(StageName(Stage))
		.Summary(this, &SMeshDefStages::StatusText, Stage)
		.Subtitle(this, &SMeshDefStages::StageSummary, Stage)
		.State(this, &SMeshDefStages::StageSectionState, Stage)
		.InitiallyExpanded(bOpen)
		.OnExpansionChanged_Lambda([this, Stage](bool bExpanded) { StageExpanded.Add(Stage, bExpanded); })
		[
			Body
		];
}

EMeshSectionState SMeshDefStages::StageSectionState(EMeshStage Stage) const
{
	const UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return EMeshSectionState::None;
	}

	switch (Def->GetStageState(Stage).Status)
	{
	case EMeshStageStatus::Ready:   return EMeshSectionState::Done;
	case EMeshStageStatus::Running: return EMeshSectionState::Current;
	case EMeshStageStatus::Stale:
	case EMeshStageStatus::Failed:  return EMeshSectionState::Attention;
	default:                        break;
	}

	// Not run: current when it is the first stage that is neither done nor waiting on something earlier.
	for (const EMeshStage Earlier : MeshDefStagesUI::Order)
	{
		if (!Def->StageSwitches.IsOn(Earlier))
		{
			continue;
		}
		if (Earlier == Stage)
		{
			return CanRun(Stage) ? EMeshSectionState::Current : EMeshSectionState::Todo;
		}
		if (Def->GetStageState(Earlier).Status != EMeshStageStatus::Ready && CanRun(Earlier))
		{
			return EMeshSectionState::Todo;
		}
	}
	return EMeshSectionState::Todo;
}

TSharedRef<SWidget> SMeshDefStages::BuildActionBar(EMeshStage Stage)
{
	return SNew(SBorder)
		.BorderImage(MeshForgeStyle::InsetBrush())
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
	{
		const UMeshDef* Def = Definition.Get();
		if (Def != nullptr && Def->GetMeshInput() == EMeshPipelineInput::Text)
		{
			return LOCTEXT("HintMeshText",
				"Sends the prompt above to the generator, without any picture, and files what comes back "
				"as a new take. Nothing appears in the project until a take is imported.");
		}
		return LOCTEXT("HintMesh",
			"Sends the pictures shown above to the generator and files what comes back as a new "
			"take. Nothing appears in the project until a take is imported.");
	}

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
			const UMeshPostPipeline* AsPost = Cast<UMeshPostPipeline>(Default);
			if (Default->GetKind() == Wanted && (AsPost == nullptr || AsPost->IsOfferedInPicker()))
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

void SMeshDefStages::AddRegisteredPostStep(FName TypeId)
{
	UMeshDef* Def = Definition.Get();

	if (Def == nullptr)
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("AddRegisteredPostStep", "Add post-processing step"));
	Def->Modify();

	FString Error;
	if (UMeshRegisteredPostStep* Step = UMeshRegisteredPostStep::Create(Def, TypeId, Error))
	{
		Def->PostPipelines.Add(Step);
	}
	else
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
	}

	Def->RefreshStaleness();
	Refresh();
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
		// Switching the provider keeps the words: a prompt is what is being made, not how.
		UMeshDef::CarryPrompt(Def->ConceptPipeline, Instance, Def->Prompt);
		Def->ConceptPipeline = Cast<UMeshImagePipeline>(Instance);
		break;

	case EMeshStage::Mesh:
		UMeshDef::CarryPrompt(Def->MeshPipeline, Instance, Def->Prompt);
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

void SMeshDefStages::MovePostStep(int32 From, int32 To)
{
	UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("MovePostStep", "Move post-processing step"));
	FString Error;
	if (!Def->MovePostStep(From, To, Error))
	{
		// Not offered in the first place - the arrows grey out with the reason - so reaching here means
		// the chain changed under the panel. Say why rather than moving nothing silently.
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
		return;
	}

	// The views are kept by position and the positions just changed; move them with their steps, so
	// each keeps its own open categories.
	if (PostDetails.IsValidIndex(From) && PostDetails.IsValidIndex(To))
	{
		const TSharedPtr<IDetailsView> Moved = PostDetails[From];
		PostDetails.RemoveAt(From);
		PostDetails.Insert(Moved, To);
	}
	else
	{
		PostDetails.Reset();
	}

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

		// Where the step stands, for its badge and header: asked of the subsystem at most once a second,
		// because finding its runs reads the asset registry.
		struct FStepGlance { double At = -1.0; EMeshSectionState State = EMeshSectionState::Todo; FText Line; };
		const auto Glance = MakeShared<FStepGlance>();
		auto Look = [WeakDef, WeakStep, Glance]() -> const FStepGlance&
		{
			const double Now = FPlatformTime::Seconds();
			if (Now - Glance->At <= 1.0)
			{
				return *Glance;
			}
			Glance->At = Now;

			const UMeshDef* D = WeakDef.Get();
			const UMeshPostPipeline* S = WeakStep.Get();
			const UMeshForgeSubsystem* Sub = UMeshForgeSubsystem::Get();
			if (D == nullptr || S == nullptr || Sub == nullptr)
			{
				Glance->State = EMeshSectionState::None;
				Glance->Line = FText::GetEmpty();
				return *Glance;
			}

			const int32 At = D->PostPipelines.IndexOfByKey(S);
			TArray<FMeshPostOutput> Runs = S->StepId.IsValid() ? Sub->GetStepOutputs(D, S->StepId) : TArray<FMeshPostOutput>();

			if (!S->bEnabled)
			{
				Glance->State = EMeshSectionState::None;
				Glance->Line = LOCTEXT("StepOffLine", "switched off - the chain skips it");
			}
			else if (S->IsInteractiveStep() && Sub->IsWaitingForInteractive(D, At))
			{
				Glance->State = EMeshSectionState::Attention;
				Glance->Line = LOCTEXT("StepWaitingLine", "waiting for you to finish the edit");
			}
			else if (S->IsInteractiveStep() && Sub->HasUnclaimedInteractiveResult(D, At))
			{
				Glance->State = EMeshSectionState::Attention;
				Glance->Line = LOCTEXT("StepPickUpLine", "an edit is finished - pick up the result");
			}
			else if (Runs.Num() > 0)
			{
				Glance->State = EMeshSectionState::Done;
				Glance->Line = FText::Format(LOCTEXT("StepRanLine", "last run {0}"), MeshDefStagesUI::PostOutputLabel(Runs[0]));
			}
			else
			{
				Glance->State = EMeshSectionState::Todo;
				Glance->Line = LOCTEXT("StepNotRunLine", "not run yet");
			}
			return *Glance;
		};

		// Whether this step's section is open, kept across rebuilds by the step itself rather than its
		// position, so moving a step does not open or close the one that took its place.
		const FObjectKey StepKey(Step);
		const bool* Remembered = StepExpanded.Find(StepKey);
		const bool bOpen = Remembered ? *Remembered : Look().State != EMeshSectionState::Done;

		auto MoveButton = [this, WeakDef, Index](int32 To, const FName Icon, const FText& Tip) -> TSharedRef<SWidget>
		{
			return SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(3.0f, 2.0f))
				.IsEnabled_Lambda([WeakDef, Index, To]()
				{
					const UMeshDef* D = WeakDef.Get();
					return D != nullptr && D->PostStepMoveProblem(Index, To).IsEmpty();
				})
				.ToolTipText_Lambda([WeakDef, Index, To, Tip]()
				{
					const UMeshDef* D = WeakDef.Get();
					const FString Why = D && D->PostPipelines.IsValidIndex(To) ? D->PostStepMoveProblem(Index, To) : FString();
					return Why.IsEmpty() ? Tip : FText::FromString(Why);
				})
				.OnClicked_Lambda([this, Index, To]()
				{
					MovePostStep(Index, To);
					return FReply::Handled();
				})
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush(Icon))
					.ColorAndOpacity(FSlateColor::UseForeground())
				];
		};

		Box->AddSlot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(SMeshForgeSection)
			.Inset(true)
			.Number(Index + 1)
			.Title(Step->bEnabled
				? Step->GetStepDisplayName()
				: FText::Format(LOCTEXT("PostStepTitleOff", "{0}  (off)"), Step->GetStepDisplayName()))
			.Summary_Lambda([Look]() { return Look().Line; })
			.State_Lambda([Look]() { return Look().State; })
			.InitiallyExpanded(bOpen)
			.OnExpansionChanged_Lambda([this, StepKey](bool bExpanded) { StepExpanded.Add(StepKey, bExpanded); })
			.HeaderRight()
			[
				SNew(SHorizontalBox)

				// Up and down, first: moving is about the chain, the buttons after it are about this step.
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MoveButton(Index - 1, "Icons.ChevronUp",
						LOCTEXT("MoveStepUpTip", "Move this step up. The chain runs top to bottom; the Post stage goes stale and every output is kept."))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					MoveButton(Index + 1, "Icons.ChevronDown",
						LOCTEXT("MoveStepDownTip", "Move this step down. The chain runs top to bottom; the Post stage goes stale and every output is kept."))
				]

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

				// A step a person finishes: stop waiting on it, or take a result that arrived while
				// nothing was waiting. Checked once a second - the second one reads a file.
				+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("StopWaitingLabel", "Stop waiting"))
					.ToolTipText(LOCTEXT("StopWaitingTip",
						"Stop waiting for this step. The edit stays open - in Blender, or in the sculpt window - and a "
						"result finished later can be picked up here."))
					.Visibility_Lambda([WeakDef, WeakStep]()
					{
						const UMeshDef* D = WeakDef.Get();
						const UMeshForgeSubsystem* S = UMeshForgeSubsystem::Get();
						return D && S && WeakStep.IsValid() && WeakStep->IsInteractiveStep()
							&& S->IsWaitingForInteractive(D, D->PostPipelines.IndexOfByKey(WeakStep.Get()))
							? EVisibility::Visible : EVisibility::Collapsed;
					})
					.OnClicked_Lambda([WeakDef]()
					{
						if (UMeshForgeSubsystem* S = UMeshForgeSubsystem::Get())
						{
							S->StopWaitingForInteractive(WeakDef.Get());
						}
						return FReply::Handled();
					})
				]

				+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("PickUpResultLabel", "Pick up result"))
					.ToolTipText(LOCTEXT("PickUpResultTip",
						"Take the finished edit as this step's output. The steps after it "
						"do not run; run them when you are ready."))
					.Visibility_Lambda([WeakDef, WeakStep, CachedPickUp = MakeShared<TPair<double, bool>>(-1.0, false)]()
					{
						const double Now = FPlatformTime::Seconds();
						if (Now - CachedPickUp->Key > 1.0)
						{
							CachedPickUp->Key = Now;
							const UMeshDef* D = WeakDef.Get();
							const UMeshForgeSubsystem* S = UMeshForgeSubsystem::Get();
							CachedPickUp->Value = D && S && WeakStep.IsValid() && WeakStep->IsInteractiveStep()
								&& S->HasUnclaimedInteractiveResult(D, D->PostPipelines.IndexOfByKey(WeakStep.Get()));
						}
						return CachedPickUp->Value ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.OnClicked_Lambda([WeakDef, WeakStep]()
					{
						if (UMeshDef* D = WeakDef.Get())
							if (UMeshForgeSubsystem* S = UMeshForgeSubsystem::Get())
							{
								FString Error;
								if (!S->PickUpInteractiveResult(D, D->PostPipelines.IndexOfByKey(WeakStep.Get()), Error).IsValid())
									FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
							}
						return FReply::Handled();
					})
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
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
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					View.ToSharedRef()
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
				[
					SNew(SComboButton)
					// Only where it applies. It picks a fixed asset, which is what "Selected mesh" means; shown
					// under every source it read as a second, unexplained way of choosing the input.
					.Visibility_Lambda([WeakStep]()
					{
						return WeakStep.IsValid() && WeakStep->InputSource == EMeshPostInputSource::SelectedMesh
							? EVisibility::Visible : EVisibility::Collapsed;
					})
					.ToolTipText(LOCTEXT("SavedPostInputTip",
						"Pick any saved output of this definition - from any step, any run - as a fixed input. "
						"It stays that exact asset whatever runs later. To follow a step's newest run instead, "
						"choose \"A step's output\"."))
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
							// A native step lists what it can work on; a geometry step lists static meshes.
							const bool bUsable = WeakStep.IsValid() && WeakStep->IsNativeStep()
								? Data.IsValid() && WeakStep->AcceptsNativeInput(Data.GetClass(EResolveClass::Yes))
								: Data.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName();
							if (!bUsable) continue;
							++Count;
							Menu.AddMenuEntry(FText::FromString(FString::Printf(TEXT("%s | %s | %s"), *Output.Step,
								*Output.CreatedUtc.ToString(), *Output.TakeId.Left(8))), FText::FromString(Path.ToString()), FSlateIcon(),
								FUIAction(FExecuteAction::CreateLambda([this, WeakDef, WeakStep, Path]()
								{
									if (!WeakDef.IsValid() || !WeakStep.IsValid()) return;
									const FScopedTransaction Transaction(LOCTEXT("SelectPostInput", "Select post-processing input"));
									WeakDef->Modify(); WeakStep->Modify();
									WeakStep->InputSource = EMeshPostInputSource::SelectedMesh;
									WeakStep->InputMesh = TSoftObjectPtr<UObject>(Path);
									WeakDef->MarkPackageDirty(); Refresh();
								})));
						}
						if (!Count) Menu.AddMenuEntry(LOCTEXT("NoSavedPostInput", "No saved outputs this step can use yet"), FText(), FSlateIcon(), FUIAction());
						return Menu.MakeWidget();
					})
				]

				// "A step's output": which step, then which of its runs. Latest by default, so a chain keeps
				// flowing; any earlier run on purpose, to carry on from the one that came out best.
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
				[
					SNew(SHorizontalBox)
					.Visibility_Lambda([WeakStep]()
					{
						return WeakStep.IsValid() && WeakStep->InputSource == EMeshPostInputSource::StepOutput
							? EVisibility::Visible : EVisibility::Collapsed;
					})

					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(STextBlock).Text(LOCTEXT("FromStepLabel", "From step"))
					]

					+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 12.0f, 0.0f)
					[
						SNew(SComboButton)
						.ToolTipText(LOCTEXT("FromStepTip", "Which step's output this step starts from."))
						.ButtonContent()
						[
							SNew(STextBlock)
							.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
							.Text_Lambda([WeakDef, WeakStep]()
							{
								const UMeshDef* D = WeakDef.Get();
								const UMeshPostPipeline* S = WeakStep.Get();
								if (S != nullptr && S->StartsFromMeshStage()) return MeshDefStagesUI::MeshStageSourceLabel(D);
								const TObjectPtr<UMeshPostPipeline>* Source = D && S && S->InputStepId.IsValid()
									? D->PostPipelines.FindByPredicate([S](const UMeshPostPipeline* O) { return O && O->StepId == S->InputStepId; })
									: nullptr;
								return Source ? MeshDefStagesUI::PostStepLabel(D, *Source) : LOCTEXT("ChooseSourceStep", "Choose a step…");
							})
						]
						.OnGetMenuContent_Lambda([this, WeakDef, WeakStep]() -> TSharedRef<SWidget>
						{
							FMenuBuilder Menu(true, nullptr);
							UMeshDef* D = WeakDef.Get();

							// The Mesh stage first: where every chain's first mesh comes from, and the one source
							// the first step has. Its takes are its runs, chosen in the list beside this one.
							if (D != nullptr)
							{
								Menu.AddMenuEntry(MeshDefStagesUI::MeshStageSourceLabel(D),
									LOCTEXT("MeshStageSourceTip", "Start from a mesh the Mesh stage generated - the take selected in "
										"the Takes tab, or one you choose under Run."),
									FSlateIcon(),
									FUIAction(FExecuteAction::CreateLambda([this, WeakDef, WeakStep]()
									{
										if (!WeakDef.IsValid() || !WeakStep.IsValid()) return;
										const FScopedTransaction Transaction(LOCTEXT("SelectMeshStageSource", "Start from the Mesh stage"));
										WeakDef->Modify(); WeakStep->Modify();
										WeakStep->InputStepId = UMeshPostPipeline::MeshStageSourceId;
										WeakStep->InputTakeId.Reset();
										WeakDef->RefreshStaleness();
										WeakDef->MarkPackageDirty();
										Refresh();
									})));
								Menu.AddSeparator();
							}

							int32 Count = 0;
							for (int32 Other = 0; D && Other < D->PostPipelines.Num(); ++Other)
							{
								UMeshPostPipeline* Source = D->PostPipelines[Other];
								if (Source == nullptr || Source == WeakStep.Get()) continue;
								++Count;
								const TWeakObjectPtr<UMeshPostPipeline> WeakSource = Source;
								Menu.AddMenuEntry(MeshDefStagesUI::PostStepLabel(D, Source), FText(), FSlateIcon(),
									FUIAction(FExecuteAction::CreateLambda([this, WeakDef, WeakStep, WeakSource]()
									{
										if (!WeakDef.IsValid() || !WeakStep.IsValid() || !WeakSource.IsValid()) return;
										const FScopedTransaction Transaction(LOCTEXT("SelectSourceStep", "Choose the step to start from"));
										WeakDef->Modify(); WeakStep->Modify();
										if (!WeakSource->StepId.IsValid())
										{
											// A step that never ran has no id yet; it needs one to be pointed at.
											WeakSource->Modify();
											WeakSource->StepId = FGuid::NewGuid();
										}
										WeakStep->InputStepId = WeakSource->StepId;
										WeakStep->InputTakeId.Reset();
										WeakDef->RefreshStaleness();
										WeakDef->MarkPackageDirty();
										Refresh();
									})));
							}
							if (!Count) Menu.AddMenuEntry(LOCTEXT("NoOtherSteps", "No other post-processing steps yet"), FText(), FSlateIcon(), FUIAction());
							return Menu.MakeWidget();
						})
					]

					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(STextBlock).Text(LOCTEXT("FromRunLabel", "Run"))
					]

					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[
						SNew(SComboButton)
						.ToolTipText(LOCTEXT("FromRunTip",
							"Which run of that step. Latest follows it: every time it runs again, this step starts "
							"from the new result. A particular run stays that run."))
						.ButtonContent()
						[
							SNew(STextBlock)
							.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
							.Text_Lambda([WeakDef, WeakStep]()
							{
								const UMeshForgeSubsystem* Sub = UMeshForgeSubsystem::Get();
								const UMeshPostPipeline* S = WeakStep.Get();
								if (S != nullptr && S->StartsFromMeshStage())
								{
									const UMeshDef* D = WeakDef.Get();
									if (D == nullptr || !D->SourceMesh.IsNull()) return LOCTEXT("SourceMeshRun", "The Source Mesh");
									if (S->InputTakeId.IsEmpty())
									{
										const FMeshCandidate* Selected = D->FindSelectedCandidate();
										return Selected
											? FText::Format(LOCTEXT("SelectedTakeIs", "Selected take  ({0})"), MeshDefStagesUI::TakeLabel(*Selected))
											: LOCTEXT("SelectedTakeNone", "Selected take  (none selected)");
									}
									const FMeshCandidate* Take = D->FindCandidate(S->InputTakeId);
									return Take ? MeshDefStagesUI::TakeLabel(*Take) : LOCTEXT("TakeGone", "That take no longer exists");
								}
								if (Sub == nullptr || S == nullptr || !S->InputStepId.IsValid()) return FText::GetEmpty();
								FMeshPostOutput Output;
								const bool bFound = Sub->FindStepOutput(WeakDef.Get(), S->InputStepId, S->InputTakeId, Output);
								if (S->InputTakeId.IsEmpty())
								{
									return bFound
										? FText::Format(LOCTEXT("LatestRunIs", "Latest run  ({0})"), MeshDefStagesUI::PostOutputLabel(Output))
										: LOCTEXT("LatestRunNone", "Latest run  (none yet)");
								}
								return bFound ? MeshDefStagesUI::PostOutputLabel(Output) : LOCTEXT("RunGone", "That run no longer exists");
							})
						]
						.OnGetMenuContent_Lambda([this, WeakDef, WeakStep]() -> TSharedRef<SWidget>
						{
							FMenuBuilder Menu(true, nullptr);
							const UMeshForgeSubsystem* Sub = UMeshForgeSubsystem::Get();
							if (Sub == nullptr || !WeakStep.IsValid() || !WeakStep->InputStepId.IsValid())
							{
								Menu.AddMenuEntry(LOCTEXT("ChooseStepFirst", "Choose a step first"), FText(), FSlateIcon(), FUIAction());
								return Menu.MakeWidget();
							}

							auto Choose = [this, WeakDef, WeakStep](const FString& TakeId)
							{
								return FUIAction(FExecuteAction::CreateLambda([this, WeakDef, WeakStep, TakeId]()
								{
									if (!WeakDef.IsValid() || !WeakStep.IsValid()) return;
									const FScopedTransaction Transaction(LOCTEXT("SelectSourceRun", "Choose the run to start from"));
									WeakDef->Modify(); WeakStep->Modify();
									WeakStep->InputTakeId = TakeId;
									WeakDef->RefreshStaleness();
									WeakDef->MarkPackageDirty();
									Refresh();
								}));
							};

							if (WeakStep->StartsFromMeshStage())
							{
								const UMeshDef* D = WeakDef.Get();
								if (D == nullptr || !D->SourceMesh.IsNull())
								{
									Menu.AddMenuEntry(LOCTEXT("SourceMeshNoTakes", "This definition starts from its Source Mesh, which has no takes"),
										FText(), FSlateIcon(), FUIAction());
									return Menu.MakeWidget();
								}

								Menu.AddMenuEntry(LOCTEXT("SelectedTakeEntry", "Selected take"),
									LOCTEXT("SelectedTakeEntryTip", "Follow the Takes tab: start from whichever take is selected there when the step runs."),
									FSlateIcon(), Choose(FString()));
								Menu.AddSeparator();

								// Newest first, like every other run list. Unfinished and failed takes have no mesh to start from.
								int32 Takes = 0;
								for (int32 Index = D->Candidates.Num() - 1; Index >= 0; --Index)
								{
									const FMeshCandidate& Take = D->Candidates[Index];
									if (!Take.IsUsable()) continue;
									++Takes;
									Menu.AddMenuEntry(MeshDefStagesUI::TakeLabel(Take), FText::FromString(Take.MeshId),
										FSlateIcon(), Choose(Take.MeshId));
								}
								if (!Takes) Menu.AddMenuEntry(LOCTEXT("NoTakesYet", "No finished takes yet - generate one on the Mesh stage"),
									FText(), FSlateIcon(), FUIAction());
								return Menu.MakeWidget();
							}

							Menu.AddMenuEntry(LOCTEXT("LatestRunEntry", "Latest run"),
								LOCTEXT("LatestRunEntryTip", "Follow the step: start from its newest result, including one made earlier in the same chain."),
								FSlateIcon(), Choose(FString()));
							Menu.AddSeparator();

							int32 Count = 0;
							for (const FMeshPostOutput& Output : Sub->GetStepOutputs(WeakDef.Get(), WeakStep->InputStepId))
							{
								if (!MeshDefStagesUI::CanStartFrom(WeakStep.Get(), Output.Asset.ToSoftObjectPath())) continue;
								++Count;
								Menu.AddMenuEntry(MeshDefStagesUI::PostOutputLabel(Output),
									FText::FromString(Output.Asset.ToString()), FSlateIcon(), Choose(Output.TakeId));
							}
							if (!Count) Menu.AddMenuEntry(LOCTEXT("NoRunsYet", "No runs of that step this one can use yet"), FText(), FSlateIcon(), FUIAction());
							return Menu.MakeWidget();
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
				[
					SNew(STextBlock).AutoWrapText(true)
					.Text_Lambda([Reason]() { return FText::FromString(Reason()); })
				]
			]
		];
	}

	const FMeshForgeModule* Module = FMeshForgeModule::GetPtrIfLoaded();
	const TArray<const FMeshForgePostStepType*> Registered =
		Module ? Module->GetPostStepTypes() : TArray<const FMeshForgePostStepType*>();

	if (Classes.Num() == 0 && Registered.Num() == 0)
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

			// Read again rather than captured: a plugin can register while the panel is open.
			const FMeshForgeModule* Live = FMeshForgeModule::GetPtrIfLoaded();
			const TArray<const FMeshForgePostStepType*> Types =
				Live ? Live->GetPostStepTypes() : TArray<const FMeshForgePostStepType*>();

			if (Types.Num() > 0)
			{
				Menu.BeginSection(NAME_None, LOCTEXT("RegisteredPostSteps", "From other plugins"));
				for (const FMeshForgePostStepType* Type : Types)
				{
					Menu.AddMenuEntry(
						Type->DisplayName,
						FText::Format(LOCTEXT("RegisteredEntryTip", "{0}\n\nFrom {1}."),
							Type->Description, FText::FromString(Type->OwningPlugin)),
						FSlateIcon(),
						FUIAction(FExecuteAction::CreateSP(
							this, &SMeshDefStages::AddRegisteredPostStep, Type->Id)));
				}
				Menu.EndSection();
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
	case EMeshStage::Concept:    return LOCTEXT("StageConcept", "Concept image");
	case EMeshStage::References: return LOCTEXT("StageRefs",    "References");
	case EMeshStage::Mesh:       return LOCTEXT("StageMesh",    "Mesh");
	case EMeshStage::Post:       return LOCTEXT("StagePost",    "Post-processing");
	default:                     return LOCTEXT("StageImport",  "Import");
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

		if (Def->GetMeshInput() == EMeshPipelineInput::Text)
		{
			return LOCTEXT("RefsTextOnly",
				"The generator is set to build from words, so no picture is sent. The Images tab is "
				"kept as it is for when it is set back to a picture.");
		}

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
	case EMeshStageStatus::Ready:   return LOCTEXT("StatusReady",   "done");
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
		if (Def->ConceptPipeline->ReadsPrompt() && Def->ConceptPipeline->Prompt.IsEmpty())
		{
			return LOCTEXT("NeedPrompt", "Write what to draw in the prompt below first.");
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
		// **The same refusals the subsystem makes, said before the button rather than after.**
		// A generator set to words needs words and no picture at all, so it is answered first. A main
		// picture is required by every image-reconstructing model, and a multi-view request with nothing
		// but the main picture is multi-view in name only - it comes back looking like an ordinary result
		// that nobody can account for.
		if (Def->GetMeshInput() == EMeshPipelineInput::Text)
		{
			if (Def->GetMeshPrompt().IsEmpty())
			{
				return LOCTEXT("NeedMeshPrompt",
					"This generator is set to build from words and its prompt is empty. Describe the "
					"object in the generator's Prompt.");
			}
		}
		else if (!Def->HasInput() && Def->ConceptImages.Num() == 0 && Def->ReferenceImages.Num() == 0)
		{
			return LOCTEXT("NeedReference", "No reference image yet.");
		}
		else if (Def->ResolveMainImage().IsNull() && Def->SourceImagePath.IsEmpty())
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
			return Def->StageSwitches.bMesh
				? LOCTEXT("NeedAMesh",
					"No mesh to work on yet. Generate one, or set a Source Mesh on the Mesh stage to "
					"post-process a mesh you already have.")
				: LOCTEXT("NeedAStartingMesh", "No mesh to work on yet. Choose it under Starts From, at the top.");
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
	// Only for a definition from before mesh pipelines: with no pipeline to hold them, its mesh stage
	// still sends the definition's own words.
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
