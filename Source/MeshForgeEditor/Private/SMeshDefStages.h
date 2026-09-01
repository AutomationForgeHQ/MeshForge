// The five stages, what each has done, what it will use, and the button that runs it.

#pragma once

#include "CoreMinimal.h"
#include "MeshForgeTypes.h"
#include "Widgets/SCompoundWidget.h"

class UMeshDef;
class UMeshForgePipeline;
class IDetailsView;

/**
 * The Stages tab: the pipeline as a column of rows, one per stage.
 *
 * The panel this plugin most needed and did not have. A definition used to be a details panel of
 * thirty properties in which the two questions somebody actually has - *what has run* and *what
 * will running this cost me* - were nowhere, and the answer to the second was found by pressing the
 * button.
 *
 * Every row says four things: what the stage is, what it would use, what state it is in, and what
 * it will cost. A stage that cannot run says why in the place where the button would be, because a
 * disabled button without an explanation is worse than no button.
 *
 * **A stage that takes one pipeline draws that pipeline's own settings inside its row.** This is
 * the whole reason pipelines are typed C++ classes rather than a bag of strings: choosing the local
 * image pipeline puts a model, a size and a step count on the row, and choosing the Meshy one
 * replaces them with a model, an aspect ratio and a pose. Neither list is written here. Both come
 * from the same reflected properties an agent reads, so the two surfaces cannot drift apart.
 *
 * **Everything about running the stage sits in a bar at the foot of the row.** The button used to
 * be in a column on the right, vertically centred against a row whose height is decided by a
 * details panel - so on a stage with a dozen settings the button floated somewhere in the middle
 * of the right-hand edge, and any message that took its place had a 260-pixel column to say
 * itself in. The bar is the width of the row, it is at the end of the row the way a dialog's
 * buttons are at the end of a dialog, and the message next to the button has room to be a
 * sentence.
 */
class SMeshDefStages : public SCompoundWidget
{
public:
	DECLARE_DELEGATE_OneParam(FOnRunStage, EMeshStage);

	SLATE_BEGIN_ARGS(SMeshDefStages) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UMeshDef>, Definition)
		SLATE_EVENT(FOnRunStage, OnRunStage)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Re-read the definition. Called when a stage finishes or the asset changes under us. */
	void Refresh();

