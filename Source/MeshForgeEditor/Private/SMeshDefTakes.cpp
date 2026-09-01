#include "SMeshDefTakes.h"

#include "MeshDef.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"

#define LOCTEXT_NAMESPACE "MeshForgeEditor"

namespace MeshDefTakesPrivate
{
	static const FName ColWhen(TEXT("When"));
	static const FName ColProvider(TEXT("Provider"));
	static const FName ColSize(TEXT("Size"));
	static const FName ColTook(TEXT("Took"));
	static const FName ColCost(TEXT("Cost"));
	static const FName ColState(TEXT("State"));

	static FText Duration(float Seconds)
	{
		const int32 Whole = FMath::Max(0, FMath::FloorToInt(Seconds));

		return (Whole < 60)
			? FText::Format(LOCTEXT("TakeSeconds", "{0}s"), FText::AsNumber(Whole))
			: FText::Format(LOCTEXT("TakeMinutes", "{0}m {1}s"),
				FText::AsNumber(Whole / 60), FText::AsNumber(Whole % 60));
	}

	static FText Megabytes(int64 Bytes)
	{
		return (Bytes <= 0)
			? FText::GetEmpty()
			: FText::Format(LOCTEXT("TakeMB", "{0} MB"),
				FText::AsNumber(FMath::RoundToInt(Bytes / (1024.0 * 1024.0))));
	}
}

void SMeshDefTakes::Construct(const FArguments& InArgs)
{
	Definition   = InArgs._Definition;
	OnImportTake = InArgs._OnImportTake;

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(12.0f, 12.0f, 12.0f, 8.0f)
		[
			SNew(STextBlock)
			.Text(this, &SMeshDefTakes::SummaryText)
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot().FillHeight(0.55f).Padding(12.0f, 0.0f, 12.0f, 0.0f)
		[
			SNew(SOverlay)

			+ SOverlay::Slot()
			[
				SAssignNew(List, SListView<TSharedPtr<FForgeTakeRecord>>)
				.Visibility(this, &SMeshDefTakes::ListVisibility)
				.ListItemsSource(&Takes)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SMeshDefTakes::MakeRow)
				.HeaderRow(
					SNew(SHeaderRow)

					+ SHeaderRow::Column(MeshDefTakesPrivate::ColWhen)
						.DefaultLabel(LOCTEXT("ColWhen", "When"))
						.FillWidth(0.26f)

					+ SHeaderRow::Column(MeshDefTakesPrivate::ColProvider)
						.DefaultLabel(LOCTEXT("ColProvider", "Made by"))
						.FillWidth(0.28f)

					+ SHeaderRow::Column(MeshDefTakesPrivate::ColSize)
						.DefaultLabel(LOCTEXT("ColSize", "Size"))
						.FillWidth(0.12f)

					+ SHeaderRow::Column(MeshDefTakesPrivate::ColTook)
						.DefaultLabel(LOCTEXT("ColTook", "Took"))
						.FillWidth(0.12f)

					+ SHeaderRow::Column(MeshDefTakesPrivate::ColCost)
						.DefaultLabel(LOCTEXT("ColCost", "Cost"))
						.FillWidth(0.14f)

					+ SHeaderRow::Column(MeshDefTakesPrivate::ColState)
						.DefaultLabel(LOCTEXT("ColState", "On disk"))
						.FillWidth(0.08f))
			]

			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SNew(SBox).MaxDesiredWidth(460.0f)
				[
					SNew(STextBlock)
					.Visibility(this, &SMeshDefTakes::EmptyVisibility)
					.Text(LOCTEXT("NoTakes",
						"Nothing generated yet.\n\nEvery take this definition produces is filed under "
						"Forge/Library with what made it - the prompt, the pictures, the model, the "
						"settings and the price - and stays there whether or not it is imported."))
					.Justification(ETextJustify::Center)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(12.0f, 8.0f)
		[
			SNew(SSeparator)
		]

		+ SVerticalBox::Slot().FillHeight(0.45f).Padding(12.0f, 0.0f, 12.0f, 12.0f)
		[
			BuildDetails()
		]
	];

	Refresh();
}

TSharedRef<SWidget> SMeshDefTakes::BuildDetails()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SMeshDefTakes::SelectedArtifactState)
				.AutoWrapText(true)
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("UseThisTake", "Use this take"))
				.IsEnabled(this, &SMeshDefTakes::CanImportSelected)
				.ToolTipText(LOCTEXT("UseThisTakeTip",
					"Import this take and make it the definition's mesh, replacing whatever is there. "
					"Free - the artifact is already on this disk, so no provider is involved and "
					"nothing is billed."))
				.OnClicked(this, &SMeshDefTakes::OnImportClicked)
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("RevealTake", "Show files"))
				.ToolTipText(LOCTEXT("RevealTakeTip",
					"Open this take's folder in the file browser - the artifact, its record, and any "
					"input that was copied in."))
				.OnClicked(this, &SMeshDefTakes::OnRevealClicked)
			]
		]

		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(10.0f, 8.0f))
			[
				SNew(SScrollBox)

				+ SScrollBox::Slot()
				[
					SNew(STextBlock)
					.Text(this, &SMeshDefTakes::DetailText)
					.AutoWrapText(true)
				]
			]
		];
}

