# TimeSplitters: Future Perfect models in Blender: File > Import > TimeSplitters FP model opens one of the game's
# models (from its paks, by name) through glTF, with its skeleton and textures; the sidebar's TSFP tab writes the edited
# scene into a mod in the game's mods folder. The conversion is tools/tsfp_model.py, packed beside this file.
import os
import tempfile

import bpy
from bpy.props import StringProperty
from bpy.types import AddonPreferences, Operator, Panel

from . import tsfp_model as T

bl_info = {
    "name": "TimeSplitters Future Perfect models",
    "author": "TSFP XBE Recomp",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "File > Import > TimeSplitters FP model; View3D > Sidebar > TSFP",
    "description": "Open the game's models, edit them and write them into a mod",
    "category": "Import-Export",
}

_games = {}


def prefs(context):
    return context.preferences.addons[__package__].preferences


def game(context):
    folder = bpy.path.abspath(prefs(context).game_folder)
    if not folder:
        raise T.ModelError("set the game's folder (the one with TSFP.exe) in the add-on's preferences")
    g = _games.get(folder)
    if g is None:
        g = _games[folder] = T.Game(folder)
    return g


def mods_folder(g):
    # the game's folder holds assets\ and mods\
    return os.path.join(os.path.dirname(g.root), "mods")


def call(op, **kw):
    """An operator with the keywords this Blender has (the glTF add-on's change between versions)."""
    known = op.get_rna_type().properties.keys()
    return op(**{k: v for k, v in kw.items() if k in known})


def search_models(self, context, text):
    try:
        names = game(context).models()
    except (T.ModelError, OSError):
        return []
    text = text.lower()
    return [n for n in names if text in n][:400]


def search_poses(self, context, text):
    try:
        names = game(context).bindposes()
    except (T.ModelError, OSError):
        return []
    return [n for n in names if text.lower() in n.lower()]


class TSFP_Prefs(AddonPreferences):
    bl_idname = __package__
    game_folder: StringProperty(name="Game folder", subtype="DIR_PATH",
                                description="The folder of TSFP.exe, with the game in assets")

    def draw(self, context):
        self.layout.prop(self, "game_folder")


class TSFP_OT_import(Operator):
    bl_idname = "import_scene.tsfp_model"
    bl_label = "TimeSplitters FP model"
    bl_description = "Open one of the game's models with its skeleton and textures"
    bl_options = {"REGISTER", "UNDO"}

    model: StringProperty(name="Model", search=search_models,
                          description="Its name in the game, such as chrs/cortezarmouredingame or guns/pistol9")
    bindpose: StringProperty(name="Skeleton", search=search_poses,
                             description="The rest pose to stand it in; empty: found by its name and bones")

    def invoke(self, context, event):
        if not self.model:
            self.model = context.scene.get("tsfp_model", "")
        return context.window_manager.invoke_props_dialog(self, width=420)

    def execute(self, context):
        try:
            g = game(context)
            name, path, m = T.load(g, self.model)
            glb = os.path.join(tempfile.gettempdir(), "tsfp_%s.glb" % name.replace("/", "_"))
            lines = []
            pose = T.to_gltf(g, name, m, glb, self.bindpose or None, log=lines.append)
        except (T.ModelError, OSError) as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        call(bpy.ops.import_scene.gltf, filepath=glb, merge_vertices=False, import_shading="NORMALS")
        context.scene["tsfp_model"] = name
        context.scene["tsfp_bindpose"] = pose or ""
        if not context.scene.get("tsfp_mod"):
            context.scene["tsfp_mod"] = name.split("/")[-1]
        for line in lines:
            self.report({"INFO"}, line)
        self.report({"INFO"}, "Opened %s" % name)
        return {"FINISHED"}


class TSFP_OT_write(Operator):
    bl_idname = "export_scene.tsfp_model"
    bl_label = "Write into mod"
    bl_description = ("Write the scene's model into the mod folder, in place of the game's: the meshes named after "
                      "the model's pieces replace them, any other mesh goes to the bones it is weighted to")
    bl_options = {"REGISTER"}

    def execute(self, context):
        scene = context.scene
        name = scene.get("tsfp_model", "")
        mod = scene.get("tsfp_mod", "") or name.split("/")[-1]
        if not name:
            self.report({"ERROR"}, "Open a model first (File > Import > TimeSplitters FP model)")
            return {"CANCELLED"}
        # pictures the glTF writer would keep as JPEG or WebP are written as PNG
        for img in bpy.data.images:
            if img.source == "FILE" and img.file_format not in ("PNG",):
                try:
                    img.file_format = "PNG"
                except (TypeError, AttributeError):
                    pass
        glb = os.path.join(tempfile.gettempdir(), "tsfp_edited.glb")
        call(bpy.ops.export_scene.gltf, filepath=glb, export_format="GLB", export_extras=True, export_skins=True,
             export_yup=True, export_animations=False, export_morph=False, export_apply=False,
             export_vertex_color="ACTIVE", export_image_format="AUTO", use_visible=False)
        try:
            g = game(context)
            _, path, m = T.load(g, name)
            lines = []
            data, textures = T.from_gltf(g, name, m, glb, scene.get("tsfp_bindpose") or None, log=lines.append)
            folder = os.path.join(mods_folder(g), mod)
            T.write_mod(g, path, data, textures, folder, log=lines.append)
        except (T.ModelError, OSError) as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        for line in lines:
            self.report({"INFO"}, line)
        self.report({"INFO"}, "Written into the mod %s: switch it on in the game's F1 > Mods" % mod)
        return {"FINISHED"}


class TSFP_PT_panel(Panel):
    bl_label = "TimeSplitters FP"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "TSFP"

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        if not prefs(context).game_folder:
            layout.label(text="Set the game's folder in the add-on's preferences", icon="ERROR")
        layout.operator(TSFP_OT_import.bl_idname, text="Open a model", icon="IMPORT")
        if "tsfp_model" in scene:
            layout.label(text=scene["tsfp_model"], icon="OUTLINER_OB_ARMATURE")
            layout.prop(scene, '["tsfp_mod"]', text="Mod")
            layout.operator(TSFP_OT_write.bl_idname, icon="EXPORT")


def menu_import(self, context):
    self.layout.operator(TSFP_OT_import.bl_idname, text="TimeSplitters FP model")


classes = (TSFP_Prefs, TSFP_OT_import, TSFP_OT_write, TSFP_PT_panel)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.TOPBAR_MT_file_import.append(menu_import)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_import)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
