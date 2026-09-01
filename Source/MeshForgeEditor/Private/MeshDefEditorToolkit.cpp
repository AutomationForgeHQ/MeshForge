#include "MeshDefEditorToolkit.h"

#include "MeshDef.h"
#include "SMeshDefImages.h"
#include "SMeshDefPreview.h"
#include "SMeshDefStages.h"
#include "SMeshForgeJobs.h"
#include "SMeshDefTakes.h"
#include "MeshForgeSubsystem.h"

#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Widgets/Docking/SDockTab.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "MeshForgeEditor"

const FName FMeshDefEditorToolkit::ToolkitName(TEXT("MeshDefEditor"));
const FName FMeshDefEditorToolkit::StagesTabId(TEXT("MeshDefEditor_Stages"));
const FName FMeshDefEditorToolkit::DetailsTabId(TEXT("MeshDefEditor_Details"));
const FName FMeshDefEditorToolkit::ImagesTabId(TEXT("MeshDefEditor_Images"));
const FName FMeshDefEditorToolkit::PreviewTabId(TEXT("MeshDefEditor_Preview"));
const FName FMeshDefEditorToolkit::TakesTabId(TEXT("MeshDefEditor_Takes"));

void FMeshDefEditorToolkit::Initialise(
	EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& Host, UMeshDef* Def)
{
	Definition = Def;

	FPropertyEditorModule& PropertyEditor =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	FDetailsViewArgs Args;
	Args.bAllowSearch = true;
	Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;

	DetailsView = PropertyEditor.CreateDetailView(Args);
	DetailsView->SetObject(Def);

	// The version string is load-bearing. Unreal remembers a layout by name, so a saved one from
	// before this changed comes back with the old arrangement and the change looks like it did not
	// happen. Bumping the name is the only thing that discards it.
	const TSharedRef<FTabManager::FLayout> Layout =
		FTabManager::NewLayout("MeshDefEditor_v4")
		->AddArea(
			FTabManager::NewPrimaryArea()
			->SetOrientation(Orient_Horizontal)

			// The work on the left, the controls on the right. A generated mesh is the thing being
			// judged, and reading order puts what is being judged first - the same arrangement the
			// Static Mesh and Material editors use, so it costs nobody a second to learn.
			//
			// The mesh and the pictures share a stack, so switching between "what did it make" and
			// "what was it shown" is one click and they never fight for width.
			->Split(
				FTabManager::NewStack()
				->SetSizeCoefficient(0.62f)
				->AddTab(PreviewTabId, ETabState::OpenedTab)
				->AddTab(ImagesTabId, ETabState::OpenedTab)
				->AddTab(TakesTabId, ETabState::OpenedTab)
				->SetForegroundTab(PreviewTabId)
			)

			// Stages and Settings share a stack rather than splitting the column between them.
			//
			// They were stacked vertically until each stage grew its own pipeline settings, at which
			// point Stages needed the whole height and Settings was left showing half a category.
			// Sharing costs one click to reach the settings, and the settings that matter most - the
			// ones belonging to the chosen pipeline - are now on the stage row itself, so that click
			// is for the finish and import options rather than for anything mid-flow.
			->Split(
				FTabManager::NewStack()
				->SetSizeCoefficient(0.38f)
				->AddTab(StagesTabId, ETabState::OpenedTab)
				->AddTab(DetailsTabId, ETabState::OpenedTab)
				->SetForegroundTab(StagesTabId)
			)
		);

	InitAssetEditor(Mode, Host, ToolkitName, Layout,
		/*bCreateDefaultStandaloneMenu*/ true, /*bCreateDefaultToolbar*/ true, Def);

	// The work happens in the background now, so the panel has to be told when it lands. Without
	// this a finished draw sat in the project with the stage still reading "running" until somebody
	// clicked something.
	if (UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get())
	{
		JobsChangedHandle = Subsystem->OnJobsChanged.AddSP(this, &FMeshDefEditorToolkit::RefreshAll);

		// An edit made in one tab is usually visible in another - the picture a generator will be
		// sent is shown both in the gallery and on the Mesh stage - and a panel can only refresh
		// itself. This is also how an edit made over MCP reaches an editor somebody has open.
		DefinitionEditedHandle = Subsystem->OnDefinitionEdited.AddSP(
			this, &FMeshDefEditorToolkit::OnDefinitionEdited);
	}
}

