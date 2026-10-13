/* Manages the arrays, strings, buffer and images of the document
 * (opt_gltf_internal.h), and implements the document functions of
 * opt_gltf.h. */

#include "opt_gltf_internal.h"
#include "opt_json.h"

void* opt_gltf_push_array(void** arr, size_t* count, size_t* cap, size_t elem_size, size_t want) {
	if (*count + want > *cap) {
		/* Reaching this branch means the pre-counting pass missed an
		 * entry. cgltf records pointers (not indices) into these arrays
		 * and writes them by pointer arithmetic at serialization time,
		 * so a realloc here would dangle every pointer captured before
		 * this call. Abort instead of silently corrupting the output. */
		abort();
	}
	void* slot = (char*)*arr + *count * elem_size;
	*count += want;
	return slot;
}

/* owned_strings can be reallocated safely. cgltf keeps no pointers into
 * this array, only pointers to the strings, which are allocated separately
 * and never move. */
char* opt_gltf_keep_string(GltfBuild* gb, char* s) {
	if (!s) {
		gb->out_of_memory = true;
		return NULL;
	}
	if (gb->owned_strings_count + 1 > gb->owned_strings_cap) {
		size_t ncap  = gb->owned_strings_cap ? gb->owned_strings_cap * 2 : 64;
		char** grown = (char**)realloc(gb->owned_strings, ncap * sizeof(char*));
		if (!grown) {
			free(s);
			gb->out_of_memory = true;
			return NULL;
		}
		gb->owned_strings     = grown;
		gb->owned_strings_cap = ncap;
	}
	gb->owned_strings[gb->owned_strings_count++] = s;
	return s;
}

/* Prints `object` as the AERON_flight_model extension of the node, and
 * deletes `object`. */
bool opt_gltf_set_extension(GltfBuild* build, cgltf_node* node, cJSON* object) {
	cgltf_extension* extension = (cgltf_extension*)calloc(1, sizeof *extension);
	char*            data      = OptJson_Print(object);
	if (!extension || !data) {
		free(extension);
		free(data);
		return false;
	}
	extension->name        = (char*)"AERON_flight_model";
	extension->data        = opt_gltf_keep_string(build, data);
	node->extensions       = extension;
	node->extensions_count = 1;
	return true;
}

/* Appends bytes to the .bin buffer at a 4-byte boundary, as the glTF spec
 * requires for accessors. Returns the byte offset of the data. */
size_t opt_gltf_bin_append(GltfBuild* gb, const void* data, size_t bytes) {
	if (gb->bin_size + bytes + 4 > gb->bin_cap) {
		size_t ncap = gb->bin_cap ? gb->bin_cap * 2 : 4096;
		while (ncap < gb->bin_size + bytes + 4)
			ncap *= 2;
		uint8_t* grown = (uint8_t*)realloc(gb->bin, ncap);
		if (!grown) {
			gb->out_of_memory = true;
			return 0;
		}
		gb->bin     = grown;
		gb->bin_cap = ncap;
	}
	/* Align to 4 bytes, which covers both float and uint16 components. */
	size_t off = (gb->bin_size + 3) & ~(size_t)3;
	while (gb->bin_size < off)
		gb->bin[gb->bin_size++] = 0;
	memcpy(gb->bin + gb->bin_size, data, bytes);
	gb->bin_size += bytes;
	return off;
}

cgltf_buffer_view* opt_gltf_new_view(GltfBuild* gb, size_t offset, size_t bytes,
									 cgltf_buffer_view_type type) {
	cgltf_buffer_view* v = (cgltf_buffer_view*)opt_gltf_push_array((void**)&gb->views, &gb->views_count,
																   &gb->views_cap, sizeof *v, 1);
	v->buffer            = &gb->buffer;
	v->offset            = offset;
	v->size              = bytes;
	v->type              = type;
	return v;
}

cgltf_accessor* opt_gltf_new_accessor(GltfBuild* gb, cgltf_buffer_view* view, cgltf_size count,
									  cgltf_type type, cgltf_component_type comp) {
	cgltf_accessor* a = (cgltf_accessor*)opt_gltf_push_array((void**)&gb->accessors, &gb->accessors_count,
															 &gb->accessors_cap, sizeof *a, 1);
	a->buffer_view    = view;
	a->offset         = 0;
	a->count          = count;
	a->type           = type;
	a->component_type = comp;
	/* cgltf sets the stride when it parses a file. This graph is built
	 * directly, so the stride is set here for accessor reads. */
	a->stride = cgltf_calc_size(type, comp);
	return a;
}

/* Registers an RGBA image as image and texture `slot`, written to
 * textures/<file>. Takes ownership of `rgba`, `name` and `file`. */
