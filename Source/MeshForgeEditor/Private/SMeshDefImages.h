// Every picture this definition has, one large, and which of them the mesh model will be shown.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateBrush.h"
#include "Widgets/SCompoundWidget.h"
#include "MeshForgeImageSlots.h"

class UMeshDef;
class UTexture2D;
class STextBlock;

/** Where a picture came from. Shown beside it, because it changes how much to trust it. */
enum class EMeshDefImageOrigin : uint8
{
	/** Brought in by hand - a photograph, a render, something from an art bible. */
	Added,

	/** Drawn by the concept stage. */
	Drawn,

	/** Produced by a refinement pipeline from one of the others. */
	Refined,
};

/** One entry in the gallery: a texture, where it came from, and what job it currently has. */
struct FMeshDefImageEntry
{
	TWeakObjectPtr<UTexture2D> Texture;

	/** The soft pointer as the definition stores it, which is what selection compares against. */
	TSoftObjectPtr<UTexture2D> Path;

	EMeshDefImageOrigin Origin = EMeshDefImageOrigin::Drawn;

	/** "Drawn 2", "Added". Shown under the picture. */
	FText Label;

	/** True when this is the picture the mesh model is shown. */
	bool bIsMain = false;

	/** Its position in ExtraViews, or INDEX_NONE. */
	int32 ViewIndex = INDEX_NONE;

	/**
	 * The brush the strip draws with, owned here.
	 *
	 * Owned by the entry and not by the widget that builds it, because SImage keeps a raw pointer:
	 * a brush made as a local inside the build function is destroyed the moment that function
	 * returns, and the widget then draws through freed memory. This lives as long as the gallery.
	 */
	TSharedPtr<FSlateBrush> Thumbnail;

	/**
	 * The same texture at preview size. A separate brush because a brush carries its own size.
	 *
	 * A picture drawn moments ago looks soft here for a few seconds after the editor opens. That is
	 * Unreal compiling the texture, not this panel picking a small mip - it resolves on its own, and
	 * forcing mips resident does nothing for it. Worth knowing before anybody spends an afternoon on
	 * the streaming system, as somebody already has.
	 */
	TSharedPtr<FSlateBrush> Large;
};

/**
 * The Images tab: one picture large, the rest as a filmstrip beneath it.
 *
 * **Large, because the picture is the thing being judged.** This is where somebody decides whether
 * a reference is worth reconstructing from - whether the latches read, whether the shadow will
 * become geometry - and none of that is visible in a 168-pixel tile. It was a grid of those first,
 * and every judgement meant opening the texture in a separate asset editor.
 *
 * **One strip rather than a row per source, because the mesh model does not care.** A photograph
 * somebody dropped in, a picture the concept stage drew and a cut-out a refinement pipeline
 * produced are all just pictures at the moment one of them is chosen, and separating them into
 * three lists made the actual question - *which one* - harder to answer rather than easier.
 *
 * **Choosing is the whole interaction.** A single-view generator takes one picture and everything
 * else here is a candidate; a multi-view one takes that picture plus up to three more angles. The
 * strip says which is which, and how many more the current generator can actually use - because
 * three extra views silently ignored looks exactly like three extra views used badly.
 *
 * Everything here is a real `UTexture2D` in the project. That is deliberate: a viewer full of
 * pictures that are not assets cannot be dragged into a material, cannot be found again through the
 * Content Browser, and quietly teaches people that generated work lives somewhere outside the
 * project. Unreal keeps the original bytes in the asset besides, so sending one back to a provider
 * loses nothing to block compression.
 */
class SMeshDefImages : public SCompoundWidget
{
public:
	/** Something here changed what the pipeline would do, so the other panels are now out of date. */
	DECLARE_DELEGATE(FOnSelectionChanged);

