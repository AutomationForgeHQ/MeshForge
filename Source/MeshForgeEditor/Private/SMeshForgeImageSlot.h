// One picture slot on a generator: what is in it, and how a picture gets there.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class UMeshDef;
class UTexture2D;
struct FSlateBrush;

/**
 * A slot the generator will read: the main picture, or one of the extra views.
 *
 * **A drop target and a drag source, both.** Reading which pictures a model will be sent was the
 * first job of this strip and choosing them stayed in the gallery - which was right while the only
 * way to choose was a right-click menu, and became the long way round the moment there were four
 * slots to fill. Dragging a picture onto the slot you want it in is what people try first, it says
 * what it means without a menu, and it is the same gesture a post-processing chain will need for
 * feeding one step's output into the next.
 *
 * Dragging between two slots swaps a picture's position, because assignment takes it out of
 * whatever slot it was in before writing the new one.
 *
 * **A slot refuses a picture from another definition.** Everything downstream hashes and re-imports
 * from this definition's own image list; a picture from outside it would be sent once and be
 * unaccounted for afterwards. The gallery is where a picture joins the list.
 */
class SMeshForgeImageSlot : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SMeshForgeImageSlot)
		: _SlotIndex(INDEX_NONE)
		, _Size(84.0f)
		, _AcceptsDrops(true)
	{}

		SLATE_ARGUMENT(TWeakObjectPtr<UMeshDef>, Definition)

		/** Below zero for the main picture; 0..2 for the extra views, in send order. */
		SLATE_ARGUMENT(int32, SlotIndex)

		SLATE_ARGUMENT(float, Size)

		/** False draws the slot but takes no drops - for a strip shown while a job is running. */
		SLATE_ARGUMENT(bool, AcceptsDrops)

	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:

	/** True for the main picture, which is the one whose absence stops a generation. */
	bool IsMain() const { return SlotIndex < 0; }

	/** "main image", "view 2". */
	FText Caption() const;

	const FSlateBrush* Border() const;

	/** Tinted rather than swapped for another brush, so the highlight cannot depend on a style name. */
	FSlateColor BorderColour() const;

	/** The right-click menu: empty the slot, or find the texture. */
	FReply ShowSlotMenu(const FPointerEvent& Event);

	//~ Dragging a picture out of this slot.
	virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnDragDetected(const FGeometry& Geometry, const FPointerEvent& Event) override;

	//~ Dropping one in.
	virtual void OnDragEnter(const FGeometry& Geometry, const FDragDropEvent& Event) override;
	virtual void OnDragLeave(const FDragDropEvent& Event) override;
	virtual FReply OnDragOver(const FGeometry& Geometry, const FDragDropEvent& Event) override;
	virtual FReply OnDrop(const FGeometry& Geometry, const FDragDropEvent& Event) override;

	/** Whether a drag carries a picture from this definition, which is the only kind a slot takes. */
	bool WillTake(const FDragDropEvent& Event) const;

	TWeakObjectPtr<UMeshDef> Definition;

	int32 SlotIndex = INDEX_NONE;
	float Size = 84.0f;
	bool bAcceptsDrops = true;

	/** What was in the slot when this widget was built. */
	TSoftObjectPtr<UTexture2D> Held;

	/** Owned here: SImage keeps a raw pointer and a local brush is freed before it is drawn. */
	TSharedPtr<FSlateBrush> Thumbnail;

	/** True while a picture this slot would take is over it. */
	bool bDropHovered = false;
};
