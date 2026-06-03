#include "UnrealBridgeMaterialLibrary.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionConstantBiasScale.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSphereMask.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionTransformPosition.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "MaterialExpressionIO.h"
#include "MaterialShared.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace BridgeMaterialGraphJson
{
	struct FGraphOptions
	{
		FString Mode = TEXT("summary");
		FString NodeGuid;
		FString PropertyName;
		int32 MaxDepth = 0;
		FString OutputPath;
		bool bIncludePins = true;
		bool bIncludeProperties = true;
		bool bIncludeCaptions = true;
		bool bIncludeAdjacency = false;
		bool bIncludeCustomCode = false;
		bool bStableOrder = true;
		int32 MaxNodes = 0;
		int32 MaxBytes = 0;
	};

	struct FPinInfo
	{
		FString PinId;
		FString Name;
		FString Type;
		FString DefaultValue;
		int32 Index = 0;
		TArray<FString> ConnectionIds;
	};

	struct FNodeInfo
	{
		FString NodeId;
		FString ExpressionGuid;
		FString ClassName;
		FString Name;
		FString Caption;
		FString Desc;
		int32 X = 0;
		int32 Y = 0;
		TArray<FPinInfo> Inputs;
		TArray<FPinInfo> Outputs;
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	};

	struct FConnectionInfo
	{
		FString Id;
		FString SourceNodeId;
		FString SourceExpressionGuid;
		FString SourcePin;
		FString SourceOutput;
		int32 SourceOutputIndex = 0;
		FString TargetNodeId;
		FString TargetExpressionGuid;
		FString TargetPin;
		FString TargetInput;
		int32 TargetInputIndex = 0;
	};

	struct FPropertyConnectionInfo
	{
		FString Id;
		FString Property;
		FString SourceNodeId;
		FString SourceExpressionGuid;
		FString SourcePin;
		FString SourceOutput;
		int32 SourceOutputIndex = 0;
	};

	struct FResolvedGraphAsset
	{
		bool bSuccess = false;
		bool bIsMaterialInstance = false;
		FString Error;
		FString RequestedPath;
		FString AssetType;
		FString Path;
		FString ResolvedGraphPath;
		FString BaseMaterialPath;
		UMaterial* Material = nullptr;
		UMaterialFunctionInterface* Function = nullptr;
		UMaterialInterface* MaterialInterface = nullptr;
	};

	struct FGraphSnapshot
	{
		FResolvedGraphAsset Asset;
		TArray<FNodeInfo> Nodes;
		TArray<FConnectionInfo> Connections;
		TArray<FPropertyConnectionInfo> PropertyConnections;
		TMap<FString, int32> NodeIndexById;
		TMap<UMaterialExpression*, int32> NodeIndexByExpression;
		TMultiMap<FString, int32> NodeIndicesByExpressionGuid;
		TSharedPtr<FJsonObject> MaterialObject = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> FunctionInterfaceObject = MakeShared<FJsonObject>();
		TArray<FString> Warnings;
	};

	static FString SerializeJsonObject(const TSharedPtr<FJsonObject>& Root)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return Out;
	}

	static TSharedPtr<FJsonObject> ErrorJson(
		const FString& MaterialPath,
		const FString& Mode,
		const FString& Error)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schema_version"), 2);
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("mode"), Mode);
		Root->SetStringField(TEXT("requested_path"), MaterialPath);
		Root->SetStringField(TEXT("error"), Error);
		Root->SetBoolField(TEXT("truncated"), false);
		Root->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>{});
		return Root;
	}

	static FString StripClassPrefix(FString Name)
	{
		Name.RemoveFromStart(TEXT("UMaterialExpression"));
		Name.RemoveFromStart(TEXT("MaterialExpression"));
		return Name;
	}

	static FString GuidToString(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::DigitsWithHyphens);
	}

	static FString NormalizeGuidString(const FString& In)
	{
		FGuid Parsed;
		if (FGuid::Parse(In, Parsed))
		{
			return GuidToString(Parsed);
		}
		return In;
	}

	static FString SafePinName(const FString& Name, int32 Index, const TCHAR* Prefix)
	{
		if (!Name.IsEmpty())
		{
			return Name;
		}
		return FString::Printf(TEXT("%s_%d"), Prefix, Index);
	}

	static FString OutputPinName(UMaterialExpression* Expr, int32 OutputIndex)
	{
		if (!Expr)
		{
			return FString();
		}
		TArray<FExpressionOutput>& Outputs = Expr->GetOutputs();
		if (Outputs.IsValidIndex(OutputIndex))
		{
			return SafePinName(Outputs[OutputIndex].OutputName.ToString(), OutputIndex, TEXT("Output"));
		}
		return FString::Printf(TEXT("Output_%d"), OutputIndex);
	}

	static FString InputPinName(UMaterialExpression* Expr, int32 InputIndex)
	{
		if (!Expr)
		{
			return FString();
		}
		return SafePinName(Expr->GetInputName(InputIndex).ToString(), InputIndex, TEXT("Input"));
	}

	static FString EnumValueName(UEnum* Enum, int64 Value, const FString& PrefixToStrip = FString())
	{
		if (!Enum)
		{
			return FString::Printf(TEXT("%lld"), Value);
		}
		FString Name = Enum->GetNameStringByValue(Value);
		if (!PrefixToStrip.IsEmpty())
		{
			Name.RemoveFromStart(PrefixToStrip);
		}
		return Name;
	}

	template <typename EnumType>
	static FString EnumValueName(EnumType Value, const FString& PrefixToStrip = FString())
	{
		return EnumValueName(StaticEnum<EnumType>(), (int64)Value, PrefixToStrip);
	}

	static TSharedPtr<FJsonValue> JsonString(const FString& Value)
	{
		return MakeShared<FJsonValueString>(Value);
	}

	static TSharedPtr<FJsonValue> JsonNumber(double Value)
	{
		return MakeShared<FJsonValueNumber>(Value);
	}

	static TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Out.Add(JsonString(Value));
		}
		return Out;
	}

	static TArray<TSharedPtr<FJsonValue>> LinearColorArray(const FLinearColor& Color)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(JsonNumber(Color.R));
		Out.Add(JsonNumber(Color.G));
		Out.Add(JsonNumber(Color.B));
		Out.Add(JsonNumber(Color.A));
		return Out;
	}

	static TArray<TSharedPtr<FJsonValue>> Vector4Array(const FVector4f& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(JsonNumber(Value.X));
		Out.Add(JsonNumber(Value.Y));
		Out.Add(JsonNumber(Value.Z));
		Out.Add(JsonNumber(Value.W));
		return Out;
	}

	static void ParseOptions(const FString& OptionsJson, FGraphOptions& Out)
	{
		if (OptionsJson.TrimStartAndEnd().IsEmpty())
		{
			return;
		}

		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(OptionsJson);
		if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
		{
			return;
		}

		FString StringValue;
		if (Obj->TryGetStringField(TEXT("mode"), StringValue) && !StringValue.IsEmpty())
		{
			Out.Mode = StringValue.ToLower();
		}
		if (Obj->TryGetStringField(TEXT("node_guid"), StringValue))
		{
			Out.NodeGuid = NormalizeGuidString(StringValue);
		}
		if (Obj->TryGetStringField(TEXT("property_name"), StringValue))
		{
			Out.PropertyName = StringValue;
		}
		if (Obj->TryGetStringField(TEXT("output_path"), StringValue))
		{
			Out.OutputPath = StringValue;
		}

		double NumberValue = 0.0;
		if (Obj->TryGetNumberField(TEXT("max_depth"), NumberValue))
		{
			Out.MaxDepth = FMath::Max(0, (int32)NumberValue);
		}
		if (Obj->TryGetNumberField(TEXT("max_nodes"), NumberValue))
		{
			Out.MaxNodes = FMath::Max(0, (int32)NumberValue);
		}
		if (Obj->TryGetNumberField(TEXT("max_bytes"), NumberValue))
		{
			Out.MaxBytes = FMath::Max(0, (int32)NumberValue);
		}

		bool BoolValue = false;
		if (Obj->TryGetBoolField(TEXT("include_pins"), BoolValue)) Out.bIncludePins = BoolValue;
		if (Obj->TryGetBoolField(TEXT("include_properties"), BoolValue)) Out.bIncludeProperties = BoolValue;
		if (Obj->TryGetBoolField(TEXT("include_captions"), BoolValue)) Out.bIncludeCaptions = BoolValue;
		if (Obj->TryGetBoolField(TEXT("include_adjacency"), BoolValue)) Out.bIncludeAdjacency = BoolValue;
		if (Obj->TryGetBoolField(TEXT("include_custom_code"), BoolValue)) Out.bIncludeCustomCode = BoolValue;
		if (Obj->TryGetBoolField(TEXT("stable_order"), BoolValue)) Out.bStableOrder = BoolValue;
	}

	static bool PropertyMatches(const FString& Candidate, const FString& Requested)
	{
		if (Requested.IsEmpty())
		{
			return false;
		}
		FString A = Candidate;
		FString B = Requested;
		A.RemoveFromStart(TEXT("MP_"), ESearchCase::IgnoreCase);
		B.RemoveFromStart(TEXT("MP_"), ESearchCase::IgnoreCase);
		return A.Equals(B, ESearchCase::IgnoreCase);
	}

	static FString MaterialPropertyName(EMaterialProperty Property)
	{
		return EnumValueName<EMaterialProperty>(Property, TEXT("MP_"));
	}

	static FString AssetClassName(UObject* Asset)
	{
		if (!Asset || !Asset->GetClass())
		{
			return TEXT("Unknown");
		}

		FString Name = Asset->GetClass()->GetName();
		Name.RemoveFromStart(TEXT("U"));
		return Name;
	}

	static FResolvedGraphAsset ResolveMaterialGraphAsset(const FString& MaterialPath)
	{
		FResolvedGraphAsset Resolved;
		Resolved.RequestedPath = MaterialPath;

		UObject* Loaded = LoadObject<UObject>(nullptr, *MaterialPath);
		if (!Loaded)
		{
			Resolved.Error = FString::Printf(TEXT("could not load '%s'"), *MaterialPath);
			return Resolved;
		}

		Resolved.Path = Loaded->GetPathName();

		if (UMaterial* Material = Cast<UMaterial>(Loaded))
		{
			Resolved.bSuccess = true;
			Resolved.AssetType = TEXT("Material");
			Resolved.Material = Material;
			Resolved.MaterialInterface = Material;
			Resolved.ResolvedGraphPath = Material->GetPathName();
			Resolved.BaseMaterialPath = Material->GetPathName();
			return Resolved;
		}

		if (UMaterialFunctionInterface* Function = Cast<UMaterialFunctionInterface>(Loaded))
		{
			UMaterialFunction* BaseFunction = Function->GetBaseFunction();
			Resolved.bSuccess = true;
			Resolved.AssetType = AssetClassName(Loaded);
			Resolved.Function = Function;
			Resolved.ResolvedGraphPath = BaseFunction ? BaseFunction->GetPathName() : Function->GetPathName();
			return Resolved;
		}

		if (UMaterialInstance* Instance = Cast<UMaterialInstance>(Loaded))
		{
			UMaterial* BaseMaterial = Instance->GetMaterial();
			if (!BaseMaterial)
			{
				Resolved.Error = FString::Printf(TEXT("material instance '%s' has no base material"), *MaterialPath);
				return Resolved;
			}

			Resolved.bSuccess = true;
			Resolved.bIsMaterialInstance = true;
			Resolved.AssetType = TEXT("MaterialInstance");
			Resolved.Material = BaseMaterial;
			Resolved.MaterialInterface = Instance;
			Resolved.ResolvedGraphPath = BaseMaterial->GetPathName();
			Resolved.BaseMaterialPath = BaseMaterial->GetPathName();
			return Resolved;
		}

		if (UMaterialInterface* MaterialInterface = Cast<UMaterialInterface>(Loaded))
		{
			UMaterial* BaseMaterial = MaterialInterface->GetMaterial();
			if (!BaseMaterial)
			{
				Resolved.Error = FString::Printf(TEXT("material interface '%s' has no base material"), *MaterialPath);
				return Resolved;
			}

			Resolved.bSuccess = true;
			Resolved.bIsMaterialInstance = true;
			Resolved.AssetType = TEXT("MaterialInterface");
			Resolved.Material = BaseMaterial;
			Resolved.MaterialInterface = MaterialInterface;
			Resolved.ResolvedGraphPath = BaseMaterial->GetPathName();
			Resolved.BaseMaterialPath = BaseMaterial->GetPathName();
			return Resolved;
		}

		Resolved.Error = FString::Printf(
			TEXT("'%s' is %s, not UMaterial, UMaterialFunctionInterface, or UMaterialInterface"),
			*MaterialPath,
			*Loaded->GetClass()->GetName());
		return Resolved;
	}

	static void AddParameterProperties(UMaterialExpressionParameter* Parameter, TSharedPtr<FJsonObject> Props)
	{
		if (!Parameter || !Props.IsValid())
		{
			return;
		}
		Props->SetStringField(TEXT("parameter_name"), Parameter->ParameterName.ToString());
		if (!Parameter->Group.IsNone())
		{
			Props->SetStringField(TEXT("group"), Parameter->Group.ToString());
		}
		Props->SetNumberField(TEXT("sort_priority"), Parameter->SortPriority);
	}

	static void FillExpressionProperties(
		UMaterialExpression* Expr,
		TSharedPtr<FJsonObject> Props,
		const FGraphOptions& Options)
	{
		if (!Expr || !Props.IsValid())
		{
			return;
		}

		if (UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expr))
		{
			AddParameterProperties(Parameter, Props);
		}

		if (UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expr))
		{
			Props->SetNumberField(TEXT("default_value"), Scalar->DefaultValue);
			Props->SetNumberField(TEXT("slider_min"), Scalar->SliderMin);
			Props->SetNumberField(TEXT("slider_max"), Scalar->SliderMax);
			Props->SetBoolField(TEXT("use_custom_primitive_data"), Scalar->bUseCustomPrimitiveData);
			Props->SetNumberField(TEXT("primitive_data_index"), Scalar->PrimitiveDataIndex);
		}
		else if (UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Expr))
		{
			Props->SetArrayField(TEXT("default_value"), LinearColorArray(Vector->DefaultValue));
			Props->SetBoolField(TEXT("use_custom_primitive_data"), Vector->bUseCustomPrimitiveData);
			Props->SetNumberField(TEXT("primitive_data_index"), Vector->PrimitiveDataIndex);
		}
		else if (UMaterialExpressionStaticBoolParameter* StaticBool = Cast<UMaterialExpressionStaticBoolParameter>(Expr))
		{
			Props->SetBoolField(TEXT("default_value"), StaticBool->DefaultValue != 0);
			Props->SetBoolField(TEXT("dynamic_branch"), StaticBool->DynamicBranch != 0);
		}
		else if (UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Expr))
		{
			Props->SetNumberField(TEXT("value"), Constant->R);
		}
		else if (UMaterialExpressionConstant2Vector* Constant2 = Cast<UMaterialExpressionConstant2Vector>(Expr))
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Add(JsonNumber(Constant2->R));
			Values.Add(JsonNumber(Constant2->G));
			Props->SetArrayField(TEXT("value"), Values);
		}
		else if (UMaterialExpressionConstant3Vector* Constant3 = Cast<UMaterialExpressionConstant3Vector>(Expr))
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Add(JsonNumber(Constant3->Constant.R));
			Values.Add(JsonNumber(Constant3->Constant.G));
			Values.Add(JsonNumber(Constant3->Constant.B));
			Props->SetArrayField(TEXT("value"), Values);
		}
		else if (UMaterialExpressionConstant4Vector* Constant4 = Cast<UMaterialExpressionConstant4Vector>(Expr))
		{
			Props->SetArrayField(TEXT("value"), LinearColorArray(Constant4->Constant));
		}
		else if (UMaterialExpressionTextureSampleParameter* TextureParam = Cast<UMaterialExpressionTextureSampleParameter>(Expr))
		{
			if (TextureParam->Texture)
			{
				Props->SetStringField(TEXT("texture_path"), TextureParam->Texture->GetPathName());
			}
			Props->SetStringField(TEXT("sampler_type"), EnumValueName<EMaterialSamplerType>(TextureParam->SamplerType, TEXT("SAMPLERTYPE_")));
		}
		else if (UMaterialExpressionTextureBase* TextureBase = Cast<UMaterialExpressionTextureBase>(Expr))
		{
			if (TextureBase->Texture)
			{
				Props->SetStringField(TEXT("texture_path"), TextureBase->Texture->GetPathName());
			}
			Props->SetStringField(TEXT("sampler_type"), EnumValueName<EMaterialSamplerType>(TextureBase->SamplerType, TEXT("SAMPLERTYPE_")));
		}
		else if (UMaterialExpressionTextureCoordinate* TexCoord = Cast<UMaterialExpressionTextureCoordinate>(Expr))
		{
			Props->SetNumberField(TEXT("coordinate_index"), TexCoord->CoordinateIndex);
			Props->SetNumberField(TEXT("u_tiling"), TexCoord->UTiling);
			Props->SetNumberField(TEXT("v_tiling"), TexCoord->VTiling);
			Props->SetBoolField(TEXT("unmirror_u"), TexCoord->UnMirrorU != 0);
			Props->SetBoolField(TEXT("unmirror_v"), TexCoord->UnMirrorV != 0);
		}
		else if (UMaterialExpressionTransformPosition* TransformPosition = Cast<UMaterialExpressionTransformPosition>(Expr))
		{
			const FString Source = EnumValueName<EMaterialPositionTransformSource>(TransformPosition->TransformSourceType, TEXT("TRANSFORMPOSSOURCE_"));
			const FString Target = EnumValueName<EMaterialPositionTransformSource>(TransformPosition->TransformType, TEXT("TRANSFORMPOSSOURCE_"));
			Props->SetStringField(TEXT("source_space"), Source);
			Props->SetStringField(TEXT("target_space"), Target);
			Props->SetStringField(TEXT("transform_source"), Source);
			Props->SetStringField(TEXT("transform_type"), Target);
		}
		else if (UMaterialExpressionTransform* Transform = Cast<UMaterialExpressionTransform>(Expr))
		{
			const FString Source = EnumValueName<EMaterialVectorCoordTransformSource>(Transform->TransformSourceType, TEXT("TRANSFORMSOURCE_"));
			const FString Target = EnumValueName<EMaterialVectorCoordTransform>(Transform->TransformType, TEXT("TRANSFORM_"));
			Props->SetStringField(TEXT("source_space"), Source);
			Props->SetStringField(TEXT("target_space"), Target);
			Props->SetStringField(TEXT("transform_source"), Source);
			Props->SetStringField(TEXT("transform_type"), Target);
		}
		else if (UMaterialExpressionConstantBiasScale* BiasScale = Cast<UMaterialExpressionConstantBiasScale>(Expr))
		{
			Props->SetNumberField(TEXT("bias"), BiasScale->Bias);
			Props->SetNumberField(TEXT("scale"), BiasScale->Scale);
		}
		else if (UMaterialExpressionSphereMask* SphereMask = Cast<UMaterialExpressionSphereMask>(Expr))
		{
			Props->SetNumberField(TEXT("attenuation_radius"), SphereMask->AttenuationRadius);
			Props->SetNumberField(TEXT("hardness_percent"), SphereMask->HardnessPercent);
		}
		else if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expr))
		{
			Props->SetStringField(TEXT("description"), Custom->Description);
			Props->SetNumberField(TEXT("code_length"), Custom->Code.Len());
			Props->SetStringField(TEXT("output_type"), EnumValueName<ECustomMaterialOutputType>(Custom->OutputType, TEXT("CMOT_")));
			if (Options.bIncludeCustomCode)
			{
				Props->SetStringField(TEXT("code"), Custom->Code);
			}

			TArray<TSharedPtr<FJsonValue>> CustomInputs;
			for (int32 InputIdx = 0; InputIdx < Custom->Inputs.Num(); ++InputIdx)
			{
				const FCustomInput& Input = Custom->Inputs[InputIdx];
				TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
				InputObj->SetStringField(TEXT("input_name"), Input.InputName.ToString());
				InputObj->SetStringField(TEXT("pin_name"), Input.InputName.ToString());
				InputObj->SetNumberField(TEXT("input_index"), InputIdx);
				CustomInputs.Add(MakeShared<FJsonValueObject>(InputObj));
			}
			Props->SetArrayField(TEXT("custom_inputs"), CustomInputs);

			TArray<TSharedPtr<FJsonValue>> AdditionalOutputs;
			for (int32 OutputIdx = 0; OutputIdx < Custom->AdditionalOutputs.Num(); ++OutputIdx)
			{
				const FCustomOutput& Output = Custom->AdditionalOutputs[OutputIdx];
				TSharedPtr<FJsonObject> OutputObj = MakeShared<FJsonObject>();
				const int32 PinIndex = OutputIdx + 1;
				OutputObj->SetStringField(TEXT("output_name"), Output.OutputName.ToString());
				OutputObj->SetStringField(TEXT("name"), Output.OutputName.ToString());
				OutputObj->SetStringField(TEXT("output_type"), EnumValueName<ECustomMaterialOutputType>(Output.OutputType, TEXT("CMOT_")));
				OutputObj->SetNumberField(TEXT("output_index"), PinIndex);
				OutputObj->SetStringField(TEXT("source_output"), FString::Printf(TEXT("Output_%d"), PinIndex));
				AdditionalOutputs.Add(MakeShared<FJsonValueObject>(OutputObj));
			}
			Props->SetArrayField(TEXT("additional_outputs"), AdditionalOutputs);
		}
		else if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expr))
		{
			Props->SetStringField(TEXT("input_name"), FunctionInput->InputName.ToString());
			Props->SetStringField(TEXT("input_id"), GuidToString(FunctionInput->Id));
			Props->SetStringField(TEXT("description"), FunctionInput->Description);
			Props->SetStringField(TEXT("input_type"), EnumValueName<EFunctionInputType>(FunctionInput->InputType, TEXT("FunctionInput_")));
			Props->SetArrayField(TEXT("preview_value"), Vector4Array(FunctionInput->PreviewValue));
			Props->SetBoolField(TEXT("use_preview_value_as_default"), FunctionInput->bUsePreviewValueAsDefault != 0);
			Props->SetNumberField(TEXT("sort_priority"), FunctionInput->SortPriority);
		}
		else if (UMaterialExpressionFunctionOutput* FunctionOutput = Cast<UMaterialExpressionFunctionOutput>(Expr))
		{
			Props->SetStringField(TEXT("output_name"), FunctionOutput->OutputName.ToString());
			Props->SetStringField(TEXT("output_id"), GuidToString(FunctionOutput->Id));
			Props->SetStringField(TEXT("description"), FunctionOutput->Description);
			Props->SetNumberField(TEXT("sort_priority"), FunctionOutput->SortPriority);
		}
		else if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
		{
			if (FunctionCall->MaterialFunction)
			{
				const FString FunctionPath = FunctionCall->MaterialFunction->GetPathName();
				Props->SetStringField(TEXT("material_function"), FunctionPath);
				Props->SetStringField(TEXT("function_path"), FunctionPath);
				Props->SetStringField(TEXT("material_function_path"), FunctionPath);
			}

			TArray<TSharedPtr<FJsonValue>> FunctionInputs;
			for (int32 InputIdx = 0; InputIdx < FunctionCall->FunctionInputs.Num(); ++InputIdx)
			{
				const FFunctionExpressionInput& Input = FunctionCall->FunctionInputs[InputIdx];
				TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
				const FString InputName = Input.ExpressionInput
					? Input.ExpressionInput->InputName.ToString()
					: InputPinName(Expr, InputIdx);
				InputObj->SetStringField(TEXT("input_name"), InputName);
				InputObj->SetStringField(TEXT("pin_name"), InputName);
				InputObj->SetNumberField(TEXT("input_index"), InputIdx);
				InputObj->SetStringField(TEXT("input_id"), GuidToString(Input.ExpressionInputId));
				if (Input.ExpressionInput)
				{
					InputObj->SetStringField(TEXT("input_type"), EnumValueName<EFunctionInputType>(Input.ExpressionInput->InputType, TEXT("FunctionInput_")));
				}
				FunctionInputs.Add(MakeShared<FJsonValueObject>(InputObj));
			}
			Props->SetNumberField(TEXT("function_input_count"), FunctionInputs.Num());
			Props->SetArrayField(TEXT("function_inputs"), FunctionInputs);

			TArray<TSharedPtr<FJsonValue>> FunctionOutputs;
			for (int32 OutputIdx = 0; OutputIdx < FunctionCall->FunctionOutputs.Num(); ++OutputIdx)
			{
				const FFunctionExpressionOutput& Output = FunctionCall->FunctionOutputs[OutputIdx];
				TSharedPtr<FJsonObject> OutputObj = MakeShared<FJsonObject>();
				const FString OutputName = Output.ExpressionOutput
					? Output.ExpressionOutput->OutputName.ToString()
					: SafePinName(Output.Output.OutputName.ToString(), OutputIdx, TEXT("Output"));
				OutputObj->SetStringField(TEXT("output_name"), OutputName);
				OutputObj->SetStringField(TEXT("pin_name"), OutputName);
				OutputObj->SetNumberField(TEXT("output_index"), OutputIdx);
				OutputObj->SetStringField(TEXT("output_id"), GuidToString(Output.ExpressionOutputId));
				OutputObj->SetStringField(TEXT("source_output"), FString::Printf(TEXT("Output_%d"), OutputIdx));
				FunctionOutputs.Add(MakeShared<FJsonValueObject>(OutputObj));
			}
			Props->SetNumberField(TEXT("function_output_count"), FunctionOutputs.Num());
			Props->SetArrayField(TEXT("function_outputs"), FunctionOutputs);
		}
		else if (UMaterialExpressionComment* Comment = Cast<UMaterialExpressionComment>(Expr))
		{
			Props->SetStringField(TEXT("text"), Comment->Text);
			Props->SetNumberField(TEXT("size_x"), Comment->SizeX);
			Props->SetNumberField(TEXT("size_y"), Comment->SizeY);
		}
	}

	static FNodeInfo BuildNode(UMaterialExpression* Expr, int32 ExpressionIndex, const FGraphOptions& Options)
	{
		FNodeInfo Node;
		Node.NodeId = FString::Printf(TEXT("n%d"), ExpressionIndex);
		Node.ExpressionGuid = GuidToString(Expr->MaterialExpressionGuid);
		Node.ClassName = StripClassPrefix(Expr->GetClass()->GetName());
		Node.Name = Expr->GetName();
		Node.X = Expr->MaterialExpressionEditorX;
		Node.Y = Expr->MaterialExpressionEditorY;
		Node.Desc = Expr->Desc;

		TArray<FString> Captions;
		Expr->GetCaption(Captions);
		if (Captions.Num() > 0)
		{
			Node.Caption = Captions[0];
		}

		if (Options.bIncludePins)
		{
			for (FExpressionInputIterator It{Expr}; It; ++It)
			{
				FPinInfo Pin;
				Pin.Index = It.Index;
				Pin.Name = InputPinName(Expr, It.Index);
				Pin.PinId = FString::Printf(TEXT("%s:in:%d"), *Node.NodeId, It.Index);
				Node.Inputs.Add(MoveTemp(Pin));
			}

			TArray<FExpressionOutput>& Outputs = Expr->GetOutputs();
			for (int32 OutputIdx = 0; OutputIdx < Outputs.Num(); ++OutputIdx)
			{
				FPinInfo Pin;
				Pin.Index = OutputIdx;
				Pin.Name = SafePinName(Outputs[OutputIdx].OutputName.ToString(), OutputIdx, TEXT("Output"));
				Pin.PinId = FString::Printf(TEXT("%s:out:%d"), *Node.NodeId, OutputIdx);
				Node.Outputs.Add(MoveTemp(Pin));
			}
		}

		if (Options.bIncludeProperties)
		{
			FillExpressionProperties(Expr, Node.Properties, Options);
		}

		return Node;
	}

	static void FillMaterialObject(FGraphSnapshot& Snapshot)
	{
		if (UMaterial* Material = Snapshot.Asset.Material)
		{
			UMaterialInterface* Interface = Snapshot.Asset.MaterialInterface ? Snapshot.Asset.MaterialInterface : Material;
			Snapshot.MaterialObject->SetStringField(TEXT("domain"), EnumValueName<EMaterialDomain>(Material->MaterialDomain, TEXT("MD_")));
			Snapshot.MaterialObject->SetStringField(TEXT("blend_mode"), EnumValueName<EBlendMode>(Interface->GetBlendMode(), TEXT("BLEND_")));
			Snapshot.MaterialObject->SetBoolField(TEXT("two_sided"), Interface->IsTwoSided());
			Snapshot.MaterialObject->SetBoolField(TEXT("use_material_attributes"), Material->bUseMaterialAttributes);

			TArray<TSharedPtr<FJsonValue>> ShadingModels;
			const FMaterialShadingModelField Models = Interface->GetShadingModels();
			for (int32 Index = 0; Index < MSM_NUM; ++Index)
			{
				const EMaterialShadingModel Model = (EMaterialShadingModel)Index;
				if (Models.HasShadingModel(Model))
				{
					ShadingModels.Add(JsonString(EnumValueName<EMaterialShadingModel>(Model, TEXT("MSM_"))));
				}
			}
			Snapshot.MaterialObject->SetArrayField(TEXT("shading_models"), ShadingModels);
		}
		else if (UMaterialFunctionInterface* Function = Snapshot.Asset.Function)
		{
			Snapshot.MaterialObject->SetStringField(TEXT("description"), Function->GetDescription());
			Snapshot.MaterialObject->SetStringField(TEXT("function_class"), Function->GetClass()->GetName());
			Snapshot.MaterialObject->SetStringField(
				TEXT("function_usage"),
				EnumValueName<EMaterialFunctionUsage>(Function->GetMaterialFunctionUsage(), TEXT("")));
			if (UMaterialFunction* BaseFunction = Function->GetBaseFunction())
			{
				Snapshot.MaterialObject->SetStringField(TEXT("user_exposed_caption"), BaseFunction->UserExposedCaption);
				Snapshot.MaterialObject->SetBoolField(TEXT("expose_to_library"), BaseFunction->bExposeToLibrary != 0);
			}
		}
	}

	static void FillFunctionInterface(FGraphSnapshot& Snapshot)
	{
		if (!Snapshot.Asset.Function)
		{
			return;
		}

		TArray<TSharedPtr<FJsonValue>> Inputs;
		TArray<TSharedPtr<FJsonValue>> Outputs;

		for (const FNodeInfo& Node : Snapshot.Nodes)
		{
			if (Node.ClassName == TEXT("FunctionInput"))
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				FString Name;
				Obj->SetStringField(TEXT("node_id"), Node.NodeId);
				Obj->SetStringField(TEXT("expression_guid"), Node.ExpressionGuid);
				Obj->SetStringField(TEXT("guid"), Node.ExpressionGuid);
				Node.Properties->TryGetStringField(TEXT("input_name"), Name);
				Obj->SetStringField(TEXT("name"), Name);
				FString InputType;
				if (Node.Properties->TryGetStringField(TEXT("input_type"), InputType))
				{
					Obj->SetStringField(TEXT("type"), InputType);
				}
				Inputs.Add(MakeShared<FJsonValueObject>(Obj));
			}
			else if (Node.ClassName == TEXT("FunctionOutput"))
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				FString Name;
				Obj->SetStringField(TEXT("node_id"), Node.NodeId);
				Obj->SetStringField(TEXT("expression_guid"), Node.ExpressionGuid);
				Obj->SetStringField(TEXT("guid"), Node.ExpressionGuid);
				Node.Properties->TryGetStringField(TEXT("output_name"), Name);
				Obj->SetStringField(TEXT("name"), Name);
				Outputs.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}

		Snapshot.FunctionInterfaceObject->SetArrayField(TEXT("inputs"), Inputs);
		Snapshot.FunctionInterfaceObject->SetArrayField(TEXT("outputs"), Outputs);
	}

	static void SortSnapshot(FGraphSnapshot& Snapshot)
	{
		Snapshot.Nodes.Sort([](const FNodeInfo& A, const FNodeInfo& B)
		{
			return A.NodeId < B.NodeId;
		});

		Snapshot.Connections.Sort([](const FConnectionInfo& A, const FConnectionInfo& B)
		{
			const FString KeyA = A.SourceNodeId + TEXT("|") + A.SourceOutput + TEXT("|") + A.TargetNodeId + TEXT("|") + A.TargetInput;
			const FString KeyB = B.SourceNodeId + TEXT("|") + B.SourceOutput + TEXT("|") + B.TargetNodeId + TEXT("|") + B.TargetInput;
			return KeyA < KeyB;
		});

		Snapshot.PropertyConnections.Sort([](const FPropertyConnectionInfo& A, const FPropertyConnectionInfo& B)
		{
			const FString KeyA = A.Property + TEXT("|") + A.SourceNodeId + TEXT("|") + A.SourceOutput;
			const FString KeyB = B.Property + TEXT("|") + B.SourceNodeId + TEXT("|") + B.SourceOutput;
			return KeyA < KeyB;
		});
	}

	static void RebuildNodeIndex(FGraphSnapshot& Snapshot)
	{
		Snapshot.NodeIndexById.Reset();
		Snapshot.NodeIndicesByExpressionGuid.Reset();
		for (int32 Index = 0; Index < Snapshot.Nodes.Num(); ++Index)
		{
			Snapshot.NodeIndexById.Add(Snapshot.Nodes[Index].NodeId, Index);
			Snapshot.NodeIndicesByExpressionGuid.Add(Snapshot.Nodes[Index].ExpressionGuid, Index);
		}
	}

	static void AssignConnectionIds(FGraphSnapshot& Snapshot)
	{
		for (FNodeInfo& Node : Snapshot.Nodes)
		{
			for (FPinInfo& Pin : Node.Inputs)
			{
				Pin.ConnectionIds.Reset();
			}
			for (FPinInfo& Pin : Node.Outputs)
			{
				Pin.ConnectionIds.Reset();
			}
		}

		for (int32 Index = 0; Index < Snapshot.Connections.Num(); ++Index)
		{
			FConnectionInfo& Conn = Snapshot.Connections[Index];
			Conn.Id = FString::Printf(TEXT("c%d"), Index);

			if (int32* SrcNodeIndex = Snapshot.NodeIndexById.Find(Conn.SourceNodeId))
			{
				FNodeInfo& SrcNode = Snapshot.Nodes[*SrcNodeIndex];
				if (SrcNode.Outputs.IsValidIndex(Conn.SourceOutputIndex))
				{
					SrcNode.Outputs[Conn.SourceOutputIndex].ConnectionIds.Add(Conn.Id);
				}
			}
			if (int32* DstNodeIndex = Snapshot.NodeIndexById.Find(Conn.TargetNodeId))
			{
				FNodeInfo& DstNode = Snapshot.Nodes[*DstNodeIndex];
				if (DstNode.Inputs.IsValidIndex(Conn.TargetInputIndex))
				{
					DstNode.Inputs[Conn.TargetInputIndex].ConnectionIds.Add(Conn.Id);
				}
			}
		}

		for (int32 Index = 0; Index < Snapshot.PropertyConnections.Num(); ++Index)
		{
			FPropertyConnectionInfo& Conn = Snapshot.PropertyConnections[Index];
			Conn.Id = FString::Printf(TEXT("p%d"), Index);

			if (int32* SrcNodeIndex = Snapshot.NodeIndexById.Find(Conn.SourceNodeId))
			{
				FNodeInfo& SrcNode = Snapshot.Nodes[*SrcNodeIndex];
				if (SrcNode.Outputs.IsValidIndex(Conn.SourceOutputIndex))
				{
					SrcNode.Outputs[Conn.SourceOutputIndex].ConnectionIds.Add(Conn.Id);
				}
			}
		}
	}

	static bool BuildSnapshot(const FString& MaterialPath, const FGraphOptions& Options, FGraphSnapshot& Out)
	{
		Out.Asset = ResolveMaterialGraphAsset(MaterialPath);
		if (!Out.Asset.bSuccess)
		{
			return false;
		}

		FillMaterialObject(Out);

		TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions;
		if (Out.Asset.Material)
		{
			Expressions = Out.Asset.Material->GetExpressions();
		}
		else if (Out.Asset.Function)
		{
			Expressions = Out.Asset.Function->GetExpressions();
		}

		Out.Nodes.Reserve(Expressions.Num());
		for (int32 ExpressionIndex = 0; ExpressionIndex < Expressions.Num(); ++ExpressionIndex)
		{
			const TObjectPtr<UMaterialExpression>& ExprPtr = Expressions[ExpressionIndex];
			UMaterialExpression* Expr = ExprPtr.Get();
			if (!Expr)
			{
				continue;
			}
			const int32 NodeIndex = Out.Nodes.Add(BuildNode(Expr, ExpressionIndex, Options));
			Out.NodeIndexByExpression.Add(Expr, NodeIndex);
		}

		RebuildNodeIndex(Out);

		for (const TObjectPtr<UMaterialExpression>& ExprPtr : Expressions)
		{
			UMaterialExpression* Expr = ExprPtr.Get();
			if (!Expr)
			{
				continue;
			}

			const int32* TargetNodeIndex = Out.NodeIndexByExpression.Find(Expr);
			if (!TargetNodeIndex || !Out.Nodes.IsValidIndex(*TargetNodeIndex))
			{
				continue;
			}
			const FNodeInfo& TargetNode = Out.Nodes[*TargetNodeIndex];

			for (FExpressionInputIterator It{Expr}; It; ++It)
			{
				FExpressionInput* Input = It.Input;
				if (!Input || !Input->Expression)
				{
					continue;
				}

				const int32* SourceNodeIndex = Out.NodeIndexByExpression.Find(Input->Expression);
				if (!SourceNodeIndex || !Out.Nodes.IsValidIndex(*SourceNodeIndex))
				{
					continue;
				}
				const FNodeInfo& SourceNode = Out.Nodes[*SourceNodeIndex];

				FConnectionInfo Conn;
				Conn.SourceNodeId = SourceNode.NodeId;
				Conn.SourceExpressionGuid = SourceNode.ExpressionGuid;
				Conn.SourceOutputIndex = Input->OutputIndex;
				Conn.SourceOutput = OutputPinName(Input->Expression, Input->OutputIndex);
				Conn.SourcePin = FString::Printf(TEXT("%s:out:%d"), *SourceNode.NodeId, Input->OutputIndex);
				Conn.TargetNodeId = TargetNode.NodeId;
				Conn.TargetExpressionGuid = TargetNode.ExpressionGuid;
				Conn.TargetInputIndex = It.Index;
				Conn.TargetInput = InputPinName(Expr, It.Index);
				Conn.TargetPin = FString::Printf(TEXT("%s:in:%d"), *TargetNode.NodeId, It.Index);
				Out.Connections.Add(MoveTemp(Conn));
			}
		}

		if (UMaterial* Material = Out.Asset.Material)
		{
			UEnum* PropEnum = StaticEnum<EMaterialProperty>();
			if (PropEnum)
			{
				for (int32 EnumIdx = 0; EnumIdx < PropEnum->NumEnums(); ++EnumIdx)
				{
					const int64 RawValue = PropEnum->GetValueByIndex(EnumIdx);
					if (RawValue == INDEX_NONE || RawValue == (int64)MP_MAX)
					{
						continue;
					}

					const EMaterialProperty Property = (EMaterialProperty)RawValue;
					FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
					if (!Input || !Input->Expression)
					{
						continue;
					}

					const int32* SourceNodeIndex = Out.NodeIndexByExpression.Find(Input->Expression);
					if (!SourceNodeIndex || !Out.Nodes.IsValidIndex(*SourceNodeIndex))
					{
						continue;
					}
					const FNodeInfo& SourceNode = Out.Nodes[*SourceNodeIndex];

					FPropertyConnectionInfo Conn;
					Conn.Property = MaterialPropertyName(Property);
					Conn.SourceNodeId = SourceNode.NodeId;
					Conn.SourceExpressionGuid = SourceNode.ExpressionGuid;
					Conn.SourceOutputIndex = Input->OutputIndex;
					Conn.SourceOutput = OutputPinName(Input->Expression, Input->OutputIndex);
					Conn.SourcePin = FString::Printf(TEXT("%s:out:%d"), *SourceNode.NodeId, Input->OutputIndex);
					Out.PropertyConnections.Add(MoveTemp(Conn));
				}
			}
		}

		if (Options.bStableOrder)
		{
			SortSnapshot(Out);
		}
		RebuildNodeIndex(Out);
		AssignConnectionIds(Out);
		FillFunctionInterface(Out);
		return true;
	}

	static TSharedPtr<FJsonObject> BuildSummary(const FGraphSnapshot& Snapshot)
	{
		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetNumberField(TEXT("node_count"), Snapshot.Nodes.Num());
		Summary->SetNumberField(TEXT("connection_count"), Snapshot.Connections.Num());
		Summary->SetNumberField(TEXT("property_connection_count"), Snapshot.PropertyConnections.Num());

		TArray<TSharedPtr<FJsonValue>> DuplicateGuids;
		TSet<FString> SeenGuids;
		for (const FNodeInfo& Node : Snapshot.Nodes)
		{
			if (SeenGuids.Contains(Node.ExpressionGuid))
			{
				continue;
			}
			SeenGuids.Add(Node.ExpressionGuid);

			TArray<int32> Matches;
			Snapshot.NodeIndicesByExpressionGuid.MultiFind(Node.ExpressionGuid, Matches);
			if (Matches.Num() <= 1)
			{
				continue;
			}

			TArray<FString> NodeIds;
			TArray<FString> Classes;
			TArray<FString> Captions;
			for (int32 MatchIndex : Matches)
			{
				if (!Snapshot.Nodes.IsValidIndex(MatchIndex))
				{
					continue;
				}
				const FNodeInfo& Match = Snapshot.Nodes[MatchIndex];
				NodeIds.Add(Match.NodeId);
				Classes.Add(Match.ClassName);
				Captions.Add(Match.Caption);
			}

			TSharedPtr<FJsonObject> DupObj = MakeShared<FJsonObject>();
			DupObj->SetStringField(TEXT("expression_guid"), Node.ExpressionGuid);
			DupObj->SetArrayField(TEXT("node_ids"), StringArray(NodeIds));
			DupObj->SetArrayField(TEXT("classes"), StringArray(Classes));
			DupObj->SetArrayField(TEXT("captions"), StringArray(Captions));
			DuplicateGuids.Add(MakeShared<FJsonValueObject>(DupObj));
		}
		Summary->SetNumberField(TEXT("unique_expression_guid_count"), SeenGuids.Num());
		Summary->SetNumberField(TEXT("duplicate_expression_guid_count"), DuplicateGuids.Num());
		Summary->SetArrayField(TEXT("duplicate_expression_guids"), DuplicateGuids);

		TMap<FString, int32> TypeCounts;
		for (const FNodeInfo& Node : Snapshot.Nodes)
		{
			TypeCounts.FindOrAdd(Node.ClassName)++;
		}

		TSharedPtr<FJsonObject> TypeCountsObj = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : TypeCounts)
		{
			TypeCountsObj->SetNumberField(Pair.Key, Pair.Value);
		}
		Summary->SetObjectField(TEXT("node_type_counts"), TypeCountsObj);

		TArray<TSharedPtr<FJsonValue>> PropertySummary;
		for (const FPropertyConnectionInfo& Conn : Snapshot.PropertyConnections)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("property"), Conn.Property);
			Obj->SetStringField(TEXT("source_node_id"), Conn.SourceNodeId);
			Obj->SetStringField(TEXT("source_expression_guid"), Conn.SourceExpressionGuid);
			Obj->SetStringField(TEXT("source_guid"), Conn.SourceExpressionGuid);
			Obj->SetStringField(TEXT("source_output"), Conn.SourceOutput);
			Obj->SetNumberField(TEXT("source_output_index"), Conn.SourceOutputIndex);
			PropertySummary.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Summary->SetArrayField(TEXT("property_connections"), PropertySummary);
		return Summary;
	}

	static void AddNodeAndNeighbors(
		const FGraphSnapshot& Snapshot,
		const FString& NodeId,
		TSet<int32>& NodeIndices,
		TSet<int32>& ConnectionIndices,
		TSet<int32>& PropertyConnectionIndices)
	{
		if (const int32* NodeIndex = Snapshot.NodeIndexById.Find(NodeId))
		{
			NodeIndices.Add(*NodeIndex);
		}

		for (int32 Index = 0; Index < Snapshot.Connections.Num(); ++Index)
		{
			const FConnectionInfo& Conn = Snapshot.Connections[Index];
			if (Conn.SourceNodeId == NodeId || Conn.TargetNodeId == NodeId)
			{
				ConnectionIndices.Add(Index);
				if (const int32* Src = Snapshot.NodeIndexById.Find(Conn.SourceNodeId))
				{
					NodeIndices.Add(*Src);
				}
				if (const int32* Dst = Snapshot.NodeIndexById.Find(Conn.TargetNodeId))
				{
					NodeIndices.Add(*Dst);
				}
			}
		}

		for (int32 Index = 0; Index < Snapshot.PropertyConnections.Num(); ++Index)
		{
			const FPropertyConnectionInfo& Conn = Snapshot.PropertyConnections[Index];
			if (Conn.SourceNodeId == NodeId)
			{
				PropertyConnectionIndices.Add(Index);
				if (const int32* Src = Snapshot.NodeIndexById.Find(Conn.SourceNodeId))
				{
					NodeIndices.Add(*Src);
				}
			}
		}
	}

	static void BuildIncludedSets(
		const FGraphSnapshot& Snapshot,
		const FGraphOptions& Options,
		TSet<int32>& NodeIndices,
		TSet<int32>& ConnectionIndices,
		TSet<int32>& PropertyConnectionIndices,
		TArray<FString>& Warnings)
	{
		const FString Mode = Options.Mode.ToLower();
		if (Mode == TEXT("summary"))
		{
			return;
		}

		if (Mode == TEXT("full"))
		{
			for (int32 Index = 0; Index < Snapshot.Nodes.Num(); ++Index) NodeIndices.Add(Index);
			for (int32 Index = 0; Index < Snapshot.Connections.Num(); ++Index) ConnectionIndices.Add(Index);
			for (int32 Index = 0; Index < Snapshot.PropertyConnections.Num(); ++Index) PropertyConnectionIndices.Add(Index);
			return;
		}

		if (Mode == TEXT("node"))
		{
			const FString Selector = NormalizeGuidString(Options.NodeGuid);
			if (Selector.IsEmpty())
			{
				Warnings.Add(FString::Printf(TEXT("node_guid '%s' was not found"), *Options.NodeGuid));
				return;
			}

			if (Snapshot.NodeIndexById.Contains(Selector))
			{
				AddNodeAndNeighbors(Snapshot, Selector, NodeIndices, ConnectionIndices, PropertyConnectionIndices);
				return;
			}

			TArray<int32> Matches;
			Snapshot.NodeIndicesByExpressionGuid.MultiFind(Selector, Matches);
			if (Matches.Num() == 0)
			{
				Warnings.Add(FString::Printf(TEXT("node_guid '%s' was not found"), *Options.NodeGuid));
				return;
			}
			if (Matches.Num() > 1)
			{
				Warnings.Add(FString::Printf(
					TEXT("node_guid '%s' matched %d nodes by expression_guid; use node_id for an unambiguous lookup"),
					*Options.NodeGuid,
					Matches.Num()));
			}
			for (int32 MatchIndex : Matches)
			{
				if (Snapshot.Nodes.IsValidIndex(MatchIndex))
				{
					AddNodeAndNeighbors(Snapshot, Snapshot.Nodes[MatchIndex].NodeId, NodeIndices, ConnectionIndices, PropertyConnectionIndices);
				}
			}
			return;
		}

		if (Mode == TEXT("subgraph"))
		{
			TMultiMap<FString, int32> IncomingByTarget;
			for (int32 Index = 0; Index < Snapshot.Connections.Num(); ++Index)
			{
				IncomingByTarget.Add(Snapshot.Connections[Index].TargetNodeId, Index);
			}

			TArray<TPair<FString, int32>> Queue;
			TSet<FString> VisitedNodes;

			for (int32 Index = 0; Index < Snapshot.PropertyConnections.Num(); ++Index)
			{
				const FPropertyConnectionInfo& Conn = Snapshot.PropertyConnections[Index];
				if (!PropertyMatches(Conn.Property, Options.PropertyName))
				{
					continue;
				}

				PropertyConnectionIndices.Add(Index);
				if (const int32* SourceIndex = Snapshot.NodeIndexById.Find(Conn.SourceNodeId))
				{
					NodeIndices.Add(*SourceIndex);
					Queue.Add(TPair<FString, int32>(Conn.SourceNodeId, 0));
					VisitedNodes.Add(Conn.SourceNodeId);
				}
			}

			if (Queue.Num() == 0)
			{
				Warnings.Add(FString::Printf(TEXT("property_name '%s' has no connected source"), *Options.PropertyName));
				return;
			}

			for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
			{
				const FString CurrentNodeId = Queue[QueueIndex].Key;
				const int32 CurrentDepth = Queue[QueueIndex].Value;
				if (Options.MaxDepth > 0 && CurrentDepth >= Options.MaxDepth)
				{
					continue;
				}

				TArray<int32> IncomingConnections;
				IncomingByTarget.MultiFind(CurrentNodeId, IncomingConnections);
				for (int32 ConnIndex : IncomingConnections)
				{
					if (!Snapshot.Connections.IsValidIndex(ConnIndex))
					{
						continue;
					}
					const FConnectionInfo& Conn = Snapshot.Connections[ConnIndex];
					ConnectionIndices.Add(ConnIndex);
					if (const int32* SourceIndex = Snapshot.NodeIndexById.Find(Conn.SourceNodeId))
					{
						NodeIndices.Add(*SourceIndex);
					}
					if (const int32* TargetIndex = Snapshot.NodeIndexById.Find(Conn.TargetNodeId))
					{
						NodeIndices.Add(*TargetIndex);
					}
					if (!VisitedNodes.Contains(Conn.SourceNodeId))
					{
						VisitedNodes.Add(Conn.SourceNodeId);
						Queue.Add(TPair<FString, int32>(Conn.SourceNodeId, CurrentDepth + 1));
					}
				}
			}
			return;
		}

		Warnings.Add(FString::Printf(TEXT("unknown mode '%s'; returned summary"), *Options.Mode));
	}

	static TSet<FString> IncludedEdgeIds(
		const FGraphSnapshot& Snapshot,
		const TSet<int32>& ConnectionIndices,
		const TSet<int32>& PropertyConnectionIndices)
	{
		TSet<FString> Ids;
		for (int32 Index : ConnectionIndices)
		{
			if (Snapshot.Connections.IsValidIndex(Index))
			{
				Ids.Add(Snapshot.Connections[Index].Id);
			}
		}
		for (int32 Index : PropertyConnectionIndices)
		{
			if (Snapshot.PropertyConnections.IsValidIndex(Index))
			{
				Ids.Add(Snapshot.PropertyConnections[Index].Id);
			}
		}
		return Ids;
	}

	static TSharedPtr<FJsonObject> PinToJson(const FPinInfo& Pin, const TSet<FString>& EdgeIds)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("pin_id"), Pin.PinId);
		Obj->SetStringField(TEXT("name"), Pin.Name);
		Obj->SetNumberField(TEXT("index"), Pin.Index);
		Obj->SetStringField(TEXT("type"), Pin.Type);
		Obj->SetStringField(TEXT("default_value"), Pin.DefaultValue);

		TArray<FString> FilteredConnectionIds;
		for (const FString& Id : Pin.ConnectionIds)
		{
			if (EdgeIds.Contains(Id))
			{
				FilteredConnectionIds.Add(Id);
			}
		}
		Obj->SetArrayField(TEXT("connection_ids"), StringArray(FilteredConnectionIds));
		return Obj;
	}

	static TSharedPtr<FJsonObject> NodeToJson(
		const FNodeInfo& Node,
		const FGraphOptions& Options,
		const TSet<FString>& EdgeIds)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("node_id"), Node.NodeId);
		Obj->SetStringField(TEXT("expression_guid"), Node.ExpressionGuid);
		Obj->SetStringField(TEXT("guid"), Node.ExpressionGuid);
		Obj->SetStringField(TEXT("class"), Node.ClassName);
		Obj->SetStringField(TEXT("name"), Node.Name);
		if (Options.bIncludeCaptions)
		{
			Obj->SetStringField(TEXT("caption"), Node.Caption);
		}
		else
		{
			Obj->SetStringField(TEXT("caption"), FString());
		}
		Obj->SetStringField(TEXT("desc"), Node.Desc);

		TSharedPtr<FJsonObject> Pos = MakeShared<FJsonObject>();
		Pos->SetNumberField(TEXT("x"), Node.X);
		Pos->SetNumberField(TEXT("y"), Node.Y);
		Obj->SetObjectField(TEXT("pos"), Pos);

		TArray<TSharedPtr<FJsonValue>> Inputs;
		TArray<TSharedPtr<FJsonValue>> Outputs;
		if (Options.bIncludePins)
		{
			for (const FPinInfo& Pin : Node.Inputs)
			{
				Inputs.Add(MakeShared<FJsonValueObject>(PinToJson(Pin, EdgeIds)));
			}
			for (const FPinInfo& Pin : Node.Outputs)
			{
				Outputs.Add(MakeShared<FJsonValueObject>(PinToJson(Pin, EdgeIds)));
			}
		}
		Obj->SetArrayField(TEXT("inputs"), Inputs);
		Obj->SetArrayField(TEXT("outputs"), Outputs);

		if (Options.bIncludeProperties)
		{
			Obj->SetObjectField(TEXT("properties"), Node.Properties);
		}
		else
		{
			Obj->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> ConnectionToJson(const FConnectionInfo& Conn)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Conn.Id);
		Obj->SetStringField(TEXT("source_node_id"), Conn.SourceNodeId);
		Obj->SetStringField(TEXT("source_expression_guid"), Conn.SourceExpressionGuid);
		Obj->SetStringField(TEXT("source_guid"), Conn.SourceExpressionGuid);
		Obj->SetStringField(TEXT("source_pin"), Conn.SourcePin);
		Obj->SetStringField(TEXT("source_output"), Conn.SourceOutput);
		Obj->SetNumberField(TEXT("source_output_index"), Conn.SourceOutputIndex);
		Obj->SetStringField(TEXT("target_node_id"), Conn.TargetNodeId);
		Obj->SetStringField(TEXT("target_expression_guid"), Conn.TargetExpressionGuid);
		Obj->SetStringField(TEXT("target_guid"), Conn.TargetExpressionGuid);
		Obj->SetStringField(TEXT("target_pin"), Conn.TargetPin);
		Obj->SetStringField(TEXT("target_input"), Conn.TargetInput);
		Obj->SetNumberField(TEXT("target_input_index"), Conn.TargetInputIndex);
		Obj->SetStringField(TEXT("kind"), TEXT("expression"));
		return Obj;
	}

	static TSharedPtr<FJsonObject> PropertyConnectionToJson(const FPropertyConnectionInfo& Conn)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Conn.Id);
		Obj->SetStringField(TEXT("property"), Conn.Property);
		Obj->SetStringField(TEXT("source_node_id"), Conn.SourceNodeId);
		Obj->SetStringField(TEXT("source_expression_guid"), Conn.SourceExpressionGuid);
		Obj->SetStringField(TEXT("source_guid"), Conn.SourceExpressionGuid);
		Obj->SetStringField(TEXT("source_pin"), Conn.SourcePin);
		Obj->SetStringField(TEXT("source_output"), Conn.SourceOutput);
		Obj->SetNumberField(TEXT("source_output_index"), Conn.SourceOutputIndex);
		Obj->SetStringField(TEXT("target_kind"), TEXT("material_property"));
		return Obj;
	}

	static void AddAdjacency(
		TSharedPtr<FJsonObject> Root,
		const FGraphSnapshot& Snapshot,
		const TSet<int32>& ConnectionIndices,
		const TSet<int32>& PropertyConnectionIndices)
	{
		TMap<FString, TArray<FString>> BySource;
		TMap<FString, TArray<FString>> ByTarget;

		for (int32 Index : ConnectionIndices)
		{
			if (!Snapshot.Connections.IsValidIndex(Index))
			{
				continue;
			}
			const FConnectionInfo& Conn = Snapshot.Connections[Index];
			BySource.FindOrAdd(Conn.SourceNodeId).Add(Conn.Id);
			ByTarget.FindOrAdd(Conn.TargetNodeId).Add(Conn.Id);
		}

		for (int32 Index : PropertyConnectionIndices)
		{
			if (!Snapshot.PropertyConnections.IsValidIndex(Index))
			{
				continue;
			}
			const FPropertyConnectionInfo& Conn = Snapshot.PropertyConnections[Index];
			BySource.FindOrAdd(Conn.SourceNodeId).Add(Conn.Id);
			ByTarget.FindOrAdd(FString::Printf(TEXT("Material:%s"), *Conn.Property)).Add(Conn.Id);
		}

		auto MapToJson = [](const TMap<FString, TArray<FString>>& Map)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			for (const TPair<FString, TArray<FString>>& Pair : Map)
			{
				Obj->SetArrayField(Pair.Key, StringArray(Pair.Value));
			}
			return Obj;
		};

		TSharedPtr<FJsonObject> Adjacency = MakeShared<FJsonObject>();
		Adjacency->SetObjectField(TEXT("by_source"), MapToJson(BySource));
		Adjacency->SetObjectField(TEXT("by_target"), MapToJson(ByTarget));
		Root->SetObjectField(TEXT("adjacency"), Adjacency);
	}

	static TSharedPtr<FJsonObject> BuildResultJson(
		const FGraphSnapshot& Snapshot,
		const FGraphOptions& Options,
		bool bForceSummaryOnly,
		bool bTruncated,
		const TArray<FString>& ExtraWarnings)
	{
		TSet<int32> NodeIndices;
		TSet<int32> ConnectionIndices;
		TSet<int32> PropertyConnectionIndices;
		TArray<FString> Warnings = Snapshot.Warnings;
		Warnings.Append(ExtraWarnings);

		if (!bForceSummaryOnly)
		{
			BuildIncludedSets(Snapshot, Options, NodeIndices, ConnectionIndices, PropertyConnectionIndices, Warnings);
		}

		if (Options.MaxNodes > 0 && NodeIndices.Num() > Options.MaxNodes)
		{
			NodeIndices.Reset();
			ConnectionIndices.Reset();
			PropertyConnectionIndices.Reset();
			bTruncated = true;
			Warnings.Add(FString::Printf(TEXT("result exceeded max_nodes=%d; returned summary only"), Options.MaxNodes));
		}

		const TSet<FString> EdgeIds = IncludedEdgeIds(Snapshot, ConnectionIndices, PropertyConnectionIndices);

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schema_version"), 2);
		Root->SetBoolField(TEXT("success"), true);
		Root->SetStringField(TEXT("mode"), Options.Mode);
		Root->SetStringField(TEXT("asset_type"), Snapshot.Asset.AssetType);
		Root->SetStringField(TEXT("requested_path"), Snapshot.Asset.RequestedPath);
		Root->SetStringField(TEXT("path"), Snapshot.Asset.Path);
		Root->SetStringField(TEXT("resolved_graph_path"), Snapshot.Asset.ResolvedGraphPath);
		Root->SetStringField(TEXT("base_material_path"), Snapshot.Asset.BaseMaterialPath);
		Root->SetBoolField(TEXT("is_material_instance"), Snapshot.Asset.bIsMaterialInstance);
		Root->SetObjectField(TEXT("material"), Snapshot.MaterialObject);
		Root->SetObjectField(TEXT("summary"), BuildSummary(Snapshot));
		Root->SetObjectField(TEXT("function_interface"), Snapshot.FunctionInterfaceObject);
		Root->SetBoolField(TEXT("truncated"), bTruncated);
		Root->SetArrayField(TEXT("warnings"), StringArray(Warnings));

		TArray<TSharedPtr<FJsonValue>> Nodes;
		TArray<TSharedPtr<FJsonValue>> Connections;
		TArray<TSharedPtr<FJsonValue>> PropertyConnections;

		for (int32 Index = 0; Index < Snapshot.Nodes.Num(); ++Index)
		{
			if (NodeIndices.Contains(Index))
			{
				Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Snapshot.Nodes[Index], Options, EdgeIds)));
			}
		}
		for (int32 Index = 0; Index < Snapshot.Connections.Num(); ++Index)
		{
			if (ConnectionIndices.Contains(Index))
			{
				Connections.Add(MakeShared<FJsonValueObject>(ConnectionToJson(Snapshot.Connections[Index])));
			}
		}
		for (int32 Index = 0; Index < Snapshot.PropertyConnections.Num(); ++Index)
		{
			if (PropertyConnectionIndices.Contains(Index))
			{
				PropertyConnections.Add(MakeShared<FJsonValueObject>(PropertyConnectionToJson(Snapshot.PropertyConnections[Index])));
			}
		}

		Root->SetArrayField(TEXT("nodes"), Nodes);
		Root->SetArrayField(TEXT("connections"), Connections);
		Root->SetArrayField(TEXT("property_connections"), PropertyConnections);

		if (Options.bIncludeAdjacency && !bForceSummaryOnly)
		{
			AddAdjacency(Root, Snapshot, ConnectionIndices, PropertyConnectionIndices);
		}

		return Root;
	}

	static FString ResolveOutputPath(const FString& OutputPath)
	{
		if (FPaths::IsRelative(OutputPath))
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealBridge"), OutputPath));
		}
		return OutputPath;
	}
}