void SMeshDefTakes::Refresh()
{
	Takes.Reset();

	if (const UMeshDef* Def = Definition.Get())
	{
		for (const FForgeTakeRecord& Record : FForgeLibrary::ListTakes(TEXT("MeshForge"), Def->GetName()))
		{
			Takes.Add(MakeShared<FForgeTakeRecord>(Record));
		}
	}

	if (List.IsValid())
	{
		List->RequestListRefresh();

		// Nothing selected reads as an empty details block, which looks broken next to a list that
		// plainly has rows in it.
		if (Takes.Num() > 0 && List->GetNumItemsSelected() == 0)
		{
			List->SetSelection(Takes[0]);
		}
	}
}

TSharedRef<ITableRow> SMeshDefTakes::MakeRow(
	TSharedPtr<FForgeTakeRecord> Take, const TSharedRef<STableViewBase>& Owner)
{
	class SRow : public SMultiColumnTableRow<TSharedPtr<FForgeTakeRecord>>
	{
	public:
		SLATE_BEGIN_ARGS(SRow) {}
			SLATE_ARGUMENT(TSharedPtr<FForgeTakeRecord>, Take)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner)
		{
			Take = InArgs._Take;
			SMultiColumnTableRow::Construct(FSuperRowType::FArguments(), Owner);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			FText Text;
			FSlateColor Colour = FSlateColor::UseForeground();

			if (Column == MeshDefTakesPrivate::ColWhen)
			{
				Text = (Take->FinishedUtc == FDateTime::MinValue())
					? LOCTEXT("TakeUnknownTime", "unknown")
					: FText::AsDateTime(Take->FinishedUtc);
			}
			else if (Column == MeshDefTakesPrivate::ColProvider)
			{
				Text = Take->ModelId.IsEmpty()
					? FText::FromString(Take->ProviderId)
					: FText::Format(LOCTEXT("TakeProvider", "{0} · {1}"),
						FText::FromString(Take->ProviderId), FText::FromString(Take->ModelId));
			}
			else if (Column == MeshDefTakesPrivate::ColSize)
			{
				Text = MeshDefTakesPrivate::Megabytes(Take->OutputBytes);
			}
			else if (Column == MeshDefTakesPrivate::ColTook)
			{
				Text = MeshDefTakesPrivate::Duration(Take->Seconds);
			}
			else if (Column == MeshDefTakesPrivate::ColCost)
			{
				// **Money where money can be said, and the amount rather than the unit.** This cell
				// used to print CostCurrency - the word "credits" - and never the number beside it,
				// so every take in the ledger read "~credits" and the column was decoration.
				//
				// Dollars first, because credits are not comparable between vendors: thirty of one
				// and thirty of another are different amounts of money, and adding a column up is
				// the only reason to have one. Marked approximate, because a published rate is not
				// an invoice.
				if (Take->CostUsd > 0.0)
				{
					Text = FText::Format(LOCTEXT("TakeCostUsd", "~${0}"),
						FText::AsNumber(Take->CostUsd,
							&FNumberFormattingOptions().SetMinimumFractionalDigits(2)
													   .SetMaximumFractionalDigits(2)));
				}
				else if (Take->CostAmount > 0.0)
				{
					// A vendor that publishes no rate. Named in its own unit rather than converted
					// on a guess - a wrong number stops somebody looking it up, a blank does not.
					Text = FText::Format(LOCTEXT("TakeCostCredits", "{0} {1}"),
						FText::AsNumber(static_cast<int32>(Take->CostAmount)),
						FText::FromString(Take->CostCurrency.IsEmpty()
							? TEXT("credits") : Take->CostCurrency));
				}
				else
				{
					Text = Take->bLocal
						? LOCTEXT("TakeFree", "free")
						: LOCTEXT("TakeCostUnknown", "not reported");
				}
			}
			else
			{
				// Failure first: a take that never produced anything is not "cleaned", and calling
				// it that hides the one row somebody most needs to see when adding up a bill.
				if (Take->Status == TEXT("failed"))
				{
					Text = LOCTEXT("TakeFailed", "failed");
					return SNew(SBox).Padding(FMargin(4.0f, 2.0f)).VAlign(VAlign_Center)
						[
							SNew(STextBlock).Text(Text)
							.ColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.45f, 0.35f)))
						];
				}

				const bool bHere = Take->HasArtifact();

				Text = bHere ? LOCTEXT("TakeOnDisk", "yes") : LOCTEXT("TakeCleaned", "cleaned");
				Colour = bHere
					? FSlateColor(FLinearColor(0.35f, 0.75f, 0.40f))
					: FSlateColor::UseSubduedForeground();
			}

			return SNew(SBox).Padding(FMargin(6.0f, 3.0f)).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Text).ColorAndOpacity(Colour)
			];
		}

	private:
		TSharedPtr<FForgeTakeRecord> Take;
	};

	return SNew(SRow, Owner).Take(Take);
}

