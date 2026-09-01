// What MeshForge is doing right now, and what it just finished.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/**
 * The job strip: one line per piece of background work, newest and still-running first.
 *
 * **It exists because the work moved off the game thread and became invisible.** While drawing ran
 * inline the editor froze, which was intolerable but at least unambiguous - something was clearly
 * happening. Started in the background it takes two minutes during which nothing on screen changes,
 * and a person who has pressed a button and seen nothing happen presses it again.
 *
 * So this shows every job across every definition, not only the one whose panel is open: two
 * definitions drawing at once share a GPU, and the second one being slow is explained entirely by
 * the first one existing.
 *
 * Collapses to nothing when there is nothing to say, so a panel that is not doing anything does not
 * spend a strip of height saying so.
 */
class SMeshForgeJobs : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMeshForgeJobs) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Re-read the job list. Called when the subsystem says something moved. */
	void Refresh();

private:
	EVisibility StripVisibility() const;

	TSharedPtr<class SVerticalBox> Rows;
};
