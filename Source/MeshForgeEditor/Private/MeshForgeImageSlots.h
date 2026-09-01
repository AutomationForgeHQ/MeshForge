// Which picture has which job, and the drag that moves one into a slot.

#pragma once

#include "CoreMinimal.h"
#include "Input/DragAndDrop.h"
#include "UObject/SoftObjectPtr.h"

class UMeshDef;
class UTexture2D;
struct FSlateBrush;

/**
 * The one implementation of "this picture is now the main image" / "this picture is view 2".
 *
 * There are two surfaces that assign a picture a job - the gallery's right-click menu and the slot
 * strip on the generator - and the rules are not obvious: a picture cannot be both the subject and
 * one of its own angles, moving it from view 2 to view 1 must not leave it in both, and slot 3 has
 * to stay slot 3 when slot 2 is empty. Written twice, those rules drift, and the way you find out
 * is a generation that sent the same picture twice.
 */
namespace MeshForgeImageSlots
{
	/**
	 * How many extra views the *format* allows, whatever the current generator will read.
	 *
	 * Three, because the endpoints that take several pictures take four and the first of them is
	 * the main image. What a given model will actually read is UMeshDef::MaxExtraViews, which is a
	 * different and smaller question.
	 */
	static constexpr int32 MaxViewSlots = 3;

	/** Make this the picture the generator reconstructs from. Takes it out of any view slot. */
	void AssignMain(UMeshDef* Def, const TSoftObjectPtr<UTexture2D>& Image);

	/** Put this picture in a numbered view slot, taking it out of whichever one it was in. */
	void AssignView(UMeshDef* Def, const TSoftObjectPtr<UTexture2D>& Image, int32 Slot);

	/** Empty a slot. Slot below zero means the main image. */
	void ClearSlot(UMeshDef* Def, int32 Slot);

	/** What is in a slot right now, or null. Slot below zero means the main image. */
	TSoftObjectPtr<UTexture2D> InSlot(const UMeshDef* Def, int32 Slot);

	/**
	 * Tell every open panel that this definition changed - on the next tick, not now.
	 *
	 * Deferred deliberately. The panels answer by rebuilding themselves, and one of the callers is
	 * a drop handler running inside the very widget that would be torn down: destroying the widget
	 * that is currently dispatching an event is how this plugin has crashed before.
	 */
	void NotifyEdited(UMeshDef* Def);

	/**
	 * A square brush that fills its slot rather than squashing the picture into it.
	 *
	 * Reference pictures are rarely square - a 1448x1086 photograph in a square slot is either
	 * letterboxed or distorted, and a distorted thumbnail is one somebody cannot recognise. The UV
	 * region crops to the centre instead.
	 *
	 * The brush is returned rather than stored because SImage keeps a raw pointer to it: a brush
	 * made as a local and handed over is freed the moment the function returns, and the widget
	 * then draws through it.
	 */
	TSharedRef<FSlateBrush> MakeCroppedBrush(UTexture2D* Texture, float Size);
}

/**
 * A picture being dragged from the gallery to a slot, or from one slot to another.
 *
 * Carries the definition as well as the picture, so a slot can refuse a drag that came from some
 * other definition's gallery rather than quietly adopting a picture that is not in this one's
 * image list - everything downstream hashes and re-imports from that list, and a picture outside
 * it would be sent once and be unaccounted for afterwards.
 */
class FMeshForgeImageDragDropOp : public FDragDropOperation
{
public:

	DRAG_DROP_OPERATOR_TYPE(FMeshForgeImageDragDropOp, FDragDropOperation)

	static TSharedRef<FMeshForgeImageDragDropOp> New(
		UMeshDef* InDefinition, const TSoftObjectPtr<UTexture2D>& InImage, const FText& InLabel);

	virtual TSharedPtr<SWidget> GetDefaultDecorator() const override;

	TWeakObjectPtr<UMeshDef> Definition;

	TSoftObjectPtr<UTexture2D> Image;

	/** "Drawn 2", "Added" - what the gallery calls this picture. */
	FText Label;

private:

	/** Owned here for the life of the drag, for the same reason every other brush in this plugin is. */
	TSharedPtr<FSlateBrush> Thumbnail;
};
