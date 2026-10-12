# Aeron Flight Model: import, author and export Aeron flight models
# (glTF with AERON_flight_model) in Blender. See README.md.

from . import exporting, importing, operators, overlays, preferences, preview, properties, ui

# Blender's glTF exporter and importer look for these on enabled add-ons;
# they act only during this add-on's own export and import.
glTF2ExportUserExtension = exporting.glTF2ExportUserExtension
glTF2ImportUserExtension = importing.glTF2ImportUserExtension

_modules = (preferences, properties, preview, operators, exporting, importing, overlays, ui)


def register():
    for module in _modules:
        module.register()


def unregister():
    for module in reversed(_modules):
        module.unregister()
