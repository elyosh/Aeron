/*
 * Environment debug tool: the library's prepared set, the active package,
 * and live overrides of exposure, strengths, ambient floor, rotation and
 * lights. Built only with AERON_DEBUG_UI. The overlay is built in
 * Aeron_Present, after the game recorded its frame, so edits apply between
 * frames.
 */

#include "environment_internal.h"

#include "aeron/debug.h"

#include <imgui.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

AeronEnvironmentLibrary*    g_library;
AeronEnvironmentDebugRowsFn g_rows;
void*                       g_rows_user;
bool                        g_registered;

const EnvTexture* find_texture(const AeronEnvironmentLibrary* library, const char* path) {
	for (uint32_t i = 0; i < library->texture_count; ++i)
		if (strcmp(library->textures[i].path, path) == 0)
			return &library->textures[i];
	return nullptr;
}

void texture_row(const AeronEnvironmentLibrary* library, const char* label, const char* path) {
	if (!path[0])
		return;
	const EnvTexture* e = find_texture(library, path);
	if (e && e->texture)
		ImGui::Text("%s: %s (%d px, %d levels, %.1f MiB)", label, path, e->size, e->level_count,
					(double)e->bytes / (1024.0 * 1024.0));
	else
		ImGui::Text("%s: %s (%s)", label, path, e && e->failure[0] ? e->failure : "not loaded");
}

void copy_yaml(const EnvSlot* slot) {
	AeronEnvironmentDefinition* d =
		static_cast<AeronEnvironmentDefinition*>(malloc(sizeof(AeronEnvironmentDefinition)));
	if (!d)
		return;
	*d = slot->package.settings;
	/* A rotation that came from the reference belongs in the game's mapping. */
	if (slot->reference.has_rotation)
		memcpy(d->rotation_degrees, slot->package.definition->rotation_degrees, sizeof d->rotation_degrees);
	const size_t length = AeronEnvironment_FormatPackageYaml(d, nullptr, 0);
	char*        text   = static_cast<char*>(malloc(length + 256));
	if (text) {
		AeronEnvironment_FormatPackageYaml(d, text, length + 1);
		if (slot->reference.has_rotation) {
			const float* r = slot->package.settings.rotation_degrees;
			snprintf(text + length, 256, "# reference\npackage: %s\nrotation_degrees: [%.9g, %.9g, %.9g]\n",
					 slot->reference.package, (double)r[0], (double)r[1], (double)r[2]);
		}
		ImGui::SetClipboardText(text);
		free(text);
	}
	free(d);
}

