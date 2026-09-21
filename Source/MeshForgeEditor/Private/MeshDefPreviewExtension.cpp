#include "MeshDefPreviewExtension.h"

namespace MeshDefPreviewExtensionsPrivate
{
	/** Function-local, so it is built on first use rather than in whatever order statics happen to run. */
	static TArray<FMeshDefPreviewExtensionType>& Registry()
	{
		static TArray<FMeshDefPreviewExtensionType> Types;
		return Types;
	}
}

void FMeshDefPreviewExtensions::Register(const FMeshDefPreviewExtensionType& Type)
{
	if (Type.Id.IsNone() || !Type.Make)
	{
		return;
	}

	TArray<FMeshDefPreviewExtensionType>& Types = MeshDefPreviewExtensionsPrivate::Registry();

	// Replacing rather than adding, so a hot reload that runs StartupModule twice leaves one.
	Types.RemoveAll([&Type](const FMeshDefPreviewExtensionType& Existing)
	{
		return Existing.Id == Type.Id;
	});

	Types.Add(Type);

	Types.StableSort([](const FMeshDefPreviewExtensionType& A, const FMeshDefPreviewExtensionType& B)
	{
		return A.Order < B.Order;
	});
}

void FMeshDefPreviewExtensions::Unregister(FName Id)
{
	MeshDefPreviewExtensionsPrivate::Registry().RemoveAll([Id](const FMeshDefPreviewExtensionType& Existing)
	{
		return Existing.Id == Id;
	});
}

const TArray<FMeshDefPreviewExtensionType>& FMeshDefPreviewExtensions::Get()
{
	return MeshDefPreviewExtensionsPrivate::Registry();
}
