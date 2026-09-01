#include "SMeshForgeJobs.h"

#include "MeshForgeSubsystem.h"
#include "MeshForgeTypes.h"

#include "Styling/AppStyle.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MeshForgeEditor"

namespace MeshForgeJobsPrivate
{
	static FSlateColor ColourFor(EMeshForgeJobState State)
	{
		switch (State)
		{
		case EMeshForgeJobState::Succeeded: return FSlateColor(FLinearColor(0.35f, 0.75f, 0.40f));
		case EMeshForgeJobState::Failed:    return FSlateColor(FLinearColor(0.90f, 0.35f, 0.30f));
		default:                            return FSlateColor(FLinearColor(0.40f, 0.65f, 0.95f));
		}
	}

	/** "42s", or "2m 14s" once a job has been going long enough for seconds to stop meaning much. */
	static FText Duration(float Seconds)
	{
		const int32 Whole = FMath::Max(0, FMath::FloorToInt(Seconds));

		return (Whole < 60)
			? FText::Format(LOCTEXT("JobSeconds", "{0}s"), FText::AsNumber(Whole))
			: FText::Format(LOCTEXT("JobMinutes", "{0}m {1}s"),
				FText::AsNumber(Whole / 60), FText::AsNumber(Whole % 60));
	}
}

void SMeshForgeJobs::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SBorder)
		.Visibility(this, &SMeshForgeJobs::StripVisibility)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(10.0f, 8.0f))
		[
			SAssignNew(Rows, SVerticalBox)
		]
	];

	Refresh();
}

void SMeshForgeJobs::Refresh()
{
	if (!Rows.IsValid())
	{
		return;
	}

	Rows->ClearChildren();

	const UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get();
	if (Subsystem == nullptr)
	{
		return;
	}

	const TArray<FMeshForgeJob> Jobs = Subsystem->GetJobs();

	if (Jobs.Num() == 0)
	{
		return;
	}

	TSharedRef<SHorizontalBox> Heading = SNew(SHorizontalBox);

	Heading->AddSlot().FillWidth(1.0f).VAlign(VAlign_Center)
	[
		SNew(STextBlock)
		.Text(LOCTEXT("JobsHeading", "Work in progress"))
		.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
	];

	Heading->AddSlot().AutoWidth()
	[
		SNew(SButton)
		.Text(LOCTEXT("ClearJobs", "Clear finished"))
		.ToolTipText(LOCTEXT("ClearJobsTip",
			"Forget the jobs that have already finished. Anything still running is left alone."))
		.OnClicked_Lambda([this]()
		{
			if (UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get())
			{
				Subsystem->ClearFinishedJobs();
			}
			return FReply::Handled();
		})
	];

	Rows->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)[ Heading ];

	for (const FMeshForgeJob& Job : Jobs)
	{
		const bool bRunning = (Job.State == EMeshForgeJobState::Running);
		const FGuid JobId = Job.Id;

		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);

		// A throbber only while it is actually turning. A static one beside a finished job is the
		// kind of thing that makes somebody wait for work that is already done.
		if (bRunning)
		{
			Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SCircularThrobber).Radius(8.0f)
			];
		}

		Row->AddSlot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("JobLine", "{0} - {1}"),
				FText::FromString(Job.DefinitionName), FText::FromString(Job.Label)))
			.AutoWrapText(true)
		];

		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			// Bound rather than baked, so a running job's clock advances without the strip being
			// rebuilt - rebuilding once a second would fight with anybody trying to click in it.
			.Text_Lambda([JobId]()
			{
				const UMeshForgeSubsystem* Live = UMeshForgeSubsystem::Get();

				FMeshForgeJob Current;

				return (Live && Live->FindJob(JobId, Current))
					? MeshForgeJobsPrivate::Duration(Current.Elapsed())
					: FText::GetEmpty();
			})
			.ColorAndOpacity(MeshForgeJobsPrivate::ColourFor(Job.State))
		];

		Rows->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 2.0f)[ Row ];

		if (Job.State == EMeshForgeJobState::Failed && !Job.Error.IsEmpty())
		{
			Rows->AddSlot().AutoHeight().Padding(24.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Job.Error))
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
		}
	}
}

EVisibility SMeshForgeJobs::StripVisibility() const
{
	const UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get();

	// NumJobs, not GetJobs. This is read every frame, and GetJobs copies the list and sorts it.
	return (Subsystem && Subsystem->NumJobs() > 0)
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

#undef LOCTEXT_NAMESPACE
