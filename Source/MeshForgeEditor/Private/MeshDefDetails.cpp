#include "MeshDefDetails.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "Algo/AnyOf.h"

namespace MeshDefDetailsPrivate
{
	/**
	 * The pipeline, in order, plus the two categories that are outputs rather than stages.
	 *
	 * The names carry their stage number because the panel beside this one numbers them too, and a
	 * person moving between the two should not have to work out that "Import" is stage five.
	 */
	static const TCHAR* const Order[] = {
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
}
