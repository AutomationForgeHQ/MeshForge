// Copyright Blackcode SA. All rights reserved.

#include "MeshForgeFactories.h"

#include "MeshDef.h"
#include "MeshForgeSettings.h"
#include "MeshWorkflow.h"
#include "MeshForge.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MeshForgeEditor"

UMeshDefFactory::UMeshDefFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = UMeshDef::StaticClass();
}

UObject* UMeshDefFactory::FactoryCreateNew(
	UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject*, FFeedbackContext*)
{
	UMeshDef* Def = NewObject<UMeshDef>(InParent, Class, Name, Flags);

	if (Def)
	{
		// The finish settings are applied here rather than inside ApplyProjectDefaults, and this is
		// the only honest place for them: nothing distinguishes a field nobody chose from one
		// somebody set to the same value, so the project's default can only be applied before there
		// is anybody to have chosen. A brand new asset is exactly that moment.
		Def->Finish = GetDefault<UMeshForgeSettings>()->DefaultFinish;

		// The same defaults CreateMeshDef applies, through the same call. A definition made by hand
		// that skipped this would look identical in the Content Browser and generate against nothing.
		Def->ApplyProjectDefaults();

		if (const UMeshWorkflow* Workflow = Cast<UMeshWorkflow>(ChosenWorkflow.TryLoad()))
		{
			TArray<FString> Notes;
			FString Error;
			if (!Def->ApplyWorkflow(Workflow, Notes, Error))
			{
				UE_LOG(LogMeshForge, Warning, TEXT("New definition '%s' could not start from '%s': %s"), *Name.ToString(), *ChosenWorkflow.ToString(), *Error);
			}
			for (const FString& Note : Notes)
			{
				UE_LOG(LogMeshForge, Warning, TEXT("New definition '%s': %s"), *Name.ToString(), *Note);
			}
		}
	}

	return Def;
}

bool UMeshDefFactory::ConfigureProperties()
{
	ChosenWorkflow.Reset();

	TArray<FAssetData> Workflows;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get()
		.GetAssetsByClass(UMeshWorkflow::StaticClass()->GetClassPathName(), Workflows, true);

	// Nothing to choose between: a blank definition, as before workflows existed, with no window.
	if (Workflows.Num() == 0)
	{
		return true;
	}

	const FSoftObjectPath Default = UMeshForgeSettings::Get()->DefaultWorkflow;
	Workflows.Sort([&Default](const FAssetData& A, const FAssetData& B)
	{
		const bool bA = A.GetSoftObjectPath() == Default, bB = B.GetSoftObjectPath() == Default;
		return bA != bB ? bA : A.AssetName.LexicalLess(B.AssetName);
	});

	bool bChosen = false;
	FSoftObjectPath Choice;

	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("PickWorkflowTitle", "New Mesh Definition"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	TSharedRef<SVerticalBox> Choices = SNew(SVerticalBox);

	Choices->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
	[
		SNew(STextBlock)
		.Text(LOCTEXT("PickWorkflowHeading", "Start from a workflow"))
		.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
	];

	auto AddChoice = [&](const FText& Name, const FText& Tip, const FSoftObjectPath& Path, bool bPrimary)
	{
		Choices->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
		[
			SNew(SButton)
			.ButtonStyle(bPrimary ? &FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton") : &FAppStyle::Get().GetWidgetStyle<FButtonStyle>("Button"))
			.ContentPadding(FMargin(10.0f, 5.0f))
			.ToolTipText(Tip)
			.OnClicked_Lambda([&bChosen, &Choice, Path, &Window]()
			{
				Choice = Path;
				bChosen = true;
				Window->RequestDestroyWindow();
				return FReply::Handled();
			})
			[
				SNew(STextBlock).Text(Name)
			]
		];
	};

	for (const FAssetData& Asset : Workflows)
	{
		if (const UMeshWorkflow* Workflow = Cast<UMeshWorkflow>(Asset.GetAsset()))
		{
			const bool bIsDefault = Asset.GetSoftObjectPath() == Default;
			AddChoice(bIsDefault
					? FText::Format(LOCTEXT("DefaultWorkflowChoice", "{0}  (project default)"), Workflow->GetShownName())
					: Workflow->GetShownName(),
				Workflow->Description, Asset.GetSoftObjectPath(), bIsDefault);
		}
	}

	AddChoice(LOCTEXT("BlankChoice", "Blank - every stage on, nothing chosen"),
		LOCTEXT("BlankChoiceTip", "A definition set up by hand, as before workflows existed."), FSoftObjectPath(), false);

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(16.0f))
		[
			SNew(SBox).MinDesiredWidth(360.0f)[ Choices ]
		]);

	GEditor->EditorAddModalWindow(Window);

	ChosenWorkflow = Choice;
	return bChosen;
}

FText UMeshDefFactory::GetDisplayName() const
{
	return LOCTEXT("NewMeshDef", "Mesh Definition");
}

FString UMeshDefFactory::GetDefaultNewAssetName() const
{
	// MSD_ rather than MD_, which MotionForge already uses for a Motion Definition. Two asset types
	// sharing a prefix in one project is a small thing that makes a content browser unreadable.
	return TEXT("MSD_NewMesh");
}

UMeshWorkflowFactory::UMeshWorkflowFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = UMeshWorkflow::StaticClass();
}

UObject* UMeshWorkflowFactory::FactoryCreateNew(
	UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject*, FFeedbackContext*)
{
	return NewObject<UMeshWorkflow>(InParent, Class, Name, Flags);
}

FText UMeshWorkflowFactory::GetDisplayName() const
{
	return LOCTEXT("NewMeshWorkflow", "Mesh Workflow");
}

FString UMeshWorkflowFactory::GetDefaultNewAssetName() const
{
	return TEXT("WF_NewWorkflow");
}

#undef LOCTEXT_NAMESPACE