FString UUnrealBridgeMaterialLibrary::GetMaterialGraphJson(
	const FString& MaterialPath,
	const FString& OptionsJson)
{
	using namespace BridgeMaterialGraphJson;

	FGraphOptions Options;
	ParseOptions(OptionsJson, Options);
	if (Options.Mode != TEXT("summary") &&
		Options.Mode != TEXT("full") &&
		Options.Mode != TEXT("node") &&
		Options.Mode != TEXT("subgraph"))
	{
		Options.Mode = TEXT("summary");
	}

	FGraphSnapshot Snapshot;
	if (!BuildSnapshot(MaterialPath, Options, Snapshot))
	{
		return SerializeJsonObject(ErrorJson(MaterialPath, Options.Mode, Snapshot.Asset.Error));
	}

	TArray<FString> Warnings;
	const bool bForceSummaryOnly = Options.Mode == TEXT("summary");
	TSharedPtr<FJsonObject> FullResult = BuildResultJson(Snapshot, Options, bForceSummaryOnly, false, Warnings);

	if (!Options.OutputPath.IsEmpty())
	{
		const FString FullJson = SerializeJsonObject(FullResult);
		const FString FinalPath = ResolveOutputPath(Options.OutputPath);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FinalPath), true);
		if (!FFileHelper::SaveStringToFile(
			FullJson,
			*FinalPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return SerializeJsonObject(ErrorJson(
				MaterialPath,
				Options.Mode,
				FString::Printf(TEXT("failed to write output_path '%s'"), *FinalPath)));
		}

		TArray<FString> ExportWarnings = Warnings;
		TSharedPtr<FJsonObject> ExportResult = BuildResultJson(Snapshot, Options, true, false, ExportWarnings);
		ExportResult->SetBoolField(TEXT("exported"), true);
		ExportResult->SetStringField(TEXT("output_path"), FinalPath);
		ExportResult->SetNumberField(TEXT("output_bytes"), FullJson.Len());
		return SerializeJsonObject(ExportResult);
	}

	const FString Json = SerializeJsonObject(FullResult);
	if (Options.MaxBytes > 0 && Json.Len() > Options.MaxBytes)
	{
		TArray<FString> LimitWarnings;
		LimitWarnings.Add(FString::Printf(TEXT("result exceeded max_bytes=%d; returned summary only"), Options.MaxBytes));
		return SerializeJsonObject(BuildResultJson(Snapshot, Options, true, true, LimitWarnings));
	}

	return Json;
}
