// Putting the definition's categories in the order the pipeline actually runs.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

/**
 * Orders a mesh definition's details categories to match the stages beside them.
 *
 * **This exists because declaration order does not decide category order, despite looking as
 * though it does.** Unreal assigns each category a sort order of its own, and the result for this
 * asset was `1 Concept, Prompt, 3 Mesh, 5 Import, 2 References, 4 Post` - stages 1, 3 and 5 above
 * stages 2 and 4, with the brief in the middle. Reordering the header did not fix it and could
 * not: nothing in the header is what is being read.
 *
 * So the order is stated once, here, as a list. A category not in the list sorts after everything
 * named - which is the right default for one added later, since appearing at the bottom is a
 * smaller surprise than appearing between the concept image and the references.
 *
 * Nothing else is customised. The properties draw themselves from their own metadata, and a
 * customization that started rewriting rows would be a second description of this asset to keep in
 * step with the first.
 */
class FMeshDefDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
