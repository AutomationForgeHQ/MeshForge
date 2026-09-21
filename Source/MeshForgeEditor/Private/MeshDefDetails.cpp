#include "MeshDefDetails.h"

#include "MeshDef.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "IPropertyUtilities.h"
#include "Algo/AnyOf.h"
#include "Containers/Ticker.h"

namespace MeshDefDetailsPrivate
{
	/** The category each switchable stage's settings live in. */
	static const TPair<EMeshStage, const TCHAR*> StageCategories[] = {
		{ EMeshStage::Concept,    TEXT("1 Concept") },
		{ EMeshStage::References, TEXT("2 References") },
		{ EMeshStage::Mesh,       TEXT("3 Mesh") },
		{ EMeshStage::Post,       TEXT("4 Post") },
		{ EMeshStage::Import,     TEXT("5 Import") },
	};

	/**
	 * The pipeline, in order, plus the two categories that are outputs rather than stages.
	 *
	 * The names carry their stage number because the panel beside this one numbers them too, and a
	 * person moving between the two should not have to work out that "Import" is stage five.
	 */
	static const TCHAR* const Order[] = {
		TEXT("0 Workflow"),
		TEXT("Prompt"),
		TEXT("1 Concept"),
		TEXT("2 References"),
		TEXT("3 Mesh"),
		TEXT("4 Post"),
		TEXT("5 Import"),
		TEXT("6 State"),
		TEXT("7 Result"),
	};
}

TSharedRef<IDetailCustomization> FMeshDefDetails::MakeInstance()
{
	return MakeShared<FMeshDefDetails>();
}

void FMeshDefDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	DetailBuilder.SortCategories([](const TMap<FName, IDetailCategoryBuilder*>& Categories)
	{
		// Spaced out, so a category this list has never heard of can be slotted between two known
		// ones later without renumbering anything.
		int32 Sort = 0;

		for (const TCHAR* Name : MeshDefDetailsPrivate::Order)
		{
			if (IDetailCategoryBuilder* const* Category = Categories.Find(FName(Name)))
			{
				(*Category)->SetSortOrder(Sort);
			}

			Sort += 10;
		}

		// Everything unnamed goes after, in whatever order it already had. A category added later
		// and forgotten here should be visibly at the end rather than invisibly in the middle.
		for (const TPair<FName, IDetailCategoryBuilder*>& Pair : Categories)
		{
			const bool bNamed = Algo::AnyOf(MeshDefDetailsPrivate::Order,
				[&Pair](const TCHAR* Name) { return Pair.Key == FName(Name); });

			if (!bNamed && Pair.Value != nullptr)
			{
				Pair.Value->SetSortOrder(Sort++);
			}
		}
	});

	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	UMeshDef* Def = Objects.Num() == 1 ? Cast<UMeshDef>(Objects[0].Get()) : nullptr;

	if (Def == nullptr)
	{
		return;
	}

	Definition = Def;
	Utilities = DetailBuilder.GetPropertyUtilities();

	if (!PropertyChangedHandle.IsValid())
	{
		PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(this, &FMeshDefDetails::OnAnyPropertyChanged);
	}

	// With Mesh off the Source Mesh is the definition's whole input, so it moves up beside the switches
	// rather than disappearing with the rest of the Mesh category.
	if (!Def->StageSwitches.bMesh)
	{
		DetailBuilder.EditCategory(TEXT("0 Workflow")).AddProperty(
			DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UMeshDef, SourceMesh)));
	}

	for (const TPair<EMeshStage, const TCHAR*>& Stage : MeshDefDetailsPrivate::StageCategories)
	{
		if (!Def->StageSwitches.IsOn(Stage.Key))
		{
			DetailBuilder.HideCategory(Stage.Value);
		}
	}
}

FMeshDefDetails::~FMeshDefDetails()
{
	if (PropertyChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
	}
}

void FMeshDefDetails::OnAnyPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	// Only this definition's switches. The delegate is global; anything else would rebuild the layout for
	// every edit anywhere in the editor.
	const FName Changed = Event.GetMemberPropertyName();
	const bool bSwitches = Changed.IsNone() || Changed == GET_MEMBER_NAME_CHECKED(UMeshDef, StageSwitches);

	if (Object == nullptr || Object != Definition.Get() || !bSwitches)
	{
		return;
	}

	// Deferred a tick: rebuilding the layout inside the broadcast destroys the widget that sent it.
	TWeakPtr<IPropertyUtilities> Weak = Utilities;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float) -> bool
	{
		if (const TSharedPtr<IPropertyUtilities> Live = Weak.Pin())
		{
			Live->ForceRefresh();
		}
		return false;
	}), 0.0f);
}
