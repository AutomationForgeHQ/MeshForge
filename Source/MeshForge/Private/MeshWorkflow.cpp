#include "MeshWorkflow.h"

FText UMeshWorkflow::GetShownName() const
{
	return DisplayName.IsEmpty() ? FText::FromString(GetName()) : DisplayName;
}

FString UMeshWorkflow::PluginOfPath(const FSoftObjectPath& Path)
{
	// The first segment of a long package name is its mount point, which for plugin content is the plugin.
	FString Package = Path.GetLongPackageName();
	Package.RemoveFromStart(TEXT("/"));
	FString Mount;
	return Package.Split(TEXT("/"), &Mount, nullptr) ? Mount : Package;
}

TArray<FString> UMeshWorkflow::DescribeMissingPresets() const
{
	TArray<FString> Missing;

	auto Check = [&Missing](const FString& Where, bool bResolved, const FSoftObjectPath& Preset)
	{
		if (!bResolved && !Preset.IsNull())
		{
			Missing.Add(FString::Printf(TEXT("%s uses '%s', which is not installed here - it needs %s."),
				*Where, *Preset.GetAssetName(), *PluginOfPath(Preset)));
		}
	};

	Check(TEXT("Concept"), Resolve(Concept.Pipeline, Concept.Preset) != nullptr, Concept.Preset.ToSoftObjectPath());
	Check(TEXT("Mesh"), Resolve(Mesh.Pipeline, Mesh.Preset) != nullptr, Mesh.Preset.ToSoftObjectPath());

	for (int32 Index = 0; Index < References.Num(); ++Index)
	{
		const FMeshWorkflowPipelineSlot& Slot = References[Index];
		Check(FString::Printf(TEXT("Reference step %d"), Index + 1), Resolve(Slot.Pipeline, Slot.Preset) != nullptr,
			Slot.Preset.ToSoftObjectPath());
	}

	for (int32 Index = 0; Index < Post.Num(); ++Index)
	{
		const FMeshWorkflowPostSlot& Slot = Post[Index];
		Check(FString::Printf(TEXT("Post step %d"), Index + 1), Resolve(Slot.Pipeline, Slot.Preset) != nullptr,
			Slot.Preset.ToSoftObjectPath());
	}

	return Missing;
}