void FMeshDefEditorToolkit::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	const TSharedRef<FWorkspaceItem> Group =
		InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu", "Mesh Definition"));

	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	InTabManager->RegisterTabSpawner(StagesTabId,
		FOnSpawnTab::CreateSP(this, &FMeshDefEditorToolkit::SpawnStagesTab))
		.SetDisplayName(LOCTEXT("StagesTab", "Stages"))
		.SetGroup(Group);

	InTabManager->RegisterTabSpawner(DetailsTabId,
		FOnSpawnTab::CreateSP(this, &FMeshDefEditorToolkit::SpawnDetailsTab))
		.SetDisplayName(LOCTEXT("DetailsTab", "Settings"))
		.SetGroup(Group);

	InTabManager->RegisterTabSpawner(ImagesTabId,
		FOnSpawnTab::CreateSP(this, &FMeshDefEditorToolkit::SpawnImagesTab))
		.SetDisplayName(LOCTEXT("ImagesTab", "Images"))
		.SetGroup(Group);

	InTabManager->RegisterTabSpawner(PreviewTabId,
		FOnSpawnTab::CreateSP(this, &FMeshDefEditorToolkit::SpawnPreviewTab))
		.SetDisplayName(LOCTEXT("PreviewTab", "Mesh"))
		.SetGroup(Group);

	InTabManager->RegisterTabSpawner(TakesTabId,
		FOnSpawnTab::CreateSP(this, &FMeshDefEditorToolkit::SpawnTakesTab))
		.SetDisplayName(LOCTEXT("TakesTab", "Takes"))
		.SetGroup(Group);
}

void FMeshDefEditorToolkit::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(StagesTabId);
	InTabManager->UnregisterTabSpawner(DetailsTabId);
	InTabManager->UnregisterTabSpawner(ImagesTabId);
	InTabManager->UnregisterTabSpawner(PreviewTabId);
	InTabManager->UnregisterTabSpawner(TakesTabId);
}

TSharedRef<SDockTab> FMeshDefEditorToolkit::SpawnStagesTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("StagesTab", "Stages"))
		[
			SNew(SVerticalBox)

			// The job strip above the stages, and collapsed when there are none. What is running is
			// the first thing somebody wants after pressing a button that returns immediately.
			+ SVerticalBox::Slot().AutoHeight().Padding(12.0f, 12.0f, 12.0f, 0.0f)
			[
				SAssignNew(JobsPanel, SMeshForgeJobs)
			]

			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SAssignNew(StagesPanel, SMeshDefStages)
				.Definition(Definition)
				.OnRunStage(SMeshDefStages::FOnRunStage::CreateSP(this, &FMeshDefEditorToolkit::RunStage))
			]
		];
}

TSharedRef<SDockTab> FMeshDefEditorToolkit::SpawnDetailsTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTab", "Settings"))
		[
			DetailsView.ToSharedRef()
		];
}

TSharedRef<SDockTab> FMeshDefEditorToolkit::SpawnImagesTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("ImagesTab", "Images"))
		[
			SAssignNew(ImagesPanel, SMeshDefImages)
			.Definition(Definition)
			.OnSelectionChanged(SMeshDefImages::FOnSelectionChanged::CreateSP(
				this, &FMeshDefEditorToolkit::RefreshAll))
		];
}

TSharedRef<SDockTab> FMeshDefEditorToolkit::SpawnPreviewTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("PreviewTab", "Mesh"))
		[
			SAssignNew(PreviewPanel, SMeshDefPreview).Definition(Definition)
		];
}

TSharedRef<SDockTab> FMeshDefEditorToolkit::SpawnTakesTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("TakesTab", "Takes"))
		[
			SAssignNew(TakesPanel, SMeshDefTakes)
			.Definition(Definition)
			.OnImportTake(SMeshDefTakes::FOnTakeChosen::CreateSP(
				this, &FMeshDefEditorToolkit::ImportTake))
		];
}