bool light_editor(EnvSlot* slot, uint32_t index) {
	AeronEnvironmentDefinition*      s       = &slot->package.settings;
	AeronEnvironmentLightDefinition* d       = &s->lights[index];
	const AeronEnvironmentLight*     l       = &slot->package.lights[index];
	bool                             changed = false;
	ImGui::PushID(static_cast<int>(index));
	if (ImGui::TreeNodeEx("light", ImGuiTreeNodeFlags_DefaultOpen, "%s%s", d->name,
						  d->primary ? " (primary)" : "")) {
		changed |= ImGui::DragFloat3("Direction (cube)", d->direction_cube, 0.005f, -1.0f, 1.0f, "%.3f");
		ImGui::Text("Direction (world): %.3f %.3f %.3f", (double)l->direction_world[0],
					(double)l->direction_world[1], (double)l->direction_world[2]);
		changed |= ImGui::ColorEdit3("Colour (linear)", d->color_linear,
									 ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		changed |= ImGui::SliderFloat("Intensity", &d->intensity, 0.0f, 16.0f, "%.3f");
		changed |= ImGui::SliderFloat("Angular radius", &d->angular_radius_degrees, 0.01f, 5.0f, "%.3f deg");
		if (ImGui::Checkbox("Primary", &d->primary)) {
			changed = true;
			for (uint32_t i = 0; i < s->light_count; ++i)
				if (i != index && d->primary)
					s->lights[i].primary = false;
			if (!d->primary)
				d->casts_shadows = false;
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!d->primary);
		changed |= ImGui::Checkbox("Casts shadows", &d->casts_shadows);
		ImGui::EndDisabled();
		ImGui::SameLine();
		changed |= ImGui::Checkbox("Lens flare", &d->lens_flare);
		if (d->visual == AERON_ENV_SUN_DISC) {
			ImGui::TextUnformatted("Visual: disc");
			changed |= ImGui::SliderFloat("Radiance scale", &d->radiance_scale, 0.0f, 16.0f, "%.3f");
			changed |= ImGui::SliderFloat("Glow radius", &d->glow_radius_degrees, 0.0f, 30.0f, "%.2f deg");
			changed |= ImGui::SliderFloat("Glow strength", &d->glow_strength, 0.0f, 16.0f, "%.3f");
		} else {
			ImGui::Text("Visual: sprite %s%s", d->sprite_path, l->sprite ? "" : " (not loaded)");
			changed |= ImGui::SliderFloat("Angular size", &d->angular_size_degrees, 0.01f, 60.0f, "%.2f deg");
			changed |= ImGui::SliderFloat("Sprite strength", &d->sprite_strength, 0.0f, 16.0f, "%.3f");
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
	return changed;
}

void slot_editor(AeronEnvironmentLibrary* library, EnvSlot* slot) {
	AeronEnvironmentDefinition* s       = &slot->package.settings;
	bool                        changed = false;
	if (s->kind == AERON_ENV_KIND_FULL) {
		changed |= ImGui::SliderFloat("Sky exposure", &s->sky_exposure, 0.0f, 16.0f, "%.3f");
		changed |= ImGui::SliderFloat("Diffuse strength", &s->diffuse_strength, 0.0f, 4.0f, "%.3f");
		changed |= ImGui::SliderFloat("Specular strength", &s->specular_strength, 0.0f, 4.0f, "%.3f");
	} else {
		changed |= ImGui::SliderFloat("Diffuse strength", &s->diffuse_strength, 0.0f, 4.0f, "%.3f");
	}
	changed |= ImGui::SliderFloat("Ambient floor scale", &s->ambient_floor_scale, 0.0f, 1.0f, "%.3f");
	changed |= ImGui::SliderFloat("Yaw", &s->rotation_degrees[0], 0.0f, 360.0f, "%.1f deg");
	changed |= ImGui::SliderFloat("Pitch", &s->rotation_degrees[1], 0.0f, 360.0f, "%.1f deg");
	changed |= ImGui::SliderFloat("Roll", &s->rotation_degrees[2], 0.0f, 360.0f, "%.1f deg");
	for (uint32_t i = 0; i < s->light_count; ++i)
		changed |= light_editor(slot, i);
	if (changed) {
		slot->overridden = true;
		env_library_apply(library, slot);
	}
	ImGui::BeginDisabled(!slot->overridden);
	if (ImGui::Button("Reset"))
		env_library_reset(library, slot);
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Copy YAML"))
		copy_yaml(slot);
}

void draw_tool(int* open, void* user) {
	(void)user;
	ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver);
	bool visible = *open != 0;
	if (!ImGui::Begin("Environment", &visible)) {
		ImGui::End();
		*open = visible ? 1 : 0;
		return;
	}
	AeronEnvironmentLibrary* library = g_library;
	if (g_rows)
		g_rows(g_rows_user);
	if (!library) {
		ImGui::TextDisabled("No environment library.");
		ImGui::End();
		*open = visible ? 1 : 0;
		return;
	}
	ImGui::Separator();
	uint64_t bytes = 0;
	for (uint32_t i = 0; i < library->texture_count; ++i)
		bytes += library->textures[i].texture ? library->textures[i].bytes : 0;
	ImGui::Text("Prepared: %u references, %u textures, %.1f MiB", library->slot_count, library->texture_count,
				(double)bytes / (1024.0 * 1024.0));
	ImGui::BeginDisabled(library->reload_requested);
	if (ImGui::Button(library->reload_requested ? "Reload pending" : "Reload"))
		AeronEnvironmentLibrary_RequestReload(library);
	ImGui::EndDisabled();

	for (uint32_t i = 0; i < library->slot_count; ++i) {
		EnvSlot*   slot   = &library->slots[i];
		const bool active = library->active == static_cast<int32_t>(i);
		char       header[160];
		snprintf(header, sizeof header, "%u: %s%s%s%s###slot%u", i, slot->reference.package,
				 slot->reference.has_rotation ? " (rotated)" : "", active ? " [active]" : "",
				 !slot->available ? " [unavailable]" : (slot->overridden ? " [edited]" : ""), i);
		if (!ImGui::CollapsingHeader(header, active ? ImGuiTreeNodeFlags_DefaultOpen : 0))
			continue;
		ImGui::PushID(static_cast<int>(i));
		if (!slot->available) {
			ImGui::TextWrapped("Unavailable: %s", slot->failure);
			ImGui::PopID();
			continue;
		}
		const AeronEnvironmentDefinition* d = slot->package.definition;
		ImGui::Text("Kind: %s", d->kind == AERON_ENV_KIND_FULL ? "full" : "lighting");
		texture_row(library, "Radiance", d->radiance_path);
		texture_row(library, "Diffuse", d->diffuse_path);
		texture_row(library, "Specular", d->specular_path);
		if (slot->reference.has_rotation)
			ImGui::Text("Reference rotation: %.1f %.1f %.1f", (double)slot->reference.rotation_degrees[0],
						(double)slot->reference.rotation_degrees[1],
						(double)slot->reference.rotation_degrees[2]);
		slot_editor(library, slot);
		ImGui::PopID();
	}
	ImGui::End();
	*open = visible ? 1 : 0;
}

} // namespace

extern "C" void env_debug_detach(AeronEnvironmentLibrary* library) {
	if (g_library == library) {
		g_library   = nullptr;
		g_rows      = nullptr;
		g_rows_user = nullptr;
	}
}

extern "C" void AeronEnvironmentLibrary_RegisterDebugTool(AeronEnvironmentLibrary*    library,
														  AeronEnvironmentDebugRowsFn rows, void* user) {
	g_library   = library;
	g_rows      = rows;
	g_rows_user = user;
	if (!g_registered && Aeron_DebugUiAvailable()) {
		Aeron_DebugRegisterTool("Environment", draw_tool, nullptr);
		g_registered = true;
	}
}
