// Every take this definition has ever produced, and what made each one.

#pragma once

#include "CoreMinimal.h"
#include "ForgeLibrary.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class UMeshDef;

/**
 * The Takes tab: the generation history, read from the library rather than from the definition.
 *
 * **Read from disk on purpose.** A definition's candidate list is what it currently believes; the
 * library is what actually happened. A take somebody removed from the list to tidy it is still
 * here, still has its prompt and its price, and can still be imported again - which is the whole
 * argument for the vault existing.
 *
 * The columns are the questions people ask in the order they ask them: when, what made it, how big,
 * how long, what it cost. Everything else is one click away rather than crammed into a row - a take
 * record has thirty fields and a table showing thirty columns is a table nobody reads.
 */
class SMeshDefTakes : public SCompoundWidget
{
public:
	DECLARE_DELEGATE_OneParam(FOnTakeChosen, FString /*TakeId*/);

	SLATE_BEGIN_ARGS(SMeshDefTakes) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UMeshDef>, Definition)

		/** Somebody picked a take to import. The panel does not import; the toolkit does. */
		SLATE_EVENT(FOnTakeChosen, OnImportTake)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Re-read the library. Called when a generation finishes or the asset changes under us. */
	void Refresh();

private:
	TSharedRef<ITableRow> MakeRow(
		TSharedPtr<FForgeTakeRecord> Take, const TSharedRef<STableViewBase>& Owner);

	/** The full record for whichever take is selected, as a details block. */
	TSharedRef<SWidget> BuildDetails();

	TSharedPtr<FForgeTakeRecord> Selected() const;

	FText SummaryText() const;
	FText DetailText() const;
	FText SelectedArtifactState() const;

	bool CanImportSelected() const;
	FReply OnImportClicked();
	FReply OnRevealClicked();

	EVisibility EmptyVisibility() const;
	EVisibility ListVisibility() const;

	TWeakObjectPtr<UMeshDef> Definition;
	FOnTakeChosen OnImportTake;

	TArray<TSharedPtr<FForgeTakeRecord>> Takes;

	TSharedPtr<SListView<TSharedPtr<FForgeTakeRecord>>> List;
};
