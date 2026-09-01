using System.IO;
using UnrealBuildTool;

public class MeshForge : ModuleRules
{
	/**
	 * Whether a sibling plugin is installed beside this one *and* carries the module we want.
	 *
	 * Needed because "optional" has to hold at *build* time, not only at runtime. Naming a module in
	 * PrivateIncludePathModuleNames takes no link and adds no .uplugin dependency - but UBT still has
	 * to resolve the name, and refuses the whole build with "Could not find definition for module"
	 * when it cannot. A plugin packaged and installed on its own then fails to compile for the
	 * customer, which is the exact opposite of what the header-only pattern was for.
	 *
	 * The module is checked rather than only the plugin, because the two can disagree across
	 * versions. Walking up from this module covers every layout a plugin is ever in: beside us in a
	 * project's Plugins folder, in our own Plugins/Forge, or under Engine/Plugins/AutomationForge.
	 *
	 * Copied rather than shared, and deliberately so - see the note about deduplication being a
	 * weaker reason than independence in the family's rules.
	 */
	private bool IsModulePresent(string PluginName, string ModuleName)
	{
		for (DirectoryInfo Dir = new DirectoryInfo(ModuleDirectory); Dir != null; Dir = Dir.Parent)
		{
			string Plugin = Path.Combine(Dir.FullName, PluginName);

			if (File.Exists(Path.Combine(Plugin, PluginName + ".uplugin")))
			{
				return File.Exists(
					Path.Combine(Plugin, "Source", ModuleName, ModuleName + ".Build.cs"));
			}
		}

		return false;
	}

	public MeshForge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",           // public headers derive from UDataAsset / reference UStaticMesh
				"DeveloperSettings",
				"EditorSubsystem",  // UMeshForgeSubsystem derives from UEditorSubsystem
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"HTTP",             // provider REST calls
				"Json",
				"JsonUtilities",
				"Projects",         // IPluginManager
				"UnrealEd",         // editor subsystem, asset creation
				"AssetTools",
				"AssetRegistry",
				"InterchangeCore",  // the glTF import path
				"InterchangeEngine",
				"StaticMeshEditor", // UStaticMeshEditorSubsystem: collision, Nanite, build settings
				"MeshDescription",  // reading back what was imported, to report it honestly
				"StaticMeshDescription",
				"GLTFExporter",   // writing a project mesh back out, so a post step can work on one
				"ImageCore",        // FImage: reading a texture back out in any source format
				"ImageWrapper",     // the PNG codec FImageUtils compresses through
				"RenderCore",       // texture platform data, for reading a UTexture2D back out
				"Slate",
				"SlateCore",
			}
			);

		// The credential store talks to the Windows Credential Manager directly. Other platforms
		// fall back to the environment variable until a Keychain/libsecret backend is written.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("Advapi32.lib");
		}

		// Headers only, deliberately not a link and not a .uplugin dependency: with ForgeKeys absent
		// the module lookup returns null and this plugin carries on with its own settings page.
		bool bWithForgeKeys = IsModulePresent("AutomationForgeHub", "ForgeKeys");

		if (bWithForgeKeys)
		{
			PrivateIncludePathModuleNames.Add("ForgeKeys");
		}

		PublicDefinitions.Add("WITH_FORGE_KEYS=" + (bWithForgeKeys ? "1" : "0"));
	}
}
