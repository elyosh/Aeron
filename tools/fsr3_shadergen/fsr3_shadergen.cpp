#include <SDL3/SDL_process.h>
#include <yaml.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Define {
	std::string name;
	std::string value;
};

enum class BindingClass {
	SampledTexture,
	StorageTexture,
	StorageBuffer,
	DeclarationSrvTexture,
	RwTexture,
	RwBuffer,
	Constant,
};

struct Binding {
	BindingClass klass;
	std::string  macro;
	std::string  name;
	std::string  when;
	std::string  sampler;
};

struct Profile {
	std::string id;
	bool        enabled = false;
	std::string precision;
	std::string spd;
	std::string reprojection;
	std::string atomicLayout;
	std::string unavailableReason;
};

struct Pass {
	std::string              id;
	std::string              ffxPass;
	bool                     directHistoryVariant = false;
	std::array<uint32_t, 3>  threads {};
	std::set<std::string>    dimensions;
	std::vector<Define>      defines;
	std::vector<Binding>     bindings;
	std::vector<std::string> includes;
	std::string              rootSignature;
	std::string              signature;
	std::string              body;
};

struct Manifest {
	uint32_t             schema = 0;
	std::string          effectName;
	std::string          effectVersion;
	std::vector<Define>  commonDefines;
	std::vector<Profile> profiles;
	std::vector<Pass>    passes;
};

struct Variant {
	const Profile*       profile       = nullptr;
	const Pass*          pass          = nullptr;
	bool                 directHistory = false;
	std::string          key;
	std::string          shaderName;
	std::vector<Binding> sampledTextures;
	std::vector<Binding> storageTextures;
	std::vector<Binding> storageBuffers;
	std::vector<Binding> declarationSrvTextures;
	std::vector<Binding> rwTextures;
	std::vector<Binding> rwBuffers;
	std::vector<Binding> constants;
};

static const std::map<std::string, std::string> kRequiredPasses = {
	{ "prepare_inputs", "FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS" },
	{ "luma_pyramid", "FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID" },
	{ "shading_change_pyramid", "FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID" },
	{ "shading_change", "FFX_FSR3UPSCALER_PASS_SHADING_CHANGE" },
	{ "prepare_reactivity", "FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY" },
	{ "luma_instability", "FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY" },
	{ "accumulate", "FFX_FSR3UPSCALER_PASS_ACCUMULATE" },
	{ "accumulate_sharpen", "FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN" },
	{ "rcas", "FFX_FSR3UPSCALER_PASS_RCAS" },
	{ "debug_view", "FFX_FSR3UPSCALER_PASS_DEBUG_VIEW" },
	{ "generate_reactive", "FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE" },
};

static const std::set<std::string> kKnownBindingNames = {
	"cbFSR3Upscaler",
	"cbGenerateReactive",
	"cbRCAS",
	"cbSPD",
	"r_accumulation",
	"r_current_luma",
	"r_dilated_depth",
	"r_dilated_motion_vectors",
	"r_dilated_reactive_masks",
	"r_farthest_depth",
	"r_farthest_depth_mip1",
	"r_input_color_jittered",
	"r_input_depth",
	"r_input_exposure",
	"r_input_motion_vectors",
	"r_input_opaque_only",
	"r_internal_upscaled_color",
	"r_lanczos_lut",
	"r_luma_history",
	"r_luma_instability",
	"r_previous_luma",
	"r_rcas_input",
	"r_reactive_mask",
	"r_reconstructed_previous_nearest_depth",
	"r_shading_change",
	"r_spd_mips",
	"r_transparency_and_composition_mask",
	"rw_accumulation",
	"rw_current_luma",
	"rw_dilated_depth",
	"rw_dilated_motion_vectors",
	"rw_dilated_reactive_masks",
	"rw_farthest_depth",
	"rw_farthest_depth_mip1",
	"rw_frame_info",
	"rw_internal_upscaled_color",
	"rw_luma_history",
	"rw_luma_instability",
	"rw_new_locks",
	"rw_output_autoreactive",
	"rw_reconstructed_previous_nearest_depth",
	"rw_shading_change",
	"rw_spd_global_atomic",
	"rw_spd_mip0",
	"rw_spd_mip1",
	"rw_spd_mip2",
	"rw_spd_mip3",
	"rw_spd_mip4",
	"rw_spd_mip5",
	"rw_upscaled_output",
};

class YamlDocument {
public:
	explicit YamlDocument(const fs::path& path) {
		FILE* file = std::fopen(path.string().c_str(), "rb");
		if (!file) {
			throw std::runtime_error("cannot open manifest: " + path.string());
		}
		yaml_parser_t parser;
		if (!yaml_parser_initialize(&parser)) {
			std::fclose(file);
			throw std::runtime_error("cannot initialize YAML parser");
		}
		yaml_parser_set_input_file(&parser, file);
		const int loaded = yaml_parser_load(&parser, &document_);
		if (!loaded) {
			std::ostringstream error;
			error << path.string() << ':' << parser.problem_mark.line + 1 << ": "
				  << (parser.problem ? parser.problem : "YAML parse error");
			yaml_parser_delete(&parser);
			std::fclose(file);
			throw std::runtime_error(error.str());
		}
		yaml_parser_delete(&parser);
		std::fclose(file);
		loaded_ = true;
	}

	~YamlDocument() {
		if (loaded_) {
			yaml_document_delete(&document_);
		}
	}

	yaml_node_t* root() { return yaml_document_get_root_node(&document_); }
	yaml_node_t* node(int index) { return yaml_document_get_node(&document_, index); }

private:
	yaml_document_t document_ {};
	bool            loaded_ = false;
};

[[noreturn]] static void Fail(const yaml_node_t* node, const std::string& message) {
	std::ostringstream error;
	error << "manifest line " << (node ? node->start_mark.line + 1 : 0) << ": " << message;
	throw std::runtime_error(error.str());
}

static std::string Scalar(const yaml_node_t* node, const char* field) {
	if (!node || node->type != YAML_SCALAR_NODE) {
		Fail(node, std::string(field) + " must be a scalar");
	}
	return std::string(reinterpret_cast<const char*>(node->data.scalar.value), node->data.scalar.length);
}

static yaml_node_t* MappingValue(YamlDocument& doc, yaml_node_t* mapping, const std::string& key,
								 bool required = true) {
	if (!mapping || mapping->type != YAML_MAPPING_NODE) {
		Fail(mapping, "expected a mapping while looking for " + key);
	}
	for (yaml_node_pair_t* pair = mapping->data.mapping.pairs.start; pair < mapping->data.mapping.pairs.top;
		 ++pair) {
		yaml_node_t* keyNode = doc.node(pair->key);
		if (Scalar(keyNode, "mapping key") == key) {
			return doc.node(pair->value);
		}
	}
	if (required) {
		Fail(mapping, "missing required field " + key);
	}
	return nullptr;
}

static bool ParseBool(yaml_node_t* node, const char* field) {
	const std::string value = Scalar(node, field);
	if (value == "true") {
		return true;
	}
	if (value == "false") {
		return false;
	}
	Fail(node, std::string(field) + " must be true or false");
}