TSharedPtr<FForgeTakeRecord> SMeshDefTakes::Selected() const
{
	if (!List.IsValid())
	{
		return nullptr;
	}

	const TArray<TSharedPtr<FForgeTakeRecord>> Chosen = List->GetSelectedItems();
	return (Chosen.Num() > 0) ? Chosen[0] : nullptr;
}

FText SMeshDefTakes::SummaryText() const
{
	if (Takes.Num() == 0)
	{
		return LOCTEXT("TakesNone", "No takes filed yet.");
	}

	int32 OnDisk = 0;

	for (const TSharedPtr<FForgeTakeRecord>& Take : Takes)
	{
		OnDisk += Take->HasArtifact() ? 1 : 0;
	}

	return FText::Format(
		LOCTEXT("TakesSummary",
			"{0} take(s) in Forge/Library, {1} still on this disk. A cleaned take keeps its record "
			"and can be fetched again where the provider's ids are stable."),
		FText::AsNumber(Takes.Num()), FText::AsNumber(OnDisk));
}

FText SMeshDefTakes::SelectedArtifactState() const
{
	const TSharedPtr<FForgeTakeRecord> Take = Selected();

	if (!Take.IsValid())
	{
		return FText::GetEmpty();
	}

	return Take->HasArtifact()
		? FText::Format(LOCTEXT("TakeReady", "Take {0} is on this disk."),
			FText::FromString(Take->TakeId))
		: FText::Format(
			LOCTEXT("TakeGone",
				"Take {0}'s file has been cleaned from Forge/Library. Its record is intact, so it "
				"can be fetched again from {1} while that task still exists there."),
			FText::FromString(Take->TakeId), FText::FromString(Take->ProviderId));
}

