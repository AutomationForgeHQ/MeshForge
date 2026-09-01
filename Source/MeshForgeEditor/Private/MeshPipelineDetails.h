// Shows a mesh pipeline which pictures it will actually be given, and lets them be dropped in.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class UMeshDef;
class UMeshForgePipeline;

/**
 * Adds the reference-picture slots to a mesh pipeline's settings.
 *
 * **Where the pictures are chosen is now both places, and that is the point.** The gallery is still
 * the list - a picture has to be an asset on the definition, because everything downstream hashes,
 * records and re-imports it - but the slot is where somebody is looking when they wonder what the
 * model will be sent, and dragging a picture onto it is the shortest way to answer that. The slots
 * write through the same functions the gallery's menu uses, so there is one set of rules about
 * which picture can hold which job.
 *
 * The number of slots follows the pipeline: one where it reads a single picture, four where
 * multi-view is on. An empty slot says so rather than being blank, because a missing main image is
 * the difference between a generation and a refusal.
 */
class FMeshPipelineDetails : public IDetailCustomization
{
public:

	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual ~FMeshPipelineDetails() override;

	virtual void CustomizeDetails(IDetailLayoutBuilder& Builder) override;

private:

	/**
	 * How the strip stays honest when something it reads changes.
	 *
	 * The slot count follows the multi-view switch and the pictures follow the definition's
	 * selection, and neither of those rebuilds a details layout on its own - so turning multi-view
	 * off left four thumbnails on screen until the asset was closed and reopened. A layout showing
	 * three pictures that will not be sent is worse than no layout.
	 *
	 * The global property-changed delegate is used rather than a hook on one named property, because
	 * this customization is registered on the pipeline *base* class and has no business knowing what
	 * a subclass called its switch.
	 */
	void OnAnyPropertyChanged(UObject* Object, struct FPropertyChangedEvent& Event);

	FDelegateHandle PropertyChangedHandle;

	TWeakObjectPtr<const UMeshForgePipeline> Pipeline;
	TWeakObjectPtr<const UMeshDef> Definition;
	TSharedPtr<class IPropertyUtilities> Utilities;
};