cgltf_texture* opt_gltf_set_image(OptGltfDocument* document, size_t slot, uint8_t* rgba, uint32_t width,
								  uint32_t height, char* name, char* file) {
	GltfBuild* build                    = &document->gb;
	document->image_pixels[slot].rgba   = rgba;
	document->image_pixels[slot].width  = width;
	document->image_pixels[slot].height = height;
	build->images[slot].name            = opt_gltf_keep_string(build, name);
	build->images[slot].uri             = opt_gltf_keep_string(build, xprintf_dup("textures/%s", file));
	free(file);
	build->images[slot].mime_type = "image/png";
	build->textures[slot].image   = &build->images[slot];
	build->textures[slot].sampler = &build->sampler;
	build->textures[slot].name    = build->images[slot].name;
	return &build->textures[slot];
}

/* material_views lists the kViewCount texture views of material `m`. */
enum { kViewCount = 21 };

static void material_views(cgltf_material* m, cgltf_texture_view* views[kViewCount]) {
	cgltf_texture_view* const all[kViewCount] = {
		&m->pbr_metallic_roughness.base_color_texture,
		&m->pbr_metallic_roughness.metallic_roughness_texture,
		&m->pbr_specular_glossiness.diffuse_texture,
		&m->pbr_specular_glossiness.specular_glossiness_texture,
		&m->clearcoat.clearcoat_texture,
		&m->clearcoat.clearcoat_roughness_texture,
		&m->clearcoat.clearcoat_normal_texture,
		&m->transmission.transmission_texture,
		&m->specular.specular_texture,
		&m->specular.specular_color_texture,
		&m->volume.thickness_texture,
		&m->sheen.sheen_color_texture,
		&m->sheen.sheen_roughness_texture,
		&m->iridescence.iridescence_texture,
		&m->iridescence.iridescence_thickness_texture,
		&m->diffuse_transmission.diffuse_transmission_texture,
		&m->diffuse_transmission.diffuse_transmission_color_texture,
		&m->anisotropy.anisotropy_texture,
		&m->normal_texture,
		&m->occlusion_texture,
		&m->emissive_texture,
	};
	memcpy(views, all, sizeof all);
}

int opt_gltf_texture_of_pixels(const OptGltfDocument* document, const uint8_t* rgba) {
	const GltfBuild* gb = &document->gb;
	for (size_t t = 0; rgba && t < gb->textures_count; ++t) {
		const cgltf_image* image = gb->textures[t].image;
		if (image && (size_t)(image - gb->images) < document->image_pixels_count &&
			document->image_pixels[image - gb->images].rgba == rgba)
			return (int)t;
	}
	return -1;
}

/* Removes the materials that no primitive draws, directly or through a
 * variant. These are the materials used only by the lower LODs of the OPT,
 * since the conversion keeps only LOD 0. Then removes the textures and
 * images that only those materials used. cgltf references them by pointer,
 * so the arrays are compacted and the pointers are updated. */
