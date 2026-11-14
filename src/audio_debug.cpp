/*
 * Audio debug tool: the output format, the spatial mode and HRTF data set,
 * the voices, and the time spent mixing. It can switch positional voices
 * between speakers and headphones. Built only with AERON_DEBUG_UI.
 */

#include "audio_debug.h"

#include "aeron/audio.h"
#include "aeron/debug.h"

#include <imgui.h>

namespace {

void draw_tool(int* open, void* user) {
	(void)user;
	ImGui::SetNextWindowSize(ImVec2(380, 220), ImGuiCond_FirstUseEver);
	bool visible = *open != 0;
	if (ImGui::Begin("Audio", &visible)) {
		AeronAudioDebugInfo info;
		Aeron_AudioGetDebugInfo(&info);
		ImGui::Text("Output: %d channels, F32, 48000 Hz", info.out_channels);
		if (info.device_channels > 0)
			ImGui::Text("Device: %d channels", info.device_channels);
		else
			ImGui::TextDisabled("Device: channel count unknown");
		/* The game has no setting for this yet, so the tool can switch it. */
		int headphones = info.headphones;
		ImGui::TextUnformatted("Positional voices:");
		ImGui::SameLine();
		ImGui::RadioButton("Speakers", &headphones, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Headphones (HRTF)", &headphones, 1);
		if (headphones != info.headphones) {
			AeronAudioSpatialSettings settings = {};
			settings.mode = headphones ? AERON_AUDIO_OUTPUT_HEADPHONES : AERON_AUDIO_OUTPUT_SPEAKERS;
			Aeron_AudioSetSpatialSettings(&settings);
		}
		if (info.hrtf_responses > 0)
			ImGui::Text("HRTF data set: %d responses of %d taps", info.hrtf_responses, info.hrtf_taps);
		else
			ImGui::TextDisabled("HRTF data set: none loaded");
		ImGui::Text("Voices: %d, %d of them positional", info.voices, info.positional_voices);
		ImGui::Separator();
		/* The time spent mixing, as a share of the audio it produced. */
		ImGui::Text("Mixing over the last second: %.1f%% on average, %.1f%% at most",
					info.mix_load_average * 100.0f, info.mix_load_peak * 100.0f);
	}
	ImGui::End();
	*open = visible ? 1 : 0;
}

} // namespace

extern "C" void Aeron_AudioRegisterDebugTool(void) {
	static bool registered = false;
	if (!registered) {
		Aeron_DebugRegisterTool("Audio", draw_tool, nullptr);
		registered = true;
	}
}
