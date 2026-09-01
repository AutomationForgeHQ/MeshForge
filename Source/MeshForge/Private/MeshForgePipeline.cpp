// The pipeline base. Everything here is reflection over the subclass's own properties.

#include "MeshForgePipeline.h"

#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"

namespace MeshForgePipelinePrivate
{
	/** Only a subclass's own settings, never the base's bookkeeping. */
	static bool IsSettingProperty(const FProperty* Property)
	{
		if (Property == nullptr || Property->GetOwnerClass() == UMeshForgePipeline::StaticClass())
		{
			return false;
		}

		return Property->HasAnyPropertyFlags(CPF_Edit)
			&& !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated);
	}

	static EMeshOptionType TypeOf(const FProperty* Property, bool& bOutSupported)
	{
		bOutSupported = true;

		if (Property->IsA<FBoolProperty>())                            { return EMeshOptionType::Bool; }
		if (Property->IsA<FEnumProperty>() || Property->IsA<FByteProperty>()) { return EMeshOptionType::Enum; }
		if (Property->IsA<FIntProperty>())                             { return EMeshOptionType::Int; }
		if (Property->IsA<FFloatProperty>() || Property->IsA<FDoubleProperty>()) { return EMeshOptionType::Float; }
		if (Property->IsA<FStrProperty>() || Property->IsA<FNameProperty>())     { return EMeshOptionType::Text; }

		// Structs, arrays and object references have no flat representation, so they are described
		// to a human by the details panel and withheld from the option list rather than flattened
		// into a string an agent would have to guess the shape of.
		bOutSupported = false;
		return EMeshOptionType::Text;
	}

	static void ReadRange(const FProperty* Property, FMeshProviderOption& Option)
	{
		const FString Min = Property->GetMetaData(TEXT("ClampMin"));
		const FString Max = Property->GetMetaData(TEXT("ClampMax"));

		if (Option.Type == EMeshOptionType::Int)
		{
			if (!Min.IsEmpty()) { Option.MinValue = FCString::Atoi(*Min); }
			if (!Max.IsEmpty()) { Option.MaxValue = FCString::Atoi(*Max); }
		}
		else if (Option.Type == EMeshOptionType::Float)
		{
			if (!Min.IsEmpty()) { Option.MinFloat = FCString::Atof(*Min); }
			if (!Max.IsEmpty()) { Option.MaxFloat = FCString::Atof(*Max); }
		}
	}

	static void ReadEnumValues(const FProperty* Property, FMeshProviderOption& Option)
	{
		const UEnum* Enum = nullptr;

		if (const FEnumProperty* AsEnum = CastField<FEnumProperty>(Property))
		{
			Enum = AsEnum->GetEnum();
		}
		else if (const FByteProperty* AsByte = CastField<FByteProperty>(Property))
		{
			Enum = AsByte->Enum;
		}

		if (Enum == nullptr)
		{
			return;
		}

		// NumEnums() counts the generated _MAX entry, which is not a value anybody may choose.
		for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
		{
			if (!Enum->HasMetaData(TEXT("Hidden"), Index))
			{
				Option.AllowedValues.Add(Enum->GetNameStringByIndex(Index));
			}
		}
	}
}

FString UMeshForgePipeline::DescribeCost() const
{
	return FString();
}

FString UMeshForgePipeline::Validate() const
{
	return FString();
}

FString UMeshForgePipeline::Signature() const
{
	// Class name first, because two pipelines with identical settings are still different pipelines
	// when they name different providers.
	FString Signature = GetClass()->GetName() + TEXT(":");

	// Reflected rather than hand-written, so a subclass cannot gain a setting and forget to include
	// it here - which would leave every definition using it claiming an output it no longer makes.
	for (TFieldIterator<FProperty> It(GetClass()); It; ++It)
	{
		const FProperty* Property = *It;
		if (!MeshForgePipelinePrivate::IsSettingProperty(Property))
		{
			continue;
		}

		FString Value;
		Property->ExportTextItem_Direct(
			Value, Property->ContainerPtrToValuePtr<void>(this), nullptr, nullptr, PPF_None);

		Signature += FString::Printf(TEXT("%s=%s;"), *Property->GetName(), *Value);
	}

	return Signature;
}

TArray<FMeshProviderOption> UMeshForgePipeline::DescribeOptions() const
{
	TArray<FMeshProviderOption> Options;

	for (TFieldIterator<FProperty> It(GetClass()); It; ++It)
	{
		const FProperty* Property = *It;
		if (!MeshForgePipelinePrivate::IsSettingProperty(Property))
		{
			continue;
		}

		bool bSupported = false;
		const EMeshOptionType Type = MeshForgePipelinePrivate::TypeOf(Property, bSupported);
		if (!bSupported)
		{
			continue;
		}

		FMeshProviderOption& Option = Options.AddDefaulted_GetRef();
		Option.Key         = Property->GetFName();
		Option.DisplayName = Property->GetDisplayNameText();
		Option.Tooltip     = Property->GetToolTipText();
		Option.Type        = Type;

		MeshForgePipelinePrivate::ReadRange(Property, Option);

		if (Type == EMeshOptionType::Enum)
		{
			MeshForgePipelinePrivate::ReadEnumValues(Property, Option);
		}

		// The default is this instance's current value rather than the class default, because an
		// agent asking what a *configured* pipeline will do wants what it is set to, not what it
		// shipped as.
		Property->ExportTextItem_Direct(
			Option.DefaultValue, Property->ContainerPtrToValuePtr<void>(this), nullptr, nullptr, PPF_None);
	}

	return Options;
}