static uint32_t ParseUInt(yaml_node_t* node, const char* field) {
	const std::string value = Scalar(node, field);
	if (value.empty() ||
		!std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); })) {
		Fail(node, std::string(field) + " must be an unsigned integer");
	}
	const unsigned long parsed = std::stoul(value);
	if (parsed > UINT32_MAX) {
		Fail(node, std::string(field) + " is out of range");
	}
	return static_cast<uint32_t>(parsed);
}

static void RequireIdentifier(const yaml_node_t* node, const std::string& value, const char* field) {
	if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_') ||
		!std::all_of(value.begin() + 1, value.end(),
					 [](unsigned char c) { return std::isalnum(c) || c == '_'; })) {
		Fail(node, std::string(field) + " is not an identifier: " + value);
	}
}

static std::vector<Define> ParseDefines(YamlDocument& doc, yaml_node_t* mapping) {
	std::vector<Define>   result;
	std::set<std::string> names;
	if (!mapping) {
		return result;
	}
	if (mapping->type != YAML_MAPPING_NODE) {
		Fail(mapping, "defines must be a mapping");
	}
	for (yaml_node_pair_t* pair = mapping->data.mapping.pairs.start; pair < mapping->data.mapping.pairs.top;
		 ++pair) {
		yaml_node_t* keyNode   = doc.node(pair->key);
		yaml_node_t* valueNode = doc.node(pair->value);
		Define       define { Scalar(keyNode, "define name"), Scalar(valueNode, "define value") };
		RequireIdentifier(keyNode, define.name, "define name");
		if (!names.insert(define.name).second)
			Fail(keyNode, "duplicate define: " + define.name);
		result.push_back(std::move(define));
	}
	return result;
}

static BindingClass ParseBindingClass(yaml_node_t* node) {
	const std::string value = Scalar(node, "binding class");
	if (value == "sampled_texture")
		return BindingClass::SampledTexture;
	if (value == "storage_texture")
		return BindingClass::StorageTexture;
	if (value == "storage_buffer")
		return BindingClass::StorageBuffer;
	if (value == "declaration_texture")
		return BindingClass::DeclarationSrvTexture;
	if (value == "rw_texture")
		return BindingClass::RwTexture;
	if (value == "rw_buffer")
		return BindingClass::RwBuffer;
	if (value == "constant")
		return BindingClass::Constant;
	Fail(node, "unknown binding class: " + value);
}

static std::vector<std::string> ParseStringSequence(YamlDocument& doc, yaml_node_t* sequence,
													const char* field) {
	if (!sequence || sequence->type != YAML_SEQUENCE_NODE) {
		Fail(sequence, std::string(field) + " must be a sequence");
	}
	std::vector<std::string> result;
	for (yaml_node_item_t* item = sequence->data.sequence.items.start;
		 item < sequence->data.sequence.items.top; ++item) {
		result.push_back(Scalar(doc.node(*item), field));
	}
	return result;
}

static Profile ParseProfile(YamlDocument& doc, yaml_node_t* node) {
	Profile profile;
	profile.id           = Scalar(MappingValue(doc, node, "id"), "profile id");
	profile.enabled      = ParseBool(MappingValue(doc, node, "enabled"), "profile enabled");
	profile.precision    = Scalar(MappingValue(doc, node, "precision"), "profile precision");
	profile.spd          = Scalar(MappingValue(doc, node, "spd"), "profile SPD");
	profile.reprojection = Scalar(MappingValue(doc, node, "reprojection"), "profile reprojection");
	profile.atomicLayout = Scalar(MappingValue(doc, node, "atomic_layout"), "profile atomic layout");
	if (yaml_node_t* reason = MappingValue(doc, node, "unavailable_reason", false)) {
		profile.unavailableReason = Scalar(reason, "unavailable reason");
	}
	RequireIdentifier(node, profile.id, "profile id");
	if (profile.precision != "fp32" && profile.precision != "fp16")
		Fail(node, "invalid precision");
	if (profile.spd != "scalar" && profile.spd != "wave")
		Fail(node, "invalid SPD implementation");
	if (profile.reprojection != "lanczos_lut" && profile.reprojection != "reference")
		Fail(node, "invalid reprojection filter");
	if (profile.atomicLayout != "structured_buffer")
		Fail(node, "unsupported atomic layout");
	if (!profile.enabled && profile.unavailableReason.empty())
		Fail(node, "disabled profile requires unavailable_reason");
	return profile;
}

static Pass ParsePass(YamlDocument& doc, yaml_node_t* node) {
	Pass pass;
	pass.id      = Scalar(MappingValue(doc, node, "id"), "pass id");
	pass.ffxPass = Scalar(MappingValue(doc, node, "ffx_pass"), "FidelityFX pass");
	RequireIdentifier(node, pass.id, "pass id");
	RequireIdentifier(node, pass.ffxPass, "FidelityFX pass");
	if (yaml_node_t* direct = MappingValue(doc, node, "direct_history_variant", false))
		pass.directHistoryVariant = ParseBool(direct, "direct history variant");

	yaml_node_t* threads = MappingValue(doc, node, "threads");
	if (threads->type != YAML_SEQUENCE_NODE ||
		threads->data.sequence.items.top - threads->data.sequence.items.start != 3)
		Fail(threads, "threads must contain exactly three values");
	for (size_t index = 0; index < 3; ++index)
		pass.threads[index] = ParseUInt(doc.node(threads->data.sequence.items.start[index]), "thread count");

	for (const std::string& dimension :
		 ParseStringSequence(doc, MappingValue(doc, node, "dimensions"), "dimensions")) {
		if (dimension != "precision" && dimension != "spd" && dimension != "reprojection")
			Fail(node, "unknown permutation dimension: " + dimension);
		pass.dimensions.insert(dimension);
	}
	pass.defines = ParseDefines(doc, MappingValue(doc, node, "defines", false));

	yaml_node_t* bindings = MappingValue(doc, node, "bindings");
	if (bindings->type != YAML_SEQUENCE_NODE)
		Fail(bindings, "bindings must be a sequence");
	for (yaml_node_item_t* item = bindings->data.sequence.items.start;
		 item < bindings->data.sequence.items.top; ++item) {
		yaml_node_t* bindingNode = doc.node(*item);
		Binding      binding;
		binding.klass = ParseBindingClass(MappingValue(doc, bindingNode, "class"));
		binding.macro = Scalar(MappingValue(doc, bindingNode, "macro"), "binding macro");
		binding.name  = Scalar(MappingValue(doc, bindingNode, "name"), "binding name");
		if (yaml_node_t* when = MappingValue(doc, bindingNode, "when", false))
			binding.when = Scalar(when, "binding condition");
		if (yaml_node_t* sampler = MappingValue(doc, bindingNode, "sampler", false))
			binding.sampler = Scalar(sampler, "binding sampler");
		RequireIdentifier(bindingNode, binding.macro, "binding macro");
		RequireIdentifier(bindingNode, binding.name, "binding name");
		if (!kKnownBindingNames.count(binding.name))
			Fail(bindingNode, "unknown FidelityFX binding name: " + binding.name);
		if (binding.klass == BindingClass::SampledTexture) {
			if (binding.sampler != "linear_clamp" && binding.sampler != "point_clamp")
				Fail(bindingNode, "sampled texture requires a linear_clamp or point_clamp sampler");
		} else if (!binding.sampler.empty()) {
			Fail(bindingNode, "sampler is valid only for sampled textures");
		}
		pass.bindings.push_back(std::move(binding));
	}

	pass.includes = ParseStringSequence(doc, MappingValue(doc, node, "includes"), "includes");
	if (yaml_node_t* root = MappingValue(doc, node, "root_signature", false))
		pass.rootSignature = Scalar(root, "root signature");
	pass.signature = Scalar(MappingValue(doc, node, "signature"), "entry signature");
	pass.body      = Scalar(MappingValue(doc, node, "body"), "entry body");
	return pass;
}

