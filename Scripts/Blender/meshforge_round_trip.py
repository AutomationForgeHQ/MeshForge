"""MeshForge's Blender side of the "Edit in Blender" post step.

Blender is started by Unreal as

    blender --python meshforge_round_trip.py -- --meshforge-session <folder>/blender-session.json

The session file names the mesh to edit, any reference meshes to show beside it, and where to write the
result. This script loads them, adds a MeshForge tab to the 3D view's sidebar, and waits. Pressing
"Send back to Unreal" writes the result as glTF and then the manifest - the manifest last, by rename,
because its appearance is what tells Unreal the result is complete.

Nothing is installed into the user's Blender. The panel exists for this session only.

Three things Blender's glTF importer does silently, each of which cost a debugging round once:
  * every UV seam is a row of doubled vertices unless imported with merge_vertices - a brush tears the
    mesh along its seams;
  * imported objects are in quaternion rotation mode, so writing Euler rotation does nothing;
  * a file without normals shades flat, and flat faces export as one vertex per corner.
"""

import json
import os
import struct
import sys
import time

import bpy

SESSION = {}
EDIT_ROLE = "edit"
REFERENCE_ROLE = "reference"
ROLE_KEY = "meshforge_role"


# ---- session -------------------------------------------------------------------------------------

def read_session():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if "--meshforge-session" not in argv:
        raise RuntimeError("MeshForge: no --meshforge-session argument.")
    path = argv[argv.index("--meshforge-session") + 1]
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    data["directory"] = os.path.dirname(os.path.abspath(path))
    return data


def session_path(name):
    return os.path.join(SESSION["directory"], name)


def log(message):
    """Also into the session folder: a Blender started from Unreal has no console anybody reads."""
    print(f"MeshForge: {message}")
    try:
        with open(session_path("blender-log.txt"), "a", encoding="utf-8") as handle:
            handle.write(time.strftime("%H:%M:%S ") + message + "\n")
    except OSError:
        pass