private:
	TSharedRef<SWidget> BuildStageRow(EMeshStage Stage);

	/**
	 * The bar at the foot of a stage: what running it would do or why it cannot, what it costs, and
	 * the button.
	 *
	 * **Every part of it is a bound attribute rather than a decision made while building.** The row
	 * is rebuilt when a pipeline setting changes, but nothing rebuilds it when a job finishes on a
	 * worker thread - so a baked button was hidden for as long as it took something else to refresh
	 * the panel, and the bar said a run was still going after it had landed. Bound, the bar is
	 * correct on the next frame whatever changed it.
	 */
	TSharedRef<SWidget> BuildActionBar(EMeshStage Stage);

	/**
	 * The picker and the chosen pipeline's settings, for a stage that takes exactly one.
	 *
	 * Only Concept and Mesh. References and Post take an ordered list where the order changes the
	 * result, and a list editor that could reorder, disable and delete would be a different widget
	 * rather than a wider version of this one - so those two stay in the Settings tab until they
	 * are executable and there is something to reorder.
	 */
	TSharedRef<SWidget> BuildPipelinePicker(EMeshStage Stage);

	/** Every concrete pipeline class this stage could use, in display-name order. */
	TArray<UClass*> PipelineClassesFor(EMeshStage Stage) const;

	/** The pipeline currently on that stage, or null. */
	UMeshForgePipeline* PipelineFor(EMeshStage Stage) const;

	/** Instance a pipeline of this class onto the stage, or clear it when null. */
	void SetPipeline(EMeshStage Stage, UClass* Class);

	/**
	 * Append a post-processing step of this class.
	 *
	 * Separate from SetPipeline because post is a *chain* - a prop is often assembled from several
	 * vendors, and choosing one step must not throw away the one before it.
	 */
	void AddPostStep(UClass* Class);

	/** Remove the step at this position. */
	void RemovePostStep(int32 Index);

	/** The Post stage's own controls: what is in the chain, and how to add to it. */
	TSharedRef<SWidget> BuildPostChain();

	/** One details view per post step, kept between refreshes so open categories stay open. */
	TArray<TSharedPtr<IDetailsView>> PostDetails;

	FText StageName(EMeshStage Stage) const;
	FText StageSummary(EMeshStage Stage) const;
	FText StatusText(EMeshStage Stage) const;
	FSlateColor StatusColour(EMeshStage Stage) const;
	FText CostText(EMeshStage Stage) const;

	/** Hides the cost line on a stage that costs nothing, rather than leaving a blank row. */
	EVisibility CostVisibility(EMeshStage Stage) const;

	/** "Draw", "Generate", "Import" - what this stage's button actually does. */
	FText RunLabel(EMeshStage Stage) const;

	/**
	 * The one line beside the button: why the stage cannot run, or what pressing it would do.
	 *
	 * The same place either way, because it answers the same question. A bar that is empty until
	 * something is wrong teaches people to read it only when it is complaining.
	 */
	FText ActionMessage(EMeshStage Stage) const;

	/**
	 * What running this stage would actually do, in a sentence.
	 *
	 * Written for somebody who has not decided yet. It says where the result goes, because the
	 * thing nobody guesses about this plugin is that generating does not put anything in the
	 * project - importing does.
	 */
	FText ActionHint(EMeshStage Stage) const;

	/** Full strength for a refusal, subdued for a description, the status blue while running. */
	FSlateColor ActionMessageColour(EMeshStage Stage) const;

	/** Empty when the stage can run; otherwise the reason it cannot, shown beside the button. */
	FText BlockedReason(EMeshStage Stage) const;

	bool CanRun(EMeshStage Stage) const;

	/** Whether a job for this stage is in flight right now. */
	bool IsRunningNow(EMeshStage Stage) const;

	/** The button is there only while the stage can actually be started. */
	EVisibility RunVisibility(EMeshStage Stage) const;

	/** The spinner is there only while something is in flight. */
	EVisibility BusyVisibility(EMeshStage Stage) const;

	/**
	 * Rebuild the rows when a pipeline setting changes.
	 *
	 * Bound to every pipeline details view. Without it a rule that has just started applying - "this
	 * vendor cannot texture a model it splits into parts" - is not shown until something else
	 * happens to refresh the panel, so the Run button stays live on a request the vendor will refuse.
	 */
	void OnPipelineEdited(const FPropertyChangedEvent& Event);
	FReply OnRunClicked(EMeshStage Stage);

	/**
	 * The prompt, given its own editor at the top rather than a one-line box in a property grid.
	 *
	 * It is the field with the most effect on the result and the one people rewrite most often, and
	 * a details panel gave it the same single line it gave a lightmap resolution. Committed on
	 * focus loss rather than per keystroke, so a rewrite is one undo rather than forty.
	 */
	FText GetPrompt() const;
	void OnPromptCommitted(const FText& NewText, ETextCommit::Type CommitType);

	TSharedPtr<class SMultiLineEditableTextBox> PromptBox;

	/**
	 * One details view per single-pipeline stage, kept between refreshes.
	 *
	 * Rebuilt rather than recreated: a details view remembers which categories are expanded, and
	 * making a new one every time a stage finished collapsed everything somebody had opened.
	 */
	TMap<EMeshStage, TSharedPtr<IDetailsView>> PipelineDetails;

	TWeakObjectPtr<UMeshDef> Definition;
	FOnRunStage OnRunStage;
	TSharedPtr<class SVerticalBox> Rows;
};