static Manifest LoadManifest(const fs::path& path) {
	YamlDocument document(path);
	yaml_node_t* root = document.root();
	Manifest     manifest;
	manifest.schema = ParseUInt(MappingValue(document, root, "schema"), "schema");
	if (manifest.schema != 1)
		Fail(root, "unsupported schema version");
	yaml_node_t* effect    = MappingValue(document, root, "effect");
	manifest.effectName    = Scalar(MappingValue(document, effect, "name"), "effect name");
	manifest.effectVersion = Scalar(MappingValue(document, effect, "version"), "effect version");
	if (manifest.effectName != "fsr3upscaler" || manifest.effectVersion != "3.1.4")
		Fail(effect, "generator requires the pinned fsr3upscaler 3.1.4 component");
	manifest.commonDefines = ParseDefines(document, MappingValue(document, root, "common_defines"));

	yaml_node_t* profiles = MappingValue(document, root, "profiles");
	if (profiles->type != YAML_SEQUENCE_NODE)
		Fail(profiles, "profiles must be a sequence");
	for (yaml_node_item_t* item = profiles->data.sequence.items.start;
		 item < profiles->data.sequence.items.top; ++item)
		manifest.profiles.push_back(ParseProfile(document, document.node(*item)));

	yaml_node_t* passes = MappingValue(document, root, "passes");
	if (passes->type != YAML_SEQUENCE_NODE)
		Fail(passes, "passes must be a sequence");
	for (yaml_node_item_t* item = passes->data.sequence.items.start; item < passes->data.sequence.items.top;
		 ++item)
		manifest.passes.push_back(ParsePass(document, document.node(*item)));

	std::set<std::string> profileIds;
	for (const Profile& profile : manifest.profiles) {
		if (!profileIds.insert(profile.id).second)
			throw std::runtime_error("duplicate profile: " + profile.id);
		const std::string expectedId = profile.precision + '_' + profile.spd +
									   (profile.reprojection == "lanczos_lut" ? "_lut" : "_reference");
		if (profile.id != expectedId)
			throw std::runtime_error("profile fields do not match id: " + profile.id);
	}
	for (const char* required : { "fp16_wave_lut", "fp32_wave_lut", "fp16_scalar_lut", "fp32_scalar_lut" })
		if (!profileIds.count(required))
			throw std::runtime_error("missing required profile: " + std::string(required));
	const auto fallback =
		std::find_if(manifest.profiles.begin(), manifest.profiles.end(),
					 [](const Profile& profile) { return profile.id == "fp32_scalar_lut"; });
	if (fallback == manifest.profiles.end() || !fallback->enabled)
		throw std::runtime_error("mandatory fp32_scalar_lut profile is disabled");
	std::set<std::string> passIds;
	for (const Pass& pass : manifest.passes) {
		if (!passIds.insert(pass.id).second)
			throw std::runtime_error("duplicate pass: " + pass.id);
		const auto expected = kRequiredPasses.find(pass.id);
		if (expected == kRequiredPasses.end())
			throw std::runtime_error("unknown FSR pass: " + pass.id);
		if (pass.ffxPass != expected->second)
			throw std::runtime_error("wrong FidelityFX pass identifier for " + pass.id);
		if (pass.directHistoryVariant && pass.id != "accumulate")
			throw std::runtime_error("direct-history variant is only valid for accumulate");
		if (pass.threads[0] == 0 || pass.threads[1] == 0 || pass.threads[2] == 0)
			throw std::runtime_error("zero thread-group dimension for " + pass.id);
	}
	for (const auto& required : kRequiredPasses)
		if (!passIds.count(required.first))
			throw std::runtime_error("missing required pass: " + required.first);
	return manifest;
}

static bool BindingEnabled(const Binding& binding, const Profile& profile, bool directHistory) {
	if (binding.when.empty())
		return true;
	if (binding.when == "spd_wave")
		return profile.spd == "wave";
	if (binding.when == "spd_scalar")
		return profile.spd == "scalar";
	if (binding.when == "reprojection_lut")
		return profile.reprojection == "lanczos_lut";
	if (binding.when == "reprojection_reference")
		return profile.reprojection == "reference";
	if (binding.when == "precision_fp16")
		return profile.precision == "fp16";
	if (binding.when == "precision_fp32")
		return profile.precision == "fp32";
	if (binding.when == "output_external")
		return !directHistory;
	if (binding.when == "output_direct_history")
		return directHistory;
	throw std::runtime_error("unknown binding condition: " + binding.when);
}

static std::string VariantKey(const Pass& pass, const Profile& profile, bool directHistory) {
	std::string key = pass.id;
	if (directHistory)
		key += "_direct_history";
	if (pass.dimensions.count("precision"))
		key += '_' + profile.precision;
	if (pass.dimensions.count("spd"))
		key += '_' + profile.spd;
	if (pass.dimensions.count("reprojection"))
		key += profile.reprojection == "lanczos_lut" ? "_lut" : "_reference";
	return key;
}

static std::vector<Define> VariantDefines(const Manifest& manifest, const Variant& variant);

