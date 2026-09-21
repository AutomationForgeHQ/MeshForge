// MeshForge's visual vocabulary, shared by its panels. The same house style as MotionForge's (a lifted,
// edged box per section, colour only where it carries a state) - copied rather than shared, because the
// plugins install and load on their own and one must never need the other.

#pragma once

#include "CoreMinimal.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/StyleColors.h"

namespace MeshForgeStyle
{
	inline const FLinearColor Good()  { return FLinearColor(0.30f, 0.78f, 0.45f); }
	inline const FLinearColor Warn()  { return FLinearColor(0.95f, 0.65f, 0.20f); }
	inline const FLinearColor Bad()   { return FLinearColor(0.90f, 0.36f, 0.36f); }
	inline const FLinearColor Quiet() { return FLinearColor(0.32f, 0.32f, 0.34f); }
	inline const FLinearColor Info()  { return FLinearColor(0.36f, 0.62f, 0.95f); }

	/**
	 * The box a section sits in: lifted off the panel, with an edge.
	 *
	 * `ToolPanel.GroupBorder` is within a shade of the panel behind it in the current editor theme, so the
	 * Stages tab read as one long column. Theme colours rather than fixed ones, so it follows the theme.
	 */
	inline const FSlateBrush* SectionBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FStyleColors::Header, 6.f, FStyleColors::Hover, 1.f);
		return &Brush;
	}

	/** A section inside a section - a post step inside Post-processing: sunk into it rather than lifted. */
	inline const FSlateBrush* InsetBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FStyleColors::Recessed, 5.f, FStyleColors::Hover, 1.f);
		return &Brush;
	}

	/** A round badge, tinted by whoever draws it. */
	inline const FSlateBrush* BadgeBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor::White, 11.f);
		return &Brush;
	}
}
