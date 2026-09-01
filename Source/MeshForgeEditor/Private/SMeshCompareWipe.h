// Two views of the same framing, with a seam you drag across them.

#pragma once

#include "CoreMinimal.h"
#include "Layout/Margin.h"
#include "Layout/Visibility.h"
#include "Widgets/SCompoundWidget.h"

/**
 * A side-by-side wipe: the left of the seam is A, the right is B, and the seam is draggable.
 *
 * **Both children are laid out at the full width and then clipped**, rather than each being given
 * a share of it. That is the whole trick, and it is what makes the comparison mean anything: a
 * viewport told it is 300 pixels wide renders a different frustum from one told it is 700, so
 * splitting the width puts two differently-framed pictures either side of the seam and invites
 * somebody to read the framing as a difference between the meshes. Clipped, the two pictures are
 * the same picture with a cut in it.
 *
 * **A nested SBox cannot do this and the first attempt at it failed exactly there.** A child is
 * arranged inside whatever its parent was allotted, so a 1400-wide box inside a 700-wide clipper
 * is arranged at 700 and the trick collapses into a plain split - which is what shipped, and what
 * two half-width pistols at two different scales looked like. A constraint canvas is the widget
 * that can place a child outside its own bounds, so that is what does the placing here.
 *
 * The children still have to agree about the camera; that is their business, not this widget's.
 */
class SMeshCompareWipe : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SMeshCompareWipe)
		: _Fraction(0.5f)
	{}

		/** Shown to the left of the seam. */
		SLATE_NAMED_SLOT(FArguments, ASide)

		/** Shown to the right of it. */
		SLATE_NAMED_SLOT(FArguments, BSide)

		/** Where the seam starts, 0 to 1. */
		SLATE_ARGUMENT(float, Fraction)

	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/**
	 * Show one view or two.
	 *
	 * Off, A takes the whole width and B is collapsed - which also stops B ticking and rendering,
	 * so a second preview scene costs nothing while nobody is comparing anything.
	 */
	void SetCompare(bool bInCompare);

	bool IsComparing() const { return bCompare; }

	/** Put the seam back in the middle. */
	void Recentre() { Fraction = 0.5f; }

private:

	/** Width of the whole widget as it was last laid out, or zero before the first paint. */
	float FullWidth() const;

	FOptionalSize LeftSize() const;
	FOptionalSize RightSize() const;

	/** Where each child is placed inside its clipper: full width, and B shifted left by the seam. */
	FMargin ASideOffset() const;
	FMargin BSideOffset() const;

	/** Space before the handle, so the handle sits centred on the seam. */
	FOptionalSize HandleOffset() const;

	/** Visible for B's clipper; the seam row wants SelfHitTestInvisible instead. */
	EVisibility CompareVisibility() const;

	/**
	 * The seam row, which sits above both viewports.
	 *
	 * **SelfHitTestInvisible, and the first version was not.** Visible, the row covered the whole
	 * viewport and swallowed every click, so nothing could be orbited while comparing - the mouse
	 * never reached the viewport underneath at all. Only the handle inside it is hit-testable.
	 */
	EVisibility SeamVisibility() const;

	/** Called by the handle as it is dragged, in absolute (screen) coordinates. */
	void OnSeamDragged(const FVector2D& ScreenPosition);

	/** 0 at the left edge, 1 at the right. Clamped away from the edges so neither side vanishes. */
	float Fraction = 0.5f;

	bool bCompare = false;
};