void FMeshDefEditorToolkit::ImportTake(FString TakeId)
{
	UMeshDef* Def = Definition.Get();
	UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get();

	if (Def == nullptr || Subsystem == nullptr || TakeId.IsEmpty())
	{
		return;
	}

	// Through the library rather than the candidate list. A take shown in this tab is one the
	// library holds, and half of them - every post-processed one - were never candidates, so asking
	// the definition about them answered "no finished take to import" for takes plainly on disk.
	const FMeshImportOutcome Outcome = Subsystem->ImportTake(Def, TakeId);

	FNotificationInfo Info(Outcome.bSuccess
		? FText::Format(LOCTEXT("TakeImported", "Imported: {0} triangles, {1} LODs."),
			FText::AsNumber(Outcome.TriangleCount), FText::AsNumber(Outcome.LodCount))
		: FText::FromString(Outcome.Error));

	Info.ExpireDuration = Outcome.bSuccess ? 6.0f : 12.0f;
	Info.bUseSuccessFailIcons = true;

	if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Item->SetCompletionState(
			Outcome.bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	}

	RefreshAll();
}

void FMeshDefEditorToolkit::RunStage(EMeshStage Stage)
{
	UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return;
	}

	switch (Stage)
	{
	case EMeshStage::Concept:
		// Returns immediately. What happened to it appears on the stage and in the job strip.
		Def->DrawConceptImage();
		break;

	case EMeshStage::Mesh:
		Def->GenerateNow();
		break;

	case EMeshStage::Post:
		Def->RunPostNow();
		break;

	case EMeshStage::Import:
		Def->RefinishNow();
		break;

	default:
	{
		// Said out loud rather than silently doing nothing. References is designed and not yet
		// executable, and a Run button that appears to work and does not is worse than one that
		// explains itself.
		FNotificationInfo Info(FText::Format(
			LOCTEXT("StageNotWired",
				"The {0} stage is not wired up yet. Its settings are saved on the definition and "
				"the panel reads them; nothing runs it."),
			StaticEnum<EMeshStage>()->GetDisplayNameTextByValue(static_cast<int64>(Stage))));
		Info.ExpireDuration = 6.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
		break;
	}
	}

	RefreshAll();
}

void FMeshDefEditorToolkit::OnClose()
{
	if (UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get())
	{
		Subsystem->OnJobsChanged.Remove(JobsChangedHandle);
		Subsystem->OnDefinitionEdited.Remove(DefinitionEditedHandle);
	}

	JobsChangedHandle.Reset();
	DefinitionEditedHandle.Reset();

	FAssetEditorToolkit::OnClose();
}

void FMeshDefEditorToolkit::OnDefinitionEdited(UMeshDef* Edited)
{
	// Only ours. The delegate is on the subsystem, so every open definition editor hears every
	// edit made to any of them.
	if (Edited != nullptr && Edited == Definition.Get())
	{
		RefreshAll();
	}
}

void FMeshDefEditorToolkit::RefreshAll()
{
	if (JobsPanel.IsValid())    { JobsPanel->Refresh();    }
	if (TakesPanel.IsValid())   { TakesPanel->Refresh();   }
	if (StagesPanel.IsValid())  { StagesPanel->Refresh();  }
	if (ImagesPanel.IsValid())  { ImagesPanel->Refresh();  }
	if (PreviewPanel.IsValid()) { PreviewPanel->Refresh(); }
}

FName FMeshDefEditorToolkit::GetToolkitFName() const
{
	return ToolkitName;
}

FText FMeshDefEditorToolkit::GetBaseToolkitName() const
{
	return LOCTEXT("BaseToolkitName", "Mesh Definition");
}

FString FMeshDefEditorToolkit::GetWorldCentricTabPrefix() const
{
	return LOCTEXT("TabPrefix", "Mesh ").ToString();
}

FLinearColor FMeshDefEditorToolkit::GetWorldCentricTabColorScale() const
{
	return FLinearColor(0.3f, 0.55f, 0.75f, 0.5f);
}

#undef LOCTEXT_NAMESPACE
