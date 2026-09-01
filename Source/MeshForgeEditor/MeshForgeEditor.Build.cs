using UnrealBuildTool;

public class MeshForgeEditor : ModuleRules
{
	public MeshForgeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"MeshForge",       // the definition asset this exists to present
				"AssetDefinition", // UAssetDefinition, which decides colour, category and what opens
				"AssetTools",      // creating a definition through the ordinary new-asset dialogue
				"UnrealEd",        // UFactory
				"Slate",
				"SlateCore",
				"PropertyEditor",
				"InputCore",
				"ToolMenus",           // the toolbar the stage buttons live on
				"EditorStyle",
				"EditorWidgets",
				"AdvancedPreviewScene", // the orbiting mesh viewport, same one the Static Mesh editor uses
				"AssetRegistry",        // finding the pipeline assets a stage may be pointed at
				"ContentBrowser",       // "show me this in the browser", from the image grid
				"RenderCore",
				"MeshDescription",
			}
			);
	}
}
