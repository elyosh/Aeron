#include "aeron/asset/opt_sim_model.h"

#include "opt_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool image_error(char* error, size_t error_size, const char* message, const char* detail) {
	if (error && error_size)
		snprintf(error, error_size, "%s%s%s", message, detail ? ": " : "", detail ? detail : "");
	return false;
}

/* The 8x8 placeholder: every texel index 0, every palette entry neutral grey
 * (RGB565). Debris and texture-slot code finds a texture under each mesh. */
static opt_wnode_t placeholder_texture(opt_writer_t* writer) {
	static const uint8_t texels[64] = { 0 };
	uint16_t*            palette    = malloc(16u * 256u * sizeof *palette);
	if (!palette)
		return 0;
	for (uint32_t entry = 0; entry < 16u * 256u; ++entry)
		palette[entry] = 0x8410u;
	const opt_wnode_t texture = opt_w_texture(writer, AERON_OPT_PLACEHOLDER_TEXTURE_NAME, 8, 8, texels,
											  (int)sizeof texels, palette, AERON_OPT_PLACEHOLDER_TEXTURE_ID);
	free(palette);
	return texture;
}

static opt_wnode_t mesh_root(opt_writer_t* writer, const AeronOptSimMesh* mesh, bool v5, opt_wnode_t texture,
							 opt_wnode_t* stub) {
	static const float origin_uv[1][2] = { { 0.0f, 0.0f } };
	static const float always[1]       = { 0.0f };
	/* Stub FaceData: no faces in the image. Faces live in the simulation
	 * model's side data; any FaceData reader added to a game later sees an
	 * empty mesh for replacement models and must read the side data. */
	*stub                         = opt_w_face_data_stub(writer);
	const opt_wnode_t lod_items[] = { texture, *stub };
	const opt_wnode_t lod0        = texture ? opt_w_group(writer, lod_items, 2) : opt_w_group(writer, stub, 1);
	const opt_wnode_t grouping    = opt_w_face_grouping(writer, always, &lod0, 1);
	const opt_wnode_t faces_group = opt_w_group(writer, (const opt_wnode_t[]) { 0, 0, 0, grouping }, 4);

	const uint32_t glow_count = v5 ? mesh->glow_count : 0;
	const uint32_t count      = 5 + mesh->hardpoint_count + glow_count + 2;
	opt_wnode_t*   children   = malloc((size_t)count * sizeof *children);
	if (!children)
		return 0;
	uint32_t child    = 0;
	children[child++] = opt_w_vertices(writer, (const float (*)[3])mesh->vertices, (int)mesh->vertex_count + 2);
	children[child++] = opt_w_texcoords(writer, origin_uv, 1);
	children[child++] = opt_w_vertex_normals(writer, (const float (*)[3])mesh->normals, (int)mesh->vertex_count);
	children[child++] = opt_w_descriptor(writer, &mesh->descriptor);
	for (uint32_t index = 0; index < mesh->hardpoint_count; ++index)
		children[child++] = opt_w_hardpoint(writer, &mesh->hardpoints[index]);
	for (uint32_t index = 0; index < glow_count; ++index)
		children[child++] = opt_w_engine_glow(writer, &mesh->glows[index]);
	children[child++]      = opt_w_rot_scale(writer, &mesh->rot_scale);
	children[child++]      = faces_group;
	const opt_wnode_t root = opt_w_group(writer, children, (int)child);
	free(children);
	return root;
}

bool Aeron_OptSimModelWriteImage(const AeronOptSimModel* model, const AeronOptImageOptions* options,
								 uint8_t** payload, int* payload_size, uint32_t* stub_face_data_offsets,
								 char* error, size_t error_size) {
	if (!model || !options || !payload || !payload_size || !stub_face_data_offsets || !model->mesh_count)
		return image_error(error, error_size, "invalid OPT image arguments", NULL);
	for (uint32_t index = 0; index < model->mesh_count; ++index)
		if (!model->meshes[index].vertices || !model->meshes[index].normals)
			return image_error(error, error_size, "OPT simulation model was trimmed before writing its image", NULL);
	const bool    v5     = options->profile == AERON_OPT_IMAGE_PROFILE_V5;
	opt_writer_t* writer = opt_writer_create(v5 ? OPT_WRITE_PROFILE_V5 : OPT_WRITE_PROFILE_V2);
	opt_wnode_t*  roots  = malloc((size_t)model->mesh_count * sizeof *roots);
	opt_wnode_t*  stubs  = malloc((size_t)model->mesh_count * sizeof *stubs);
	bool          ok     = false;
	if (!writer || !roots || !stubs) {
		image_error(error, error_size, "out of memory writing OPT image", NULL);
		goto done;
	}
	for (uint32_t index = 0; index < model->mesh_count; ++index) {
		opt_wnode_t texture = 0;
		if (options->placeholder_texture)
			texture = index == 0 ? placeholder_texture(writer) : opt_w_node_ref(writer, AERON_OPT_PLACEHOLDER_TEXTURE_NAME);
		roots[index] = mesh_root(writer, &model->meshes[index], v5, texture, &stubs[index]);
	}
	char detail[160] = { 0 };
	if (!opt_writer_finish(writer, roots, (int)model->mesh_count, payload, payload_size, detail, sizeof detail)) {
		image_error(error, error_size, "OPT image serialization failed", detail);
		goto done;
	}
	for (uint32_t index = 0; index < model->mesh_count; ++index)
		stub_face_data_offsets[index] = opt_writer_payload_offset(writer, stubs[index]);
	ok = true;
done:
	free(roots);
	free(stubs);
	opt_writer_free(writer);
	return ok;
}