static std::vector<Variant> BuildVariants(const Manifest& manifest) {
	std::vector<Variant>  variants;
	std::set<std::string> variantKeys;
	for (const Profile& profile : manifest.profiles) {
		if (!profile.enabled)
			continue;
		for (const Pass& pass : manifest.passes) {
			const uint32_t outputVariantCount = pass.directHistoryVariant ? 2u : 1u;
			for (uint32_t outputVariant = 0; outputVariant < outputVariantCount; ++outputVariant) {
				const bool directHistory = outputVariant != 0;
				Variant    variant;
				variant.profile       = &profile;
				variant.pass          = &pass;
				variant.directHistory = directHistory;
				variant.key           = VariantKey(pass, profile, directHistory);
				variant.shaderName    = "fsr3_" + variant.key + ".comp";
				for (const Binding& binding : pass.bindings) {
					if (!BindingEnabled(binding, profile, directHistory))
						continue;
					switch (binding.klass) {
						case BindingClass::SampledTexture:
							variant.sampledTextures.push_back(binding);
							break;
						case BindingClass::StorageTexture:
							variant.storageTextures.push_back(binding);
							break;
						case BindingClass::StorageBuffer:
							variant.storageBuffers.push_back(binding);
							break;
						case BindingClass::DeclarationSrvTexture:
							variant.declarationSrvTextures.push_back(binding);
							break;
						case BindingClass::RwTexture:
							variant.rwTextures.push_back(binding);
							break;
						case BindingClass::RwBuffer:
							variant.rwBuffers.push_back(binding);
							break;
						case BindingClass::Constant:
							variant.constants.push_back(binding);
							break;
					}
				}
				const auto validateBindings = [&](const std::vector<Binding>& bindings, const char* klass,
												  uint32_t limit) {
					std::set<std::string> names;
					for (const Binding& binding : bindings) {
						if (!names.insert(binding.name).second)
							throw std::runtime_error("duplicate " + std::string(klass) + " binding in " +
													 variant.key + ": " + binding.name);
					}
					if (bindings.size() > limit)
						throw std::runtime_error("too many " + std::string(klass) + " bindings in " +
												 variant.key);
				};
				validateBindings(variant.sampledTextures, "sampled texture", 16);
				validateBindings(variant.storageTextures, "storage texture", 64);
				validateBindings(variant.storageBuffers, "storage buffer", 64);
				validateBindings(variant.rwTextures, "read-write texture", 64);
				validateBindings(variant.rwBuffers, "read-write buffer", 64);
				validateBindings(variant.constants, "constant buffer", 3);
				const auto hasRwTexture = [&](const char* name) {
					return std::any_of(variant.rwTextures.begin(), variant.rwTextures.end(),
									   [&](const Binding& binding) { return binding.name == name; });
				};
				if (pass.id == "accumulate") {
					const bool hasExternalOutput = hasRwTexture("rw_upscaled_output");
					if (!hasRwTexture("rw_internal_upscaled_color") || !hasRwTexture("rw_new_locks"))
						throw std::runtime_error("incomplete accumulate output bindings for " + variant.key);
					if (hasExternalOutput == directHistory)
						throw std::runtime_error("wrong external-output binding for " + variant.key);
				} else if (pass.id == "accumulate_sharpen") {
					if (!hasRwTexture("rw_internal_upscaled_color") || !hasRwTexture("rw_new_locks") ||
						hasRwTexture("rw_upscaled_output"))
						throw std::runtime_error("wrong accumulate-sharpen output bindings for " +
												 variant.key);
				} else if ((pass.id == "rcas" || pass.id == "debug_view") &&
						   !hasRwTexture("rw_upscaled_output")) {
					throw std::runtime_error("missing external-output binding for " + variant.key);
				}
				std::set<std::string> macros;
				for (const Binding& binding : pass.bindings) {
					if (BindingEnabled(binding, profile, directHistory) &&
						!macros.insert(binding.macro).second)
						throw std::runtime_error("duplicate binding macro in " + variant.key + ": " +
												 binding.macro);
				}
				if (variant.sampledTextures.size() + variant.storageTextures.size() > 64)
					throw std::runtime_error("too many SRV textures in " + variant.key);
				const std::string profileKey = profile.id + ':' + variant.key;
				if (!variantKeys.insert(profileKey).second)
					throw std::runtime_error("duplicate generated variant: " + profileKey);
				variants.push_back(std::move(variant));
			}
		}
	}
	if (variants.empty())
		throw std::runtime_error("manifest has no enabled shader profile");
	return variants;
}

static std::string GenerateWrapper(const Manifest& manifest, const Variant& variant) {
	std::ostringstream output;
	output << "// Generated from fsr3_shaders.yaml. Do not edit.\n\n";
	for (const Define& define : VariantDefines(manifest, variant))
		output << "#define " << define.name << ' ' << define.value << "\n";
	output << '\n';
	uint32_t slot = 0;
	for (const Binding& binding : variant.sampledTextures)
		output << "#define " << binding.macro << ' ' << slot++ << "\n";
	for (const Binding& binding : variant.storageTextures)
		output << "#define " << binding.macro << ' ' << slot++ << "\n";
	for (const Binding& binding : variant.storageBuffers)
		output << "#define " << binding.macro << ' ' << slot++ << "\n";
	for (const Binding& binding : variant.declarationSrvTextures)
		output << "#define " << binding.macro << ' ' << slot++ << "\n";
	output << '\n';
	slot = 0;
	for (const Binding& binding : variant.rwTextures)
		output << "#define " << binding.macro << ' ' << slot++ << "\n";
	for (const Binding& binding : variant.rwBuffers)
		output << "#define " << binding.macro << ' ' << slot++ << "\n";
	output << '\n';
	slot = 0;
	for (const Binding& binding : variant.constants)
		output << "#define " << binding.macro << ' ' << slot++ << "\n";
	output << '\n';
	for (const std::string& include : variant.pass->includes)
		output << "#include \"" << include << "\"\n";
	output << "\n#define FFX_FSR3UPSCALER_THREAD_GROUP_WIDTH " << variant.pass->threads[0]
		   << "\n#define FFX_FSR3UPSCALER_THREAD_GROUP_HEIGHT " << variant.pass->threads[1]
		   << "\n#define FFX_FSR3UPSCALER_THREAD_GROUP_DEPTH " << variant.pass->threads[2]
		   << "\n#define FFX_FSR3UPSCALER_NUM_THREADS "
			  "[numthreads(FFX_FSR3UPSCALER_THREAD_GROUP_WIDTH, FFX_FSR3UPSCALER_THREAD_GROUP_HEIGHT, "
			  "FFX_FSR3UPSCALER_THREAD_GROUP_DEPTH)]\n\n"
		   << "FFX_FSR3UPSCALER_NUM_THREADS\n";
	if (variant.pass->rootSignature == "cb2")
		output << "FFX_FSR3UPSCALER_EMBED_CB2_ROOTSIG_CONTENT\n";
	else
		output << "FFX_FSR3UPSCALER_EMBED_ROOTSIG_CONTENT\n";
	output << "void CS(" << variant.pass->signature << ")\n{\n";
	std::istringstream body(variant.pass->body);
	std::string        line;
	while (std::getline(body, line))
		output << "    " << line << '\n';
	output << "}\n";
	return output.str();
}

static bool WriteIfDifferent(const fs::path& path, const std::string& contents) {
	std::ifstream existing(path, std::ios::binary);
	if (existing) {
		std::ostringstream buffer;
		buffer << existing.rdbuf();
		if (buffer.str() == contents)
			return false;
	}
	fs::create_directories(path.parent_path());
	const fs::path temporary = path.string() + ".tmp";
	std::ofstream  output(temporary, std::ios::binary | std::ios::trunc);
	if (!output)
		throw std::runtime_error("cannot write " + temporary.string());
	output << contents;
	output.close();
	if (!output)
		throw std::runtime_error("failed writing " + temporary.string());
	std::error_code ignored;
	fs::remove(path, ignored);
	fs::rename(temporary, path);
	return true;
}

static std::string ReadFile(const fs::path& path) {
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot read " + path.string());
	std::ostringstream contents;
	contents << input.rdbuf();
	return contents.str();
}