def write_manifest(fields):
    """Written to a temporary name and renamed, so Unreal never reads half a file."""
    fields = dict(fields)
    fields["version"] = 1
    fields["session"] = SESSION["session"]
    fields["sentUtc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    final = session_path(SESSION["manifest"])
    partial = final + ".partial"
    with open(partial, "w", encoding="utf-8") as handle:
        json.dump(fields, handle, indent=2)
    os.replace(partial, final)


# ---- loading -------------------------------------------------------------------------------------

def find_window_context():
    """A window and a 3D view, for operators that need one. Timers run with no context of their own."""
    windows = list(bpy.context.window_manager.windows)
    for window in windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                region = next((r for r in area.regions if r.type == "WINDOW"), None)
                return {"window": window, "screen": window.screen, "area": area, "region": region}
    if windows:
        return {"window": windows[0], "screen": windows[0].screen}
    return {}  # background: no windows, and the operators used here do not need one


def import_glb(path):
    """Import a .glb and return the objects it created."""
    before = set(bpy.data.objects)
    with bpy.context.temp_override(**find_window_context()):
        bpy.ops.import_scene.gltf(filepath=path, merge_vertices=True)
    created = [obj for obj in bpy.data.objects if obj not in before]
    for obj in created:
        # Setting the mode converts the stored rotation, so nothing moves - it only starts listening.
        obj.rotation_mode = "XYZ"
    return created


def smooth_if_no_normals(obj):
    if obj.type != "MESH":
        return
    mesh = obj.data
    if getattr(mesh, "has_custom_normals", False):
        return
    if hasattr(mesh, "shade_smooth"):
        mesh.shade_smooth()
    else:
        for polygon in mesh.polygons:
            polygon.use_smooth = True


def world_positions(objects):
    """Every vertex of the meshes as they stand, in one order: object order, then vertex order.

    The mesh data rather than the evaluated mesh, so an armature at rest and a mesh posed by one both
    report the shape that is being edited, and the order is the one Unreal matches itself against.
    """
    points = []
    for obj in objects:
        if obj.type != "MESH":
            continue
        matrix = obj.matrix_world
        points.extend(matrix @ vertex.co for vertex in obj.data.vertices)
    return points


def write_positions(path, before, after):
    """Where each vertex arrived and where it was left, for Unreal to match its own mesh against.

    Unreal keeps the mesh it sent - its UVs, material slots, skin weights and morph targets are keyed to
    an order glTF does not preserve - so it moves its own vertices to these answers instead of importing
    what comes back. "GFP1", a uint32 count, then the two blocks as float32 triples, Blender's space and
    metres. The same file MeshForge Garment's fitting runner writes.
    """
    with open(path, "wb") as handle:
        handle.write(b"GFP1")
        handle.write(struct.pack("<I", len(before)))
        for points in (before, after):
            handle.write(struct.pack(f"<{3 * len(points)}f", *(c for p in points for c in p)))


def topology(objects):
    """Vertex and face counts of the evaluated meshes - what the exporter will actually write."""
    depsgraph = bpy.context.evaluated_depsgraph_get()
    vertices = faces = 0
    for obj in objects:
        if obj.type != "MESH":
            continue
        evaluated = obj.evaluated_get(depsgraph)
        mesh = evaluated.to_mesh()
        vertices += len(mesh.vertices)
        faces += len(mesh.polygons)
        evaluated.to_mesh_clear()
    return vertices, faces


def push_undo(message):
    """Record the scene as it is now as an undo step. Harmless where undo is off (background mode)."""
    try:
        bpy.ops.ed.undo_push(message=message)
    except RuntimeError as error:
        log(f"could not record an undo step ({message}): {error}")


def edit_objects():
    return [obj for obj in bpy.data.objects if obj.get(ROLE_KEY) == EDIT_ROLE]


def setup():
    try:
        load_scene()
    except Exception:  # noqa: BLE001 - anything, so it reaches the log rather than a console nobody sees
        import traceback
        log("could not set the scene up:\n" + traceback.format_exc())
    return None  # a timer that returns None runs once


def load_scene():
    # An empty scene. Removing through the data API rather than an operator, which needs a context
    # this early in startup does not reliably have.
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)

    references = bpy.data.collections.new("MeshForge References")
    bpy.context.scene.collection.children.link(references)

    for entry in SESSION.get("references", []):
        created = import_glb(session_path(entry["file"]))

        # A skinned file brings an armature, and the importer gives its bones a display shape: an
        # "Icosphere" mesh two metres across, left in the scene as an ordinary visible object.
        # Measured on a MetaHuman body, 342 bones all pointing at one such sphere around the hips.
        # It is the bones' shape, not part of the reference, so it is hidden with the armature.
        bone_shapes = {
            bone.custom_shape
            for obj in created if obj.type == "ARMATURE"
            for bone in obj.pose.bones if bone.custom_shape is not None
        }

        for obj in created:
            obj[ROLE_KEY] = REFERENCE_ROLE
            for collection in list(obj.users_collection):
                collection.objects.unlink(obj)
            references.objects.link(obj)
            obj.hide_select = True
            obj.lock_location = obj.lock_rotation = obj.lock_scale = (True, True, True)
            if obj.type == "ARMATURE" or obj in bone_shapes:
                obj.hide_set(True)
                obj.hide_render = True
            else:
                smooth_if_no_normals(obj)

    edited = import_glb(session_path(SESSION["input"]))

    # A skinned mesh brings its armature, which its modifier needs and nobody edits here, and the importer
    # gives every bone a display shape: one two-metre "Icosphere" mesh object, which is a mesh like any
    # other. Neither is the thing being edited - counted in, a stray 42 vertices join the garment and an
    # edit lands on the wrong mesh - so both are context: hidden, locked, and left out of what is sent back.
    bone_shapes = {
        bone.custom_shape
        for obj in edited if obj.type == "ARMATURE"
        for bone in obj.pose.bones if bone.custom_shape is not None
    }
    meshes = [obj for obj in edited if obj.type == "MESH" and obj not in bone_shapes]
    for obj in edited:
        is_context = obj.type == "ARMATURE" or obj in bone_shapes
        obj[ROLE_KEY] = REFERENCE_ROLE if is_context else EDIT_ROLE
        if is_context:
            obj.hide_set(True)
            obj.hide_render = True
            obj.hide_select = True
    for obj in meshes:
        smooth_if_no_normals(obj)

    SESSION["baseline"] = topology(meshes)
    if SESSION.get("positions"):
        SESSION["arrived"] = world_positions(meshes)

    context = find_window_context()
    with bpy.context.temp_override(**context):
        for obj in bpy.context.view_layer.objects:
            obj.select_set(False)
        for obj in meshes:
            obj.select_set(True)
        if meshes:
            bpy.context.view_layer.objects.active = meshes[0]
        if "area" in context:
            bpy.ops.view3d.view_selected()
            context["area"].spaces.active.show_region_ui = True

        # Operators called from a script push no undo steps, so without these the history holds only the
        # empty startup scene: the first Ctrl+Z after a sculpt stroke jumps back past every import, into a
        # scene whose materials no longer exist, and Blender crashed rebuilding it (seen 2026-09-14,
        # null read in DepsgraphNodeBuilder::build_materials). A step for the loaded scene, and another
        # once in sculpt mode, give undo somewhere real to stop.
        push_undo("MeshForge: loaded for editing")
        if SESSION.get("sculpt") and len(meshes) == 1:
            bpy.ops.object.mode_set(mode="SCULPT")
            push_undo("MeshForge: sculpt mode")

    log(f"editing '{SESSION.get('definition')}' - {SESSION['baseline'][0]} vertices, "
        f"{len(SESSION.get('references', []))} reference(s), mode {bpy.context.object.mode if bpy.context.object else 'none'}. "
        f"Send back to Unreal from the sidebar's MeshForge tab.")