bool opt_gltf_prune_unused(OptGltfDocument* document) {
	GltfBuild*   gb = &document->gb;
	const size_t largest =
		gb->materials_count > gb->textures_count ? gb->materials_count : gb->textures_count;
	const size_t count = largest > gb->images_count ? largest : gb->images_count;
	size_t*      to    = (size_t*)malloc((count ? count : 1) * sizeof *to);
	if (!to)
		return false;

	/* Materials. */
	for (size_t m = 0; m < gb->materials_count; ++m)
		to[m] = SIZE_MAX;
	for (size_t p = 0; p < gb->primitives_count; ++p) {
		const cgltf_primitive* prim = &gb->primitives[p];
		if (prim->material)
			to[prim->material - gb->materials] = 0;
		for (size_t k = 0; k < prim->mappings_count; ++k)
			if (prim->mappings[k].material)
				to[prim->mappings[k].material - gb->materials] = 0;
	}
	size_t kept = 0;
	for (size_t m = 0; m < gb->materials_count; ++m) {
		if (to[m] == SIZE_MAX)
			continue;
		gb->materials[kept]       = gb->materials[m];
		gb->material_anim[kept]   = gb->material_anim[m];
		gb->material_screen[kept] = gb->material_screen[m];
		if (gb->material_given)
			gb->material_given[kept] = gb->material_given[m];
		to[m] = kept++;
	}
	for (size_t p = 0; p < gb->primitives_count; ++p) {
		cgltf_primitive* prim = &gb->primitives[p];
		if (prim->material)
			prim->material = &gb->materials[to[prim->material - gb->materials]];
		for (size_t k = 0; k < prim->mappings_count; ++k)
			if (prim->mappings[k].material)
				prim->mappings[k].material = &gb->materials[to[prim->mappings[k].material - gb->materials]];
	}
	gb->materials_count = kept;

	/* Textures. */
	cgltf_texture_view* views[kViewCount];
	for (size_t t = 0; t < gb->textures_count; ++t)
		to[t] = SIZE_MAX;
	for (size_t m = 0; m < gb->materials_count; ++m) {
		material_views(&gb->materials[m], views);
		for (int v = 0; v < kViewCount; ++v)
			if (views[v]->texture)
				to[views[v]->texture - gb->textures] = 0;
		/* The textures of the material's skin layers. */
		const OptGltfMaterial* given = gb->material_given ? gb->material_given[m] : NULL;
		for (size_t l = 0; given && l < given->skin_count; ++l) {
			const OptGltfImage* skin[3] = { &given->skins[l].base, &given->skins[l].emissive,
											&given->skins[l].metallic_roughness };
			for (int k = 0; k < 3; ++k) {
				const int t = opt_gltf_texture_of_pixels(document, skin[k]->view.rgba);
				if (t >= 0)
					to[t] = 0;
			}
		}
	}
	kept = 0;
	for (size_t t = 0; t < gb->textures_count; ++t) {
		if (to[t] == SIZE_MAX)
			continue;
		gb->textures[kept] = gb->textures[t];
		to[t]              = kept++;
	}
	for (size_t m = 0; m < gb->materials_count; ++m) {
		material_views(&gb->materials[m], views);
		for (int v = 0; v < kViewCount; ++v)
			if (views[v]->texture)
				views[v]->texture = &gb->textures[to[views[v]->texture - gb->textures]];
	}
	gb->textures_count = kept;

	/* Images, with their pixels. */
	for (size_t i = 0; i < gb->images_count; ++i)
		to[i] = SIZE_MAX;
	for (size_t t = 0; t < gb->textures_count; ++t)
		if (gb->textures[t].image)
			to[gb->textures[t].image - gb->images] = 0;
	kept = 0;
	for (size_t i = 0; i < gb->images_count; ++i) {
		if (to[i] == SIZE_MAX) {
			if (!document->image_pixels[i].borrowed)
				free(document->image_pixels[i].rgba);
			continue;
		}
		gb->images[kept]             = gb->images[i];
		document->image_pixels[kept] = document->image_pixels[i];
		to[i]                        = kept++;
	}
	for (size_t i = gb->images_count; i < document->image_pixels_count; ++i)
		if (!document->image_pixels[i].borrowed)
			free(document->image_pixels[i].rgba);
	for (size_t t = 0; t < gb->textures_count; ++t)
		if (gb->textures[t].image)
			gb->textures[t].image = &gb->images[to[gb->textures[t].image - gb->images]];
	gb->images_count             = kept;
	document->image_pixels_count = kept;
	free(to);
	return true;
}

cgltf_data* OptGltf_Data(OptGltfDocument* document) { return document ? &document->data : NULL; }

bool OptGltf_ImageView(const OptGltfDocument* document, const cgltf_image* image,
					   OptGltfImageView* out_view) {
	if (!document || !image || !out_view || !document->gb.images)
		return false;
	const ptrdiff_t index = image - document->gb.images;
	if (index < 0 || (size_t)index >= document->image_pixels_count)
		return false;
	const OptGltfOwnedImage* owned = &document->image_pixels[index];
	if (!owned->rgba || owned->width == 0 || owned->height == 0)
		return false;
	out_view->rgba   = owned->rgba;
	out_view->width  = owned->width;
	out_view->height = owned->height;
	return true;
}

void OptGltf_Free(OptGltfDocument* document) {
	if (!document)
		return;
	GltfBuild* gb = &document->gb;

	/* The loop below frees the children of the root (gb.nodes[0].children)
	 * and the children of each mesh node, since both are stored in
	 * node->children. Freeing root_children as well would free it twice. */
	for (size_t i = 0; i < gb->nodes_count; ++i) {
		free(gb->nodes[i].children);
		free(gb->nodes[i].extensions);
	}
	free(document->scene_roots);
	for (size_t i = 0; i < document->image_pixels_count; ++i)
		if (!document->image_pixels[i].borrowed)
			free(document->image_pixels[i].rgba);
	free(document->image_pixels);
	free(gb->images);
	free(gb->textures);
	for (size_t i = 0; i < gb->materials_count; ++i)
		free(gb->materials[i].extensions);
	free(gb->materials);
	free(gb->material_anim);
	free(gb->material_screen);
	free(gb->material_given);
	free(gb->meshes);
	free(gb->primitives);
	free(gb->attributes);
	free(gb->mappings);
	free(gb->variants);
	free(gb->views);
	free(gb->accessors);
	free(gb->nodes);
	free(gb->lights);
	free(gb->buffer.data ? gb->buffer.data : gb->bin);
	free(document->data.extensions_used);

	for (size_t i = 0; i < gb->owned_strings_count; ++i)
		free(gb->owned_strings[i]);
	free(gb->owned_strings);
	free(document);
}
