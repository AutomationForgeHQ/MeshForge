#include "SMeshCompareWipe.h"

#include "Styling/AppStyle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MeshForgeEditor"

namespace MeshCompareWipePrivate
{
	/** How wide the grab band is. Wider than the line it draws, because a 2px target is a fight. */
	static constexpr float HandleWidth = 12.0f;

	/** How close to an edge the seam may get. Below this one side is a sliver nobody can read. */
	static constexpr float EdgeMargin = 0.04f;

	DECLARE_DELEGATE_OneParam(FOnSeamDragged, const FVector2D&);

	/**
	 * The grab band on the seam.
	 *
	 * Its own widget for one reason: a drag needs OnMouseMove, and the parent never sees the press
	 * at all - the viewports underneath handle mouse input for the orbit camera, so anything that
	 * wants the seam has to be above them and has to capture.
	 */
	class SWipeHandle : public SCompoundWidget
	{
	public:

		SLATE_BEGIN_ARGS(SWipeHandle) {}
			SLATE_EVENT(FOnSeamDragged, OnDragged)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			OnDragged = InArgs._OnDragged;

			SetCursor(EMouseCursor::ResizeLeftRight);

			ChildSlot
			[
				SNew(SBox)
				.WidthOverride(HandleWidth)
				.HAlign(HAlign_Center)
				[
					SNew(SBox)
					.WidthOverride(2.0f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
						.BorderBackgroundColor(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.65f)))
					]
				]
			];
		}

		virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event) override
		{
			return (Event.GetEffectingButton() == EKeys::LeftMouseButton)
				? FReply::Handled().CaptureMouse(SharedThis(this))
				: FReply::Unhandled();
		}

		virtual FReply OnMouseMove(const FGeometry&, const FPointerEvent& Event) override
		{
			if (!HasMouseCapture())
			{
				return FReply::Unhandled();
			}

			OnDragged.ExecuteIfBound(FVector2D(Event.GetScreenSpacePosition()));
			return FReply::Handled();
		}

		virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent& Event) override
		{
			return HasMouseCapture()
				? FReply::Handled().ReleaseMouseCapture()
				: FReply::Unhandled();
		}

	private:

		FOnSeamDragged OnDragged;
	};

	/** The label over each half, so nobody has to remember which side is which. */
	static TSharedRef<SWidget> SideLabel(const FText& Text)
	{
		return SNew(STextBlock)
			.Text(Text)
			.Visibility(EVisibility::HitTestInvisible)
			.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			.ColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.5f)));
	}
}

void SMeshCompareWipe::Construct(const FArguments& InArgs)
{
	Fraction = FMath::Clamp(InArgs._Fraction,
		MeshCompareWipePrivate::EdgeMargin, 1.0f - MeshCompareWipePrivate::EdgeMargin);

	// A canvas rather than a box, because only a canvas will place a child outside its own bounds -
	// which is exactly what "lay it out at the full width and show a slice of it" means.
	auto Clipped = [](TSharedRef<SWidget> Content, TAttribute<FMargin> Offset)
		-> TSharedRef<SWidget>
	{
		return SNew(SConstraintCanvas)

			+ SConstraintCanvas::Slot()
			.Anchors(FAnchors(0.0f, 0.0f, 0.0f, 1.0f))   // pinned left, stretched vertically
			.Alignment(FVector2D(0.0f, 0.0f))
			.Offset(Offset)
			[
				Content
			];
	};

	ChildSlot
	[
		SNew(SOverlay)

		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		[
			SNew(SBox)
			.WidthOverride(this, &SMeshCompareWipe::LeftSize)
			.Clipping(EWidgetClipping::ClipToBounds)
			[
				Clipped(InArgs._ASide.Widget,
					TAttribute<FMargin>::CreateSP(this, &SMeshCompareWipe::ASideOffset))
			]
		]

		// B is the same width and shifted left by exactly the seam, so its picture lands where A's
		// does. Anything else and the seam shows a jump rather than a cut.
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		[
			SNew(SBox)
			.WidthOverride(this, &SMeshCompareWipe::RightSize)
			.Visibility(this, &SMeshCompareWipe::CompareVisibility)
			.Clipping(EWidgetClipping::ClipToBounds)
			[
				Clipped(InArgs._BSide.Widget,
					TAttribute<FMargin>::CreateSP(this, &SMeshCompareWipe::BSideOffset))
			]
		]

		// The seam, above both. Self-hit-test-invisible, so only the band itself takes the mouse
		// and everywhere else still reaches the viewport underneath.
		+ SOverlay::Slot()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			.Visibility(this, &SMeshCompareWipe::SeamVisibility)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
			[
				SNew(SBox)
				.WidthOverride(this, &SMeshCompareWipe::HandleOffset)
				.HAlign(HAlign_Right)
				.Padding(FMargin(0.0f, 0.0f, 10.0f, 0.0f))
				.Visibility(EVisibility::HitTestInvisible)
				[
					MeshCompareWipePrivate::SideLabel(LOCTEXT("WipeA", "A"))
				]
			]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(MeshCompareWipePrivate::SWipeHandle)
				.OnDragged(MeshCompareWipePrivate::FOnSeamDragged::CreateSP(
					this, &SMeshCompareWipe::OnSeamDragged))
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Top)
			[
				SNew(SBox)
				.HAlign(HAlign_Left)
				.Padding(FMargin(10.0f, 0.0f, 0.0f, 0.0f))
				.Visibility(EVisibility::HitTestInvisible)
				[
					MeshCompareWipePrivate::SideLabel(LOCTEXT("WipeB", "B"))
				]
			]
		]
	];
}