# ---- the panel -----------------------------------------------------------------------------------

class MESHFORGE_OT_send_back(bpy.types.Operator):
    """Write the edited mesh and tell Unreal it is ready"""

    bl_idname = "meshforge.send_back"
    bl_label = "Send back to Unreal"

    def execute(self, context):
        objects = [obj for obj in edit_objects() if obj.type == "MESH"]
        if not objects:
            self.report({"ERROR"}, "MeshForge: the mesh being edited is gone. Nothing was sent.")
            return {"CANCELLED"}

        # Sculpt and edit changes are only in the mesh data once the object leaves those modes.
        if context.object is not None and context.object.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")

        for obj in context.view_layer.objects:
            obj.select_set(False)
        for obj in objects:
            obj.hide_select = False
            obj.select_set(True)
        context.view_layer.objects.active = objects[0]

        vertices, faces = topology(objects)
        before_vertices, before_faces = SESSION["baseline"]

        final = session_path(SESSION["result"])
        partial = final[:-len(".glb")] + ".partial.glb"
        bpy.ops.export_scene.gltf(
            filepath=partial,
            export_format="GLB",
            use_selection=True,
            export_apply=True,
            export_yup=True,
            export_animations=False,
            export_skins=False,
            export_morph=False,
            # Always real materials. "PLACEHOLDER" sounds right for "Unreal restores its own" and comes
            # back with no material slots at all (measured on Blender 5.2), so there is nothing to
            # restore onto. The slots are what matter; Unreal swaps its own materials back in by slot.
            export_materials="EXPORT",
        )
        os.replace(partial, final)

        # A copy of the work, so it can be reopened; the session's own file stays untitled.
        try:
            bpy.ops.wm.save_as_mainfile(filepath=session_path("edit.blend"), copy=True)
        except RuntimeError as error:
            print(f"MeshForge: could not keep a .blend of the edit: {error}")

        manifest = {
            "status": "sent",
            "result": SESSION["result"],
            "verticesBefore": before_vertices,
            "verticesAfter": vertices,
            "facesBefore": before_faces,
            "facesAfter": faces,
            "topologyChanged": (vertices, faces) != (before_vertices, before_faces),
        }

        arrived = SESSION.get("arrived")
        if arrived is not None:
            left = world_positions(objects)
            if len(left) == len(arrived):
                write_positions(session_path(SESSION["positions"]), arrived, left)
                manifest["positions"] = SESSION["positions"]
            else:
                # Unreal says so rather than guessing: a mesh whose vertices are not the ones it sent
                # cannot take its weights and morph targets with it.
                log(f"positions not written: {len(arrived)} vertices arrived and {len(left)} left.")

        log(f"sent {vertices} vertices (opened with {before_vertices}).")
        write_manifest(manifest)

        self.report({"INFO"}, "MeshForge: sent. Unreal is importing it - you can close Blender or keep editing and send again.")
        return {"FINISHED"}


class MESHFORGE_OT_cancel(bpy.types.Operator):
    """Tell Unreal to stop waiting. Nothing is imported"""

    bl_idname = "meshforge.cancel"
    bl_label = "Cancel the edit"

    def execute(self, context):
        write_manifest({"status": "cancelled"})
        self.report({"INFO"}, "MeshForge: cancelled. Unreal has stopped waiting.")
        return {"FINISHED"}


class MESHFORGE_PT_round_trip(bpy.types.Panel):
    bl_label = "MeshForge"
    bl_idname = "MESHFORGE_PT_round_trip"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "MeshForge"

    def draw(self, context):
        layout = self.layout
        layout.label(text=SESSION.get("definition", "MeshForge"), icon="MESH_DATA")
        if "baseline" in SESSION:
            layout.label(text=f"{SESSION['baseline'][0]} vertices when opened")
        column = layout.column()
        column.scale_y = 1.6
        column.operator(MESHFORGE_OT_send_back.bl_idname, icon="EXPORT")
        layout.operator(MESHFORGE_OT_cancel.bl_idname, icon="CANCEL")
        layout.separator()
        layout.label(text="Keep topology if the mesh will be")
        layout.label(text="retextured or morph-baked later.")


CLASSES = (MESHFORGE_OT_send_back, MESHFORGE_OT_cancel, MESHFORGE_PT_round_trip)


def main():
    SESSION.update(read_session())
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    # After startup has finished building the window, so import and view operators have somewhere to run.
    bpy.app.timers.register(setup, first_interval=0.2)


main()
