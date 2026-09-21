// A post step you finish by hand, in Blender, and send back in one click.

#pragma once

#include "CoreMinimal.h"
#include "MeshPostPipeline.h"
#include "MeshBlenderEditPipeline.generated.h"

/**
 * Open the mesh in Blender, let somebody fix it, and bring the result back as this step's output.
 *
 * **Two clicks out and one back.** Running the step writes the mesh, and any reference meshes, into a
 * session folder and opens Blender on them with a small add-on already loaded: the mesh selected and in
 * sculpt mode, the references locked beside it, and a *Send back to Unreal* button in the sidebar's
 * MeshForge tab. The chain waits; pressing the button writes the result and a manifest, and MeshForge
 * imports it where the input stood and carries on with the next step.
 *
 * **A file, not a socket.** The manifest is written last, by rename, so its appearance means the
 * result is complete. No port, no firewall prompt, nothing running in Blender that Unreal has to find,
 * and a result sent after the editor was closed is still there to pick up from the step.
 *
 * **A skinned mesh comes back on its own vertices.** With *Edit skinned mesh* on, the step works on the
 * skeletal asset rather than on geometry: what returns is a copy of that asset with its vertices moved to
 * where Blender left them, so its skeleton, weights, morph targets and materials are untouched. The
 * topology has to come back unchanged, and the step says so when it does not.
 *
 * **What Blender is told to leave alone.** The importer's three silent traps are handled before the
 * mesh is shown: seam vertices are merged so a brush does not tear the mesh along its UV seams, the
 * rotation mode is set so a turn is a turn, and a file without normals is shaded smooth. Nothing is
 * rescaled - glTF is metres both ways, and Unreal's importer converts back to centimetres itself.
 */
UCLASS(BlueprintType, EditInlineNew, meta = (DisplayName = "Edit in Blender - round trip"))
class MESHFORGE_API UMeshBlenderEditPipeline : public UMeshPostPipeline
{
	GENERATED_BODY()

public:

	/**
	 * Shown in Blender beside the mesh, locked and unselectable, for context.
	 *
	 * The character a garment is worn by, the wall a prop leans on. Exported in the same space as the
	 * mesh, so they stand where they stand in Unreal. Static or skeletal; a skeletal one arrives in its
	 * reference pose.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blender",
		meta = (AllowedClasses = "/Script/Engine.StaticMesh,/Script/Engine.SkeletalMesh"))
	TArray<TSoftObjectPtr<UObject>> ReferenceMeshes;

	/**
	 * Edit the skinned mesh rather than a static one.
	 *
	 * On, this step takes a skeletal mesh - the garment a skinning step made - and opens it in Blender with its
	 * skeleton, weights and morph targets, so it can be sculpted where it will deform. What comes back is not
	 * imported: the same asset is duplicated and its own vertices are moved to where Blender left them, so the
	 * skeleton it is bound to, its weights, its morph targets and its materials are the ones it went out with.
	 *
	 * **The topology therefore has to come back unchanged**, and the step says so plainly when it does not.
	 * Adding or removing geometry on a skinned mesh means new vertices with no weights; do that on the static
	 * mesh, before skinning, and skin the result.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blender", meta = (SignatureOmitsDefault))
	bool bEditSkinnedMesh = false;

	/** Open straight into sculpt mode. Off leaves Blender in object mode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blender")
	bool bOpenInSculptMode = true;

	/**
	 * Put the input's own materials back on the result, slot for slot.
	 *
	 * On by default. A round trip through Blender's material converter replaces Unreal materials with
	 * approximations of them, so the result keeps Blender's slots and gets the input's materials back
	 * by slot. Skipped, with a warning, when the slot count changed. Turn it off when the edit in
	 * Blender is the materials.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blender")
	bool bRestoreInputMaterials = true;

	virtual FName GetProviderId() const override { return TEXT("Blender"); }
	virtual FString DescribeCost() const override { return FString(); }
	virtual FString Validate() const override;
	virtual void Run(const FMeshPostJob& Job, FMeshPostResult& OutResult) const override;

	/** A skinned edit works on the asset itself: assets in, an asset out, and nothing imported. */
	virtual bool IsNativeStep() const override { return bEditSkinnedMesh; }
	virtual bool AcceptsNativeInput(const UClass* InputClass) const override;
	virtual bool WantsInteractiveInputGlb() const override { return !bEditSkinnedMesh; }
	virtual UObject* CreateNativeOutput(UObject* Input, FMeshPostNativeContext& Context, FString& Error) const override;
	virtual FString GetNativeOutputPrefix() const override { return TEXT("SK_"); }

	virtual bool IsInteractiveStep() const override { return true; }
	virtual bool BeginInteractive(const FMeshPostInteractiveSession& Session, FString& OutError) const override;
	virtual EMeshInteractiveState PollInteractive(const FMeshPostInteractiveSession& Session,
		FString& OutResultGlb, FString& OutSummary, FString& OutError) const override;
	virtual void ConsumeInteractiveResult(const FMeshPostInteractiveSession& Session) const override;
	virtual void OnInteractiveOutputImported(UStaticMesh* Output, UObject* Input) const override;

	/** The add-on script Blender is started with, inside this plugin. Empty when it is missing. */
	static FString ResolveScript();
};