void SMeshCompareWipe::SetCompare(bool bInCompare)
{
	bCompare = bInCompare;
}

float SMeshCompareWipe::FullWidth() const
{
	// The last laid-out size. Zero before the first paint, which resolves itself on the next one -
	// asking the geometry is the only way to know, and a comparison that is one frame late is not
	// a comparison anybody notices.
	return GetTickSpaceGeometry().GetLocalSize().X;
}

FOptionalSize SMeshCompareWipe::LeftSize() const
{
	const float Width = FullWidth();

	// Unset rather than zero, so the child fills its parent normally until the geometry is known.
	if (Width <= 1.0f)
	{
		return FOptionalSize();
	}

	return bCompare ? FOptionalSize(Width * Fraction) : FOptionalSize(Width);
}

FOptionalSize SMeshCompareWipe::RightSize() const
{
	const float Width = FullWidth();

	return (Width > 1.0f)
		? FOptionalSize(Width * (1.0f - Fraction))
		: FOptionalSize();
}

FMargin SMeshCompareWipe::ASideOffset() const
{
	// Left, Top, then Width and Height - which is what the last two mean on a slot whose anchors
	// are degenerate in X and stretched in Y. So: at x=0, as wide as the whole widget, full height.
	return FMargin(0.0f, 0.0f, FMath::Max(FullWidth(), 1.0f), 0.0f);
}

FMargin SMeshCompareWipe::BSideOffset() const
{
	const float Width = FMath::Max(FullWidth(), 1.0f);

	// B's clipper starts at the seam, so its child starts that far to the *left* of it - which puts
	// B's picture in the same place on screen as A's.
	return FMargin(-Width * Fraction, 0.0f, Width, 0.0f);
}

FOptionalSize SMeshCompareWipe::HandleOffset() const
{
	const float Width = FullWidth();

	return FOptionalSize(FMath::Max(0.0f,
		Width * Fraction - MeshCompareWipePrivate::HandleWidth * 0.5f));
}

EVisibility SMeshCompareWipe::CompareVisibility() const
{
	return bCompare ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SMeshCompareWipe::SeamVisibility() const
{
	return bCompare ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed;
}

void SMeshCompareWipe::OnSeamDragged(const FVector2D& ScreenPosition)
{
	const FGeometry& Geometry = GetTickSpaceGeometry();
	const float Width = Geometry.GetLocalSize().X;

	if (Width <= 1.0f)
	{
		return;
	}

	const float Local = static_cast<float>(Geometry.AbsoluteToLocal(ScreenPosition).X);

	Fraction = FMath::Clamp(Local / Width,
		MeshCompareWipePrivate::EdgeMargin, 1.0f - MeshCompareWipePrivate::EdgeMargin);
}

#undef LOCTEXT_NAMESPACE
