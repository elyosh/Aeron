# Aeron Flight Model: import, author and export Aeron flight models
# (glTF with AERON_flight_model) in Blender. See README.md.

from . import (animation, animation_operators, animation_ui, exporting, flipbook_preview, gizmos, importing,
               libraries, operators, opt_import, overlays, preferences, preview, properties, ui)

# Blender's glTF exporter and importer look for these on enabled add-ons;
# they act only during this add-on's own export and import.
glTF2ExportUserExtension = exporting.glTF2ExportUserExtension
glTF2ImportUserExtension = importing.glTF2ImportUserExtension

_modules = (preferences, libraries, animation, flipbook_preview, properties, preview, operators, animation_operators,
            exporting, importing, opt_import, overlays, gizmos, ui, animation_ui)


def register():
    for module in _modules:
        module.register()


def unregister():
    for module in reversed(_modules):
        module.unregister()
