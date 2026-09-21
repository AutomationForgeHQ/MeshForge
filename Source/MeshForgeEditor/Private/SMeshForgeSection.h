// A section of a MeshForge panel: its own box, a header that opens and closes it, and a numbered badge
// that says where that stage or step stands. MotionForge's SMotionSection, ported: the same widget, so
// the two plugins' panels read the same way.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SBox;
class SExpandableArea;

/** Where a stage or step stands, drawn as its badge. */
enum class EMeshSectionState : uint8
{
	/** Nothing to say. */
	None,
	/** Not run yet, or waiting on something earlier. */
	Todo,
	/** The one to do now, or running. */
	Current,
	/** Needs a person: stale, failed, or waiting on an edit. */
	Attention,
	Done
};

class SMeshForgeSection : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SMeshForgeSection)
		: _Number(0)
		, _State(EMeshSectionState::None)
		, _InitiallyExpanded(true)
		, _Inset(false)
	{}
		/** The position in its sequence. Zero draws no badge. */
		SLATE_ARGUMENT(int32, Number)
		SLATE_ATTRIBUTE(FText, Title)
		/** One line in the header beside the title, readable while the section is closed. */
		SLATE_ATTRIBUTE(FText, Summary)
		/** The quiet first line of the body. */
		SLATE_ATTRIBUTE(FText, Subtitle)
		SLATE_ATTRIBUTE(EMeshSectionState, State)
		SLATE_ARGUMENT(bool, InitiallyExpanded)
		/** Sunk into its parent section rather than lifted off the panel: a step inside a stage. */
		SLATE_ARGUMENT(bool, Inset)
		/** Controls at the right end of the header. */
		SLATE_NAMED_SLOT(FArguments, HeaderRight)
		SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_EVENT(FOnBooleanValueChanged, OnExpansionChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetExpanded(bool bExpanded);
	bool IsExpanded() const;

private:

	TSharedPtr<SExpandableArea> Area;
};