static uint32_t ReadSpirvWord(const std::string& bytes, size_t wordIndex) {
	const size_t offset = wordIndex * sizeof(uint32_t);
	return static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset])) |
		   (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
		   (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
		   (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
}

static bool SpirvContainsNativeFloat16(const fs::path& path) {
	const std::string bytes = ReadFile(path);
	if (bytes.size() < 5 * sizeof(uint32_t) || bytes.size() % sizeof(uint32_t) != 0 ||
		ReadSpirvWord(bytes, 0) != UINT32_C(0x07230203))
		throw std::runtime_error("invalid SPIR-V module: " + path.string());

	bool         hasFloat16Capability = false;
	bool         hasFloat16Type       = false;
	size_t       wordIndex            = 5;
	const size_t wordCount            = bytes.size() / sizeof(uint32_t);
	while (wordIndex < wordCount) {
		const uint32_t instruction = ReadSpirvWord(bytes, wordIndex);
		const uint32_t length      = instruction >> 16;
		const uint32_t opcode      = instruction & UINT32_C(0xffff);
		if (length == 0 || wordIndex + length > wordCount)
			throw std::runtime_error("malformed SPIR-V module: " + path.string());
		/* OpCapability Float16 and OpTypeFloat 16 prove that native half
		 * precision survived DXC's SPIR-V lowering. */
		if (opcode == 17 && length >= 2 && ReadSpirvWord(bytes, wordIndex + 1) == 9)
			hasFloat16Capability = true;
		if (opcode == 22 && length >= 3 && ReadSpirvWord(bytes, wordIndex + 2) == 16)
			hasFloat16Type = true;
		wordIndex += length;
	}
	return hasFloat16Capability && hasFloat16Type;
}

static bool SpirvContainsOpcode(const fs::path& path, uint32_t expectedOpcode) {
	const std::string bytes = ReadFile(path);
	if (bytes.size() < 5 * sizeof(uint32_t) || bytes.size() % sizeof(uint32_t) != 0 ||
		ReadSpirvWord(bytes, 0) != UINT32_C(0x07230203))
		throw std::runtime_error("invalid SPIR-V module: " + path.string());

	size_t       wordIndex = 5;
	const size_t wordCount = bytes.size() / sizeof(uint32_t);
	while (wordIndex < wordCount) {
		const uint32_t instruction = ReadSpirvWord(bytes, wordIndex);
		const uint32_t length      = instruction >> 16;
		if (length == 0 || wordIndex + length > wordCount)
			throw std::runtime_error("malformed SPIR-V module: " + path.string());
		if ((instruction & UINT32_C(0xffff)) == expectedOpcode)
			return true;
		wordIndex += length;
	}
	return false;
}

static uint64_t Fnv1a(const std::string& text) {
	uint64_t hash = UINT64_C(14695981039346656037);
	for (unsigned char value : text) {
		hash ^= value;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static std::string CppString(const std::string& text) {
	std::string result = "\"";
	for (char value : text) {
		if (value == '\\' || value == '"')
			result += '\\';
		result += value;
	}
	return result + '"';
}

static void EmitBindingArray(std::ostringstream& output, const std::string& variable,
							 const std::vector<Binding>& bindings) {
	if (bindings.empty())
		return;
	output << "static const wchar_t* const " << variable << "[] = {\n";
	for (const Binding& binding : bindings)
		output << "    L\"" << binding.name << "\",\n";
	output << "};\n";
}

static void EmitSamplerArray(std::ostringstream& output, const std::string& variable,
							 const std::vector<Binding>& bindings) {
	if (bindings.empty())
		return;
	output << "static const SamplerKind " << variable << "[] = {\n";
	for (const Binding& binding : bindings)
		output << "    SamplerKind::" << (binding.sampler == "point_clamp" ? "PointClamp" : "LinearClamp")
			   << ",\n";
	output << "};\n";
}

static std::string ArrayArg(const std::string& variable, const std::vector<Binding>& bindings) {
	return bindings.empty() ? "nullptr, 0"
							: variable + ", static_cast<uint32_t>(std::size(" + variable + "))";
}

static std::string GenerateHeader(const Manifest& manifest, const std::vector<Variant>& variants,
								  uint64_t hash) {
	std::ostringstream output;
	output << "#pragma once\n\n// Generated from fsr3_shaders.yaml. Do not edit.\n\n";
	output << "static constexpr uint32_t kFsr3ManifestSchema = " << manifest.schema << ";\n\n";
	output << "static const AvailableProfile kFsr3AvailableProfiles[] = {\n";
	for (const Profile& profile : manifest.profiles) {
		if (profile.enabled)
			output << "    { " << CppString(profile.id) << ", "
				   << (profile.precision == "fp16" ? "true" : "false") << ", "
				   << (profile.spd == "wave" ? "true" : "false") << ", "
				   << (profile.reprojection == "lanczos_lut" ? "true" : "false") << ", "
				   << CppString(profile.atomicLayout) << " },\n";
	}
	output << "};\n\n";
	for (const Variant& variant : variants) {
		const std::string    prefix      = "kFsr3_" + variant.profile->id + '_' + variant.key;
		std::vector<Binding> srvTextures = variant.sampledTextures;
		srvTextures.insert(srvTextures.end(), variant.storageTextures.begin(), variant.storageTextures.end());
		EmitBindingArray(output, prefix + "_srv_textures", srvTextures);
		EmitBindingArray(output, prefix + "_srv_buffers", variant.storageBuffers);
		EmitBindingArray(output, prefix + "_uav_textures", variant.rwTextures);
		EmitBindingArray(output, prefix + "_uav_buffers", variant.rwBuffers);
		EmitBindingArray(output, prefix + "_constants", variant.constants);
		EmitSamplerArray(output, prefix + "_samplers", variant.sampledTextures);
	}
	output << "\nstatic const PipelineManifest kFsr3PipelineManifests[] = {\n";
	for (const Variant& variant : variants) {
		const std::string    prefix      = "kFsr3_" + variant.profile->id + '_' + variant.key;
		std::vector<Binding> srvTextures = variant.sampledTextures;
		srvTextures.insert(srvTextures.end(), variant.storageTextures.begin(), variant.storageTextures.end());
		const bool effectiveFp16 =
			variant.pass->dimensions.count("precision") && variant.profile->precision == "fp16";
		output << "    { " << variant.pass->ffxPass << ", " << (variant.directHistory ? "true" : "false")
			   << ", " << CppString(variant.profile->id) << ", " << (effectiveFp16 ? "true" : "false") << ", "
			   << (variant.profile->spd == "wave" ? "true" : "false") << ", "
			   << (variant.profile->reprojection == "lanczos_lut" ? "true" : "false") << ", "
			   << CppString(variant.profile->atomicLayout) << ", " << CppString(variant.shaderName) << ", "
			   << variant.pass->threads[0] << ", " << variant.pass->threads[1] << ", "
			   << variant.sampledTextures.size() << ", "
			   << (variant.sampledTextures.empty() ? "nullptr" : prefix + "_samplers") << ", "
			   << ArrayArg(prefix + "_srv_textures", srvTextures) << ", "
			   << ArrayArg(prefix + "_srv_buffers", variant.storageBuffers) << ", "
			   << ArrayArg(prefix + "_uav_textures", variant.rwTextures) << ", "
			   << ArrayArg(prefix + "_uav_buffers", variant.rwBuffers) << ", "
			   << ArrayArg(prefix + "_constants", variant.constants) << " },\n";
	}
	output << "};\n\nstatic const UnavailableProfile kFsr3UnavailableProfiles[] = {\n";
	for (const Profile& profile : manifest.profiles) {
		if (!profile.enabled)
			output << "    { " << CppString(profile.id) << ", " << CppString(profile.unavailableReason)
				   << " },\n";
	}
	output << "    { nullptr, nullptr },\n";
	output << "};\n\nstatic constexpr const char* kFsr3ManifestHash = \"" << std::hex << std::setw(16)
		   << std::setfill('0') << hash << "\";\n";
	return output.str();
}

static std::vector<Define> VariantDefines(const Manifest& manifest, const Variant& variant) {
	std::vector<Define> defines = manifest.commonDefines;
	defines.insert(defines.end(), variant.pass->defines.begin(), variant.pass->defines.end());
	const bool fp16 = variant.pass->dimensions.count("precision") && variant.profile->precision == "fp16";
	defines.push_back({ "FFX_HALF", fp16 ? "1" : "0" });
	if (variant.pass->dimensions.count("spd") && variant.profile->spd == "scalar")
		defines.push_back({ "FFX_SPD_NO_WAVE_OPERATIONS", "1" });
	if (variant.pass->dimensions.count("reprojection"))
		defines.push_back({ "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE",
							variant.profile->reprojection == "lanczos_lut" ? "1" : "0" });
	if (variant.directHistory)
		defines.push_back({ "AERON_FSR3UPSCALER_DIRECT_HISTORY_OUTPUT", "1" });
	return defines;
}

static void RunProcess(const std::vector<std::string>& arguments) {
	std::vector<const char*> argv;
	argv.reserve(arguments.size() + 1);
	for (const std::string& argument : arguments)
		argv.push_back(argument.c_str());
	argv.push_back(nullptr);
	SDL_Process* process = SDL_CreateProcess(argv.data(), false);
	if (!process)
		throw std::runtime_error("could not start " + arguments.front() + ": " + SDL_GetError());
	int exitCode = -1;
	if (!SDL_WaitProcess(process, true, &exitCode)) {
		const std::string error = SDL_GetError();
		SDL_DestroyProcess(process);
		throw std::runtime_error("could not wait for shader compiler: " + error);
	}
	SDL_DestroyProcess(process);
	if (exitCode != 0)
		throw std::runtime_error("shader compiler exited with code " + std::to_string(exitCode));
}

static fs::file_time_type NewestSourceTime(const fs::path& manifestPath, const fs::path& gpuInclude,
										   const fs::path& shadercross, const fs::path& generator) {
	fs::file_time_type newest =
		std::max({ fs::last_write_time(manifestPath), fs::last_write_time(shadercross),
				   fs::last_write_time(generator) });
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(gpuInclude)) {
		if (entry.is_regular_file())
			newest = std::max(newest, entry.last_write_time());
	}
	return newest;
}

struct Reflection {
	uint32_t samplers        = 0;
	uint32_t storageTextures = 0;
	uint32_t storageBuffers  = 0;
	uint32_t rwTextures      = 0;
	uint32_t rwBuffers       = 0;
	uint32_t uniforms        = 0;
	uint32_t threadX         = 0;
	uint32_t threadY         = 0;
	uint32_t threadZ         = 0;
};

static Reflection ParseReflection(const fs::path& path) {
	const std::string json = ReadFile(path);
	Reflection        result;
	const int         count = std::sscanf(
		json.c_str(),
		"{ \"samplers\": %u, \"readonly_storage_textures\": %u, \"readonly_storage_buffers\": %u, "
		"\"readwrite_storage_textures\": %u, \"readwrite_storage_buffers\": %u, \"uniform_buffers\": %u, "
		"\"threadcount_x\": %u, \"threadcount_y\": %u, \"threadcount_z\": %u }",
		&result.samplers, &result.storageTextures, &result.storageBuffers, &result.rwTextures,
		&result.rwBuffers, &result.uniforms, &result.threadX, &result.threadY, &result.threadZ);
	if (count != 9)
		throw std::runtime_error("unexpected shader reflection format: " + path.string());
	return result;
}

static void ValidateReflection(const Variant& variant, const Reflection& reflection) {
	const Reflection expected {
		static_cast<uint32_t>(variant.sampledTextures.size()),
		static_cast<uint32_t>(variant.storageTextures.size()),
		static_cast<uint32_t>(variant.storageBuffers.size()),
		static_cast<uint32_t>(variant.rwTextures.size()),
		static_cast<uint32_t>(variant.rwBuffers.size()),
		static_cast<uint32_t>(variant.constants.size()),
		variant.pass->threads[0],
		variant.pass->threads[1],
		variant.pass->threads[2],
	};
	if (reflection.samplers != expected.samplers || reflection.storageTextures != expected.storageTextures ||
		reflection.storageBuffers != expected.storageBuffers ||
		reflection.rwTextures != expected.rwTextures || reflection.rwBuffers != expected.rwBuffers ||
		reflection.uniforms != expected.uniforms || reflection.threadX != expected.threadX ||
		reflection.threadY != expected.threadY || reflection.threadZ != expected.threadZ) {
		throw std::runtime_error("reflection mismatch for " + variant.shaderName);
	}
}

static void ValidateMslSlots(const Variant& variant, const std::string& msl, const char* resourceClass,
							 uint32_t expectedCount) {
	const std::string  marker = std::string("[[") + resourceClass + '(';
	std::set<uint32_t> slots;
	size_t             offset = 0;
	uint32_t           count  = 0;
	while ((offset = msl.find(marker, offset)) != std::string::npos) {
		offset += marker.size();
		const size_t end = msl.find(')', offset);
		if (end == std::string::npos || end == offset)
			throw std::runtime_error("invalid MSL " + std::string(resourceClass) + " attribute in " +
									 variant.shaderName);
		const uint32_t slot = static_cast<uint32_t>(std::stoul(msl.substr(offset, end - offset)));
		if (!slots.insert(slot).second)
			throw std::runtime_error("duplicate MSL " + std::string(resourceClass) + " slot in " +
									 variant.shaderName);
		++count;
		offset = end + 1;
	}
	if (count != expectedCount)
		throw std::runtime_error("wrong MSL " + std::string(resourceClass) + " count in " +
								 variant.shaderName);
}

static bool UsesFp16(const Variant& variant) {
	return variant.pass->dimensions.count("precision") && variant.profile->precision == "fp16";
}

static bool UsesWaveSpd(const Variant& variant) {
	return variant.pass->dimensions.count("spd") && variant.profile->spd == "wave";
}

static void CompileSpirv(const Variant& variant, const fs::path& wrapper, const fs::path& gpuInclude,
						 const fs::path& output, const fs::path& shadercross) {
	std::vector<std::string> args { shadercross.string(),
									wrapper.string(),
									"--source",
									"HLSL",
									"--dest",
									"SPIRV",
									"--stage",
									"compute",
									"--entrypoint",
									"CS",
									"-I",
									gpuInclude.string() };
	if (UsesWaveSpd(variant))
		args.push_back("--spirv-vulkan1.1");
	if (UsesFp16(variant))
		args.push_back("--enable-16bit-types");
	args.push_back("--output");
	args.push_back(output.string());
	RunProcess(args);
}

static void ConvertSpirv(const Variant& variant, const fs::path& spirv, const std::string& destination,
						 const fs::path& output, const fs::path& shadercross) {
	std::vector<std::string> args {
		shadercross.string(), spirv.string(), "--source", "SPIRV",        "--dest",
		destination,          "--stage",      "compute",  "--entrypoint", "CS"
	};
	if (destination == "MSL") {
		args.push_back("--msl-version");
		args.push_back(UsesWaveSpd(variant) ? "2.1.0" : "2.0.0");
	} else if (destination == "DXIL" && UsesFp16(variant)) {
		args.push_back("--enable-16bit-types");
	}
	args.push_back("--output");
	args.push_back(output.string());
	RunProcess(args);
}

static void CopySpirv(const fs::path& source, const fs::path& destination) {
	std::error_code error;
	fs::copy_file(source, destination, fs::copy_options::overwrite_existing, error);
	if (error)
		throw std::runtime_error("cannot copy " + source.string() + " to " + destination.string() + ": " +
								 error.message());
}

static void ValidateGeneratedShader(const Variant& variant, const fs::path& spirv, const fs::path& reflection,
									const fs::path& outputDir, const std::set<std::string>& formats) {
	ValidateReflection(variant, ParseReflection(reflection));
	std::string msl;
	if (formats.count("msl")) {
		msl = ReadFile(outputDir / (variant.shaderName + ".msl"));
		ValidateMslSlots(variant, msl, "texture",
						 static_cast<uint32_t>(variant.sampledTextures.size() +
											   variant.storageTextures.size() + variant.rwTextures.size()));
		ValidateMslSlots(variant, msl, "sampler", static_cast<uint32_t>(variant.sampledTextures.size()));
		ValidateMslSlots(variant, msl, "buffer",
						 static_cast<uint32_t>(variant.storageBuffers.size() + variant.rwBuffers.size() +
											   variant.constants.size()));
		if (variant.pass->dimensions.count("reprojection") &&
			variant.profile->reprojection == "lanczos_lut" && msl.find("sin(") != std::string::npos)
			throw std::runtime_error("LUT accumulate contains reference Lanczos sin(): " +
									 variant.shaderName);
	}
	const bool fp16 = UsesFp16(variant);
	/* Some AMD FP16 permutations intentionally contain only FP32 operations.
	 * Accumulate always exercises the native-half upsample/reprojection path. */
	const bool mustContainNativeHalf =
		fp16 && (variant.pass->id == "accumulate" || variant.pass->id == "accumulate_sharpen");
	if (mustContainNativeHalf && formats.count("msl") && msl.find("half") == std::string::npos)
		throw std::runtime_error("FP16 accumulate shader contains no native half types: " +
								 variant.shaderName);
	if (mustContainNativeHalf && !SpirvContainsNativeFloat16(spirv))
		throw std::runtime_error("FP16 accumulate SPIR-V contains no native float16 operations: " +
								 variant.shaderName);
	if (variant.pass->dimensions.count("spd")) {
		const bool mslUsesQuadShuffle =
			formats.count("msl") && msl.find("quad_shuffle_xor(") != std::string::npos;
		/* OpGroupNonUniformQuadSwap proves that DXC did not lower the wave
		 * permutation back to the groupshared scalar implementation. */
		const bool spirvUsesQuadSwap = SpirvContainsOpcode(spirv, 366);
		if (variant.profile->spd == "wave" &&
			(!spirvUsesQuadSwap || (formats.count("msl") && !mslUsesQuadShuffle)))
			throw std::runtime_error("wave SPD shader contains no native quad shuffle: " +
									 variant.shaderName);
		if (variant.profile->spd == "scalar" &&
			(spirvUsesQuadSwap || (formats.count("msl") && mslUsesQuadShuffle)))
			throw std::runtime_error("scalar SPD shader unexpectedly contains a quad shuffle: " +
									 variant.shaderName);
	}
}

static std::string GenerateInventory(const Manifest& manifest, const std::vector<Variant>& variants,
									 const std::set<std::string>& formats, uint64_t hash) {
	std::ostringstream output;
	output << "schema: 1\neffect: " << manifest.effectName << "\nversion: " << manifest.effectVersion
		   << "\nmanifest_hash: \"" << std::hex << std::setw(16) << std::setfill('0') << hash
		   << "\"\nprofiles:\n";
	for (const Profile& profile : manifest.profiles) {
		output << "  - id: " << profile.id << "\n    enabled: " << (profile.enabled ? "true" : "false")
			   << "\n    precision: " << profile.precision << "\n    spd: " << profile.spd
			   << "\n    reprojection: " << profile.reprojection
			   << "\n    atomic_layout: " << profile.atomicLayout << '\n';
		if (!profile.unavailableReason.empty())
			output << "    unavailable_reason: \"" << profile.unavailableReason << "\"\n";
	}
	output << "shaders:\n";
	for (const Variant& variant : variants) {
		output << "  - profile: " << variant.profile->id << "\n    pass: " << variant.pass->id
			   << "\n    name: " << variant.shaderName << "\n    precision: " << variant.profile->precision
			   << "\n    spd: " << variant.profile->spd
			   << "\n    reprojection: " << variant.profile->reprojection << "\n    outputs: [";
		bool first = true;
		for (const std::string& format : formats) {
			if (!first)
				output << ", ";
			output << format;
			first = false;
		}
		output << (first ? "" : ", ") << "reflection]\n";
	}
	return output.str();
}

static void RemoveStaleFiles(const fs::path& directory, const std::set<std::string>& expected,
							 const std::set<std::string>& extensions) {
	if (!fs::exists(directory))
		return;
	for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
		if (!entry.is_regular_file())
			continue;
		const std::string filename = entry.path().filename().string();
		if (filename.rfind("fsr3_", 0) == 0 && extensions.count(entry.path().extension().string()) &&
			!expected.count(filename))
			fs::remove(entry.path());
	}
}

struct Arguments {
	fs::path              generator;
	fs::path              manifest;
	fs::path              gpuInclude;
	fs::path              generatedDir;
	fs::path              shaderOutputDir;
	fs::path              shadercross;
	std::set<std::string> formats;
};

static std::set<std::string> ParseFormats(const std::string& value) {
	std::set<std::string> formats;
	std::istringstream    input(value);
	std::string           format;
	while (std::getline(input, format, ',')) {
		format.erase(std::remove_if(format.begin(), format.end(),
									[](unsigned char character) { return std::isspace(character) != 0; }),
					 format.end());
		std::transform(format.begin(), format.end(), format.begin(),
					   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		if (format != "msl" && format != "spv" && format != "spirv" && format != "dxil")
			throw std::runtime_error("unknown shader output format: " + format);
		formats.insert(format == "spirv" ? "spv" : format);
	}
	if (formats.empty())
		throw std::runtime_error("shader output format list is empty");
	return formats;
}

static Arguments ParseArguments(int argc, char** argv) {
	Arguments arguments;
	arguments.generator = fs::absolute(argv[0]);
	if ((argc - 1) % 2 != 0)
		throw std::runtime_error("shader generator arguments must be option/value pairs");
	for (int index = 1; index + 1 < argc; index += 2) {
		const std::string option = argv[index];
		if (option == "--manifest")
			arguments.manifest = argv[index + 1];
		else if (option == "--gpu-include")
			arguments.gpuInclude = argv[index + 1];
		else if (option == "--generated-dir")
			arguments.generatedDir = argv[index + 1];
		else if (option == "--shader-output-dir")
			arguments.shaderOutputDir = argv[index + 1];
		else if (option == "--shadercross")
			arguments.shadercross = argv[index + 1];
		else if (option == "--formats")
			arguments.formats = ParseFormats(argv[index + 1]);
		else
			throw std::runtime_error("unknown argument: " + option);
	}
	if (arguments.manifest.empty() || arguments.gpuInclude.empty() || arguments.generatedDir.empty() ||
		arguments.shaderOutputDir.empty() || arguments.shadercross.empty() || arguments.formats.empty())
		throw std::runtime_error("missing required shader generator argument");
	return arguments;
}

static int Run(int argc, char** argv) {
	const Arguments            arguments     = ParseArguments(argc, argv);
	const Manifest             manifest      = LoadManifest(arguments.manifest);
	const std::vector<Variant> variants      = BuildVariants(manifest);
	const std::string          source        = ReadFile(arguments.manifest);
	const uint64_t             hash          = Fnv1a(source);
	const fs::path             wrapperDir    = arguments.generatedDir / "shaders";
	const fs::path             includeDir    = arguments.generatedDir / "include";
	const fs::path             spirvDir      = arguments.generatedDir / "intermediate";
	const fs::path             reflectionDir = arguments.generatedDir / "reflection";
	fs::create_directories(wrapperDir);
	fs::create_directories(includeDir);
	fs::create_directories(spirvDir);
	fs::create_directories(reflectionDir);
	fs::create_directories(arguments.shaderOutputDir);

	WriteIfDifferent(includeDir / "aeron_fsr3_shader_manifest.generated.h",
					 GenerateHeader(manifest, variants, hash));
	const fs::file_time_type    newestSource = NewestSourceTime(arguments.manifest, arguments.gpuInclude,
																arguments.shadercross, arguments.generator);
	std::vector<const Variant*> uniqueVariants;
	std::map<std::string, std::string> uniqueWrappers;
	for (const Variant& variant : variants) {
		const std::string wrapperContents = GenerateWrapper(manifest, variant);
		auto [entry, inserted]            = uniqueWrappers.emplace(variant.shaderName, wrapperContents);
		if (!inserted) {
			if (entry->second != wrapperContents)
				throw std::runtime_error("conflicting generated wrappers share shader name: " +
										 variant.shaderName);
			continue;
		}
		uniqueVariants.push_back(&variant);
	}

	std::set<std::string> expectedWrappers;
	std::set<std::string> expectedIntermediate;
	std::set<std::string> expectedReflections;
	std::set<std::string> expectedShaders;
	for (const Variant* variantPtr : uniqueVariants) {
		const Variant& variant        = *variantPtr;
		const fs::path wrapper        = wrapperDir / (variant.shaderName + ".hlsl");
		const fs::path spirv          = spirvDir / (variant.shaderName + ".spv");
		const fs::path reflection     = reflectionDir / (variant.shaderName + ".json");
		const bool     wrapperChanged = WriteIfDifferent(wrapper, uniqueWrappers.at(variant.shaderName));
		expectedWrappers.insert(wrapper.filename().string());
		expectedIntermediate.insert(spirv.filename().string());
		expectedReflections.insert(reflection.filename().string());

		const bool compileSpirv = wrapperChanged || !fs::exists(spirv) ||
								  fs::last_write_time(spirv) < newestSource ||
								  fs::last_write_time(spirv) < fs::last_write_time(wrapper);
		if (compileSpirv) {
			std::cout << "shadercross FSR 3.1.4 SPIR-V: " << variant.shaderName << '\n';
			CompileSpirv(variant, wrapper, arguments.gpuInclude, spirv, arguments.shadercross);
		}

		if (compileSpirv || !fs::exists(reflection) ||
			fs::last_write_time(reflection) < fs::last_write_time(spirv)) {
			ConvertSpirv(variant, spirv, "JSON", reflection, arguments.shadercross);
		}

		for (const std::string& format : arguments.formats) {
			const std::string extension = format == "spv" ? ".spv" : "." + format;
			const fs::path    output    = arguments.shaderOutputDir / (variant.shaderName + extension);
			expectedShaders.insert(output.filename().string());
			if (!compileSpirv && fs::exists(output) &&
				fs::last_write_time(output) >= fs::last_write_time(spirv))
				continue;
			if (format == "spv") {
				CopySpirv(spirv, output);
			} else {
				std::string destination = format;
				std::transform(
					destination.begin(), destination.end(), destination.begin(),
					[](unsigned char character) { return static_cast<char>(std::toupper(character)); });
				ConvertSpirv(variant, spirv, destination, output, arguments.shadercross);
			}
		}
		ValidateGeneratedShader(variant, spirv, reflection, arguments.shaderOutputDir, arguments.formats);
	}

	RemoveStaleFiles(wrapperDir, expectedWrappers, { ".hlsl" });
	RemoveStaleFiles(spirvDir, expectedIntermediate, { ".spv" });
	RemoveStaleFiles(reflectionDir, expectedReflections, { ".json" });
	RemoveStaleFiles(arguments.shaderOutputDir, expectedShaders, { ".json", ".msl", ".spv", ".dxil" });
	WriteIfDifferent(arguments.generatedDir / "fsr3_shader_inventory.yaml",
					 GenerateInventory(manifest, variants, arguments.formats, hash));
	const fs::path stamp         = arguments.generatedDir / "fsr3_shader_bundle.stamp";
	std::string    stampContents = std::to_string(hash) + "\nformats:";
	for (const std::string& format : arguments.formats)
		stampContents += " " + format;
	WriteIfDifferent(stamp, stampContents + "\n");
	/* A dependency-only source change can rebuild shaders without changing the
	 * manifest hash. Refresh the stamp so build systems do not rerun forever. */
	fs::last_write_time(stamp, fs::file_time_type::clock::now());
	return 0;
}

} // namespace

int main(int argc, char** argv) {
	try {
		return Run(argc, argv);
	} catch (const std::exception& error) {
		std::cerr << "fsr3_shadergen: " << error.what() << '\n';
		return 1;
	}
}