	SLATE_BEGIN_ARGS(SMeshDefImages) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UMeshDef>, Definition)
		SLATE_EVENT(FOnSelectionChanged, OnSelectionChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Re-read the definition. Called when a stage finishes or the asset changes under us. */
	void Refresh();

private:
	TSharedRef<SWidget> BuildHeader();
	TSharedRef<SWidget> BuildPreview();
	TSharedRef<SWidget> BuildThumbnail(TSharedPtr<FMeshDefImageEntry> Entry, int32 Index);


	/** The asset picker behind "Add picture", filtered to textures. */
	TSharedRef<SWidget> BuildAddMenu();

	void OnAddImage(const FAssetData& Asset);
	void OnSetMain(TSoftObjectPtr<UTexture2D> Image);
	void OnToggleView(TSoftObjectPtr<UTexture2D> Image);
	void OnShowInContentBrowser(TWeakObjectPtr<UTexture2D> Texture);

	/**
	 * Take a picture out of this definition's gallery. The texture asset is left alone.
	 *
	 * Removing and deleting are different decisions and this is only the first: a reference that
	 * turned out to be wrong for *this* prop is often right for another, and a picture that cost
	 * credits to draw should not be destroyed by tidying a list. Deleting the asset stays where
	 * deleting assets belongs, in the Content Browser.
	 */
	void OnRemoveImage(TSoftObjectPtr<UTexture2D> Image);

	/**
	 * Put a picture in a specific view slot, or take it out of one.
	 *
	 * Slots rather than a list, because the vendor's first image is the front view and the rest are
	 * positions somebody chose - "add as view" gave no way to say *which* view, and no way to swap
	 * two without removing both.
	 */
	void OnSetView(TSoftObjectPtr<UTexture2D> Image, int32 Slot);

	/** The right-click menu for one picture: its role, and what else can be done to it. */
	TSharedRef<SWidget> BuildRoleMenu(TSoftObjectPtr<UTexture2D> Image);

	/** Which view slot this picture is in, or INDEX_NONE. */
	int32 ViewSlotOf(const TSoftObjectPtr<UTexture2D>& Image) const;

	/**
	 * How many views the *format* allows, whatever the current generator will read.
	 *
	 * The slot strip on the generator asks the same question, so the number lives in one place.
	 */
	static constexpr int32 MaxViewSlots = MeshForgeImageSlots::MaxViewSlots;

	/** Move the preview along the strip. Wraps, so one arrow walks the whole gallery. */
	FReply Step(int32 Delta);

	/** The entry the preview is showing, or null when there are none. */
	TSharedPtr<FMeshDefImageEntry> Current() const;

	/** What the current generator will actually be shown, as a sentence. */
	FText SelectionSummary() const;

	FText PreviewCaption() const;
	FText PreviewRole() const;
	FSlateColor PreviewRoleColour() const;
	const FSlateBrush* PreviewBrush() const;
	FText PreviewPosition() const;

	FText MainButtonText() const;
	FText ViewButtonText() const;
	bool CanSetMain() const;
	bool CanToggleView() const;
	EVisibility ViewButtonVisibility() const;

	EVisibility EmptyVisibility() const;
	EVisibility ContentVisibility() const;

	/** Arrow keys walk the strip, which is what anybody tries first. */
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

	/**
	 * Pictures dropped in from Explorer or from the Content Browser.
	 *
	 * A reference somebody already has is the most common way a definition gets its first picture,
	 * and every route to it was a dialogue: find the file, import it as a texture, find the texture,
	 * press Add. Dropping it is the thing people try first, and it did nothing.
	 *
	 * A file on disk is ingested into this definition's own image folder, so it lands where every
	 * other picture for this prop lands rather than wherever the Content Browser happened to be.
	 */
	/**
	 * A picture being dragged out of the gallery and onto a slot on the Mesh stage.
	 *
	 * Armed by whichever thumbnail was pressed and answered here, because DetectDrag needs a widget
	 * that will still be asked for the operation - and the thumbnails are borders built in a loop,
	 * not widgets of their own.
	 */
	virtual FReply OnDragDetected(const FGeometry& Geometry, const FPointerEvent& Event) override;

	virtual void OnDragEnter(const FGeometry& Geometry, const FDragDropEvent& Event) override;
	virtual void OnDragLeave(const FDragDropEvent& Event) override;
	virtual FReply OnDragOver(const FGeometry& Geometry, const FDragDropEvent& Event) override;
	virtual FReply OnDrop(const FGeometry& Geometry, const FDragDropEvent& Event) override;

private:
	/** True while something droppable is over the panel, so the border can say so. */
	bool bDragHovered = false;

	/** The picture the last left-press was on, and what the gallery calls it. */
	TSoftObjectPtr<UTexture2D> DragCandidate;
	FText DragCandidateLabel;

	/** Add these textures to the gallery. Returns how many were new. */
	int32 AddTextures(const TArray<UTexture2D*>& Textures);

	/** Import these files as textures into this definition's image folder. */
	int32 AddFiles(const TArray<FString>& Paths);

	/** Whether a drag carries anything this panel can take. */
	static bool CarriesPictures(const FDragDropEvent& Event);

	TWeakObjectPtr<UMeshDef> Definition;
	FOnSelectionChanged OnSelectionChanged;

	TArray<TSharedPtr<FMeshDefImageEntry>> Entries;

	/**
	 * Which one the preview shows. Not the same thing as the main image, deliberately: comparing a
	 * candidate against the one currently chosen is the whole reason to look at this panel, and a
	 * preview that snapped back to the chosen picture would make that impossible.
	 */
	int32 PreviewIndex = 0;

	TSharedPtr<class SHorizontalBox> Strip;
	TSharedPtr<class SComboButton> AddButton;
};
