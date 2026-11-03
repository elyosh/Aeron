#include "project_build.h"

#include "bc7_codec.h"
#include "cook_model.h"
#include "effect_library.h"
#include "ktx2_writer.h"
#include "material_checks.h"
#include "project_files.h"
#include "tool_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROJECT_TAG "[aeron_gltf_cook] project"
#define PROJECT_PATH_MAX 1200

/* A model of the project to cook. */
typedef struct ProjectModel {
	char gltf[PROJECT_PATH_MAX];
	char glb[PROJECT_PATH_MAX];
	char probes[PROJECT_PATH_MAX]; /* "" without a probes bake */
	bool cooked;
} ProjectModel;

typedef struct ProjectCook {
	ProjectModel*               models;
	const AeronGltfCookOptions* options;
} ProjectCook;

/* Formats a path into `out`, which holds PROJECT_PATH_MAX bytes. Returns
 * false when it is too long. */
static bool format_path(char* out, const char* format, ...) {
	va_list args;
	va_start(args, format);
	const int written = vsnprintf(out, PROJECT_PATH_MAX, format, args);
	va_end(args);
	return written > 0 && written < PROJECT_PATH_MAX;
}

/* Builds every library of <project>/effects into `effects_out`, and copies
 * effects.yaml. Returns the number of failures. */
static int build_effect_libraries(const char* project, const char* effects_out, int* built) {
	char   sources[PROJECT_PATH_MAX], library[PROJECT_PATH_MAX], target[PROJECT_PATH_MAX];
	char** names    = NULL;
	int    failures = 0;
	if (!format_path(sources, "%s/effects", project) || !ProjectFiles_IsFolder(sources))
		return 0;
	const int count = ProjectFiles_ListFolders(sources, &names);
	if (count < 0) {
		ToolLog_Write(TOOL_LOG_ERROR, "%s: %s can't be read", PROJECT_TAG, sources);
		return 1;
	}
	for (int i = 0; i < count; ++i) {
		if (!format_path(library, "%s/%s/%s.yaml", sources, names[i], names[i]) ||
			!ProjectFiles_IsFile(library))
			continue;
		if (EffectLibrary_BuildLibrary(PROJECT_TAG, library, NULL, effects_out) == EFFECT_LIBRARY_FAILED)
			++failures;
		else
			++*built;
	}
	ProjectFiles_FreeNames(names, count);
	if (format_path(library, "%s/effects.yaml", sources) && ProjectFiles_IsFile(library) &&
		(!format_path(target, "%s/effects.yaml", effects_out) || !ProjectFiles_CopyFile(library, target))) {
		ToolLog_Write(TOOL_LOG_ERROR, "%s: %s can't be copied to %s", PROJECT_TAG, library, effects_out);
		++failures;
	}
	return failures;
}

/* Whether `dir` holds the probes bake that the add-on writes. */
static bool has_probes_bake(const char* dir) {
	static const char* const files[] = { "bake.json", "probes.ktx2", "grid.ktx2" };
	char                     path[PROJECT_PATH_MAX];
	for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i)
		if (!format_path(path, "%s/%s", dir, files[i]) || !ProjectFiles_IsFile(path))
			return false;
	return true;
}

/* Lists the models of <project>/models that have their .gltf, or only `only`.
 * Returns NULL when the folder can't be read or memory runs out. */
static ProjectModel* list_models(const ProjectBuildOptions* o, const char* models_out, int* count) {
	char   models_dir[PROJECT_PATH_MAX];
	char** names = NULL;
	*count       = 0;
	if (!format_path(models_dir, "%s/models", o->project))
		return NULL;
	if (!ProjectFiles_IsFolder(models_dir))
		return (ProjectModel*)calloc(1, sizeof(ProjectModel));
	const int listed = ProjectFiles_ListFolders(models_dir, &names);
	if (listed < 0) {
		ToolLog_Write(TOOL_LOG_ERROR, "%s: %s can't be read", PROJECT_TAG, models_dir);
		return NULL;
	}
	ProjectModel* models = (ProjectModel*)calloc(listed > 0 ? (size_t)listed : 1, sizeof *models);
	for (int i = 0; models && i < listed; ++i) {
		ProjectModel* model = &models[*count];
		char          dir[PROJECT_PATH_MAX];
		if ((o->only && strcmp(names[i], o->only) != 0) || !format_path(dir, "%s/%s", models_dir, names[i]) ||
			!format_path(model->gltf, "%s/%s.gltf", dir, names[i]) || !ProjectFiles_IsFile(model->gltf) ||
			!format_path(model->glb, "%s/%s.glb", models_out, names[i]) ||
			!format_path(model->probes, "%s/bake/probes", dir))
			continue;
		if (!has_probes_bake(model->probes))
			model->probes[0] = '\0';
		++*count;
	}
	ProjectFiles_FreeNames(names, listed);
	return models;
}

static void cook_one_model(void* context, int index) {
	ProjectCook*  cook  = (ProjectCook*)context;
	ProjectModel* model = &cook->models[index];
	model->cooked       = aeron_gltf_cook_model(model->gltf, model->glb, cook->options,
												model->probes[0] ? model->probes : NULL);
}

bool ProjectBuild_Run(const ProjectBuildOptions* o) {
	char effects_out[PROJECT_PATH_MAX], models_out[PROJECT_PATH_MAX];
	if (!ProjectFiles_IsFolder(o->project)) {
		ToolLog_Write(TOOL_LOG_ERROR, "%s: project folder not found: %s", PROJECT_TAG, o->project);
		return false;
	}
	if (!format_path(effects_out, "%s/flight/effects", o->mod) ||
		!format_path(models_out, "%s/flight/models", o->mod) || !ProjectFiles_MakeFolders(effects_out) ||
		!ProjectFiles_MakeFolders(models_out)) {
		ToolLog_Write(TOOL_LOG_ERROR, "%s: the folders of %s can't be created", PROJECT_TAG, o->mod);
		return false;
	}

	/* The libraries are always built, because the cook checks the materials
	   against them. A library whose inputs haven't changed is left as it is. */
	int libraries = 0;
	int failures  = build_effect_libraries(o->project, effects_out, &libraries);

	int           count  = 0;
	ProjectModel* models = list_models(o, models_out, &count);
	if (!models)
		return false;
	if (o->only && count == 0) {
		ToolLog_Write(TOOL_LOG_ERROR, "%s: the project has no model %s with models/%s/%s.gltf", PROJECT_TAG,
					  o->only, o->only, o->only);
		free(models);
		return false;
	}
	/* The checks run one at a time, then the models that pass are cooked. */
	int checked = 0;
	for (int i = 0; i < count; ++i) {
		if (aeron_gltf_cook_check_materials(models[i].gltf, effects_out, NULL))
			models[checked++] = models[i];
		else
			++failures;
	}
	const int jobs    = o->jobs < 1 ? 1 : o->jobs;
	const int threads = ProjectFiles_HardwareThreads() / jobs;
	bc7_codec_set_max_threads((unsigned)(threads > 1 ? threads : 1));
	ProjectCook cook = { models, &o->cook };
	ProjectFiles_RunTasks(checked, jobs, cook_one_model, &cook);
	bc7_codec_set_max_threads(0);

	int cooked = 0;
	for (int i = 0; i < checked; ++i) {
		cooked += models[i].cooked;
		failures += !models[i].cooked;
	}
	free(models);
	ToolLog_Write(TOOL_LOG_INFO, "%s: %d effect libraries, %d of %d models cooked into %s", PROJECT_TAG,
				  libraries, cooked, count, o->mod);
	return failures == 0;
}