FText SMeshDefTakes::DetailText() const
{
	const TSharedPtr<FForgeTakeRecord> Take = Selected();

	if (!Take.IsValid())
	{
		return LOCTEXT("TakePickOne", "Select a take to see what made it.");
	}

	// A block of text rather than a grid of widgets. Every field here is a label and a value, the
	// list is long and mostly read top to bottom, and a details view of a plain struct would spend
	// two thirds of the width on expander arrows.
	TArray<FString> Lines;

	Lines.Add(FString::Printf(TEXT("Take          %s"), *Take->TakeId));
	Lines.Add(FString::Printf(TEXT("Provider      %s%s"), *Take->ProviderId,
		Take->ModelId.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("  ·  %s"), *Take->ModelId)));

	if (!Take->Endpoint.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("Endpoint      %s"), *Take->Endpoint));
	}

	Lines.Add(FString::Printf(TEXT("Ran           %s"),
		Take->bLocal ? TEXT("on this machine") : TEXT("on the provider's hardware")));

	if (Take->CostAmount > 0.0 || Take->CostUsd > 0.0)
	{
		FString Cost;

		if (Take->CostAmount > 0.0)
		{
			Cost = FString::Printf(TEXT("%d %s"), static_cast<int32>(Take->CostAmount),
				Take->CostCurrency.IsEmpty() ? TEXT("credits") : *Take->CostCurrency);
		}

		if (Take->CostUsd > 0.0)
		{
			Cost += Cost.IsEmpty()
				? FString::Printf(TEXT("about $%.2f"), Take->CostUsd)
				: FString::Printf(TEXT("  -  about $%.2f"), Take->CostUsd);
		}
		else if (!Take->bLocal)
		{
			Cost += TEXT("  -  this provider publishes no rate, so there is no dollar figure");
		}

		Lines.Add(FString::Printf(TEXT("Cost          %s%s"), *Cost,
			Take->bCostEstimated ? TEXT("  (our estimate, not a receipt)") : TEXT("")));
	}

	if (Take->Status == TEXT("failed"))
	{
		Lines.Add(TEXT(""));
		Lines.Add(FString::Printf(TEXT("FAILED        %s"),
			Take->Error.IsEmpty() ? TEXT("no reason recorded") : *Take->Error));
		Lines.Add(TEXT("              Recorded because a failed take can still have been charged."));
	}

	Lines.Add(TEXT(""));

	if (!Take->Prompt.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("Prompt        %s"), *Take->Prompt));
		Lines.Add(TEXT(""));
	}

	for (const FForgeTakeInput& Input : Take->Inputs)
	{
		const FString Where = Input.AssetPath.IsEmpty() ? Input.File : Input.AssetPath;

		Lines.Add(FString::Printf(TEXT("%-13s %s"), *Input.Role, *Where));

		if (!Input.Hash.IsEmpty())
		{
			Lines.Add(FString::Printf(TEXT("              blake3 %s"), *Input.Hash.Left(16)));
		}
	}

	if (Take->Settings.Num() > 0)
	{
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("Settings sent"));

		TArray<FString> Keys;
		Take->Settings.GetKeys(Keys);
		Keys.Sort();

		for (const FString& Key : Keys)
		{
			Lines.Add(FString::Printf(TEXT("  %-24s %s"), *Key, *Take->Settings[Key]));
		}
	}

	Lines.Add(TEXT(""));
	Lines.Add(FString::Printf(TEXT("Output        %s  ·  %s"),
		*Take->OutputFile, *MeshDefTakesPrivate::Megabytes(Take->OutputBytes).ToString()));

	if (!Take->OutputHash.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("              blake3 %s"), *Take->OutputHash.Left(16)));
	}

	if (Take->Processing.Num() > 0)
	{
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("Since then"));

		for (const FForgeProcessingEvent& Event : Take->Processing)
		{
			Lines.Add(FString::Printf(TEXT("  %s  %s"),
				*Event.StepId.ToString(), *Event.Summary));
		}
	}

	Lines.Add(TEXT(""));
	Lines.Add(FString::Printf(TEXT("Engine        %s  ·  %s"),
		*Take->EngineVersion, *Take->ToolVersion));
	Lines.Add(FString::Printf(TEXT("Folder        %s"), *Take->Directory));

	return FText::FromString(FString::Join(Lines, TEXT("\n")));
}

bool SMeshDefTakes::CanImportSelected() const
{
	const TSharedPtr<FForgeTakeRecord> Take = Selected();
	return Take.IsValid() && Take->HasArtifact();
}

FReply SMeshDefTakes::OnImportClicked()
{
	if (const TSharedPtr<FForgeTakeRecord> Take = Selected())
	{
		OnImportTake.ExecuteIfBound(Take->TakeId);
	}

	return FReply::Handled();
}

FReply SMeshDefTakes::OnRevealClicked()
{
	if (const TSharedPtr<FForgeTakeRecord> Take = Selected())
	{
		if (!Take->Directory.IsEmpty())
		{
			FPlatformProcess::ExploreFolder(*Take->Directory);
		}
	}

	return FReply::Handled();
}

EVisibility SMeshDefTakes::ListVisibility() const
{
	return Takes.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SMeshDefTakes::EmptyVisibility() const
{
	return Takes.Num() > 0 ? EVisibility::Collapsed : EVisibility::Visible;
}

#undef LOCTEXT_NAMESPACE
