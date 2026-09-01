// The window a Mesh Definition opens into.

#pragma once

#include "CoreMinimal.h"
#include "MeshForgeTypes.h"
#include "Toolkits/AssetEditorToolkit.h"

class IDetailsView;
class SMeshDefImages;
class SMeshDefPreview;
class SMeshDefStages;
class UMeshDef;

/**
 * Four tabs over one definition: the stages, the settings, the pictures and the mesh.
 *
 * A definition is a pipeline with five steps, four of which spend money or GPU time, and it used to
 * open in a details panel that gave equal weight to a prompt and a lightmap resolution. The two
 * questions somebody authoring one actually has are *what has run* and *what will this cost*, and
 * neither had anywhere to be answered.
 *
 * The tabs exist because the three things being looked at cannot share a pane. Pictures want a grid
 * and a mesh wants a viewport; putting either inside a details panel makes both useless.
 *
 * It owns no pipeline logic. Every button calls UMeshForgeSubsystem, so nothing is reachable from
 * this window that an agent cannot reach from a tool call - which is the rule the rest of this
 * family keeps and the reason the agent surface never rots.
 */
class MESHFORGEEDITOR_API FMeshDefEditorToolkit : public FAssetEditorToolkit
{
public:

	static const FName ToolkitName;
	static const FName StagesTabId;
	static const FName DetailsTabId;
	static const FName ImagesTabId;
	static const FName PreviewTabId;
	static const FName TakesTabId;

	void Initialise(EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& Host, UMeshDef* Def);

	// FAssetEditorToolkit
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;

	// IToolkit
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;

private:

	TSharedRef<SDockTab> SpawnStagesTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnDetailsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnImagesTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnPreviewTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnTakesTab(const FSpawnTabArgs& Args);

	/** Import a take from the library, replacing whatever the definition currently shows. */
	void ImportTake(FString TakeId);

	/** One stage, run through the subsystem. The panel decides nothing; it only asks. */
	void RunStage(EMeshStage Stage);

	/** Re-read the definition into every tab. */
	void RefreshAll();

	virtual void OnClose() override;

	TWeakObjectPtr<UMeshDef> Definition;

	TSharedPtr<IDetailsView>    DetailsView;
	TSharedPtr<SMeshDefStages>  StagesPanel;
	TSharedPtr<SMeshDefImages>  ImagesPanel;
	TSharedPtr<SMeshDefPreview> PreviewPanel;
	TSharedPtr<class SMeshForgeJobs> JobsPanel;
	TSharedPtr<class SMeshDefTakes>  TakesPanel;

	/**
	 * Our subscription to the subsystem's job list.
	 *
	 * Held so it can be removed on the way out: the subsystem outlives this toolkit, and a delegate
	 * left bound to a destroyed panel is a crash the next time anything draws.
	 */
	FDelegateHandle JobsChangedHandle;

	/** Somebody edited this definition from a surface that cannot refresh the other tabs. */
	void OnDefinitionEdited(UMeshDef* Edited);

	FDelegateHandle DefinitionEditedHandle;
};
