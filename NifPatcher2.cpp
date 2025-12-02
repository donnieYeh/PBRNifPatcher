// NifPatcher2.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "nifly/src/NifFile.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include "json.hpp"
using json = nlohmann::json;
using namespace std;
using namespace std::filesystem;

enum class LogLevel { Info, Warning, Error };

void log_message(LogLevel level, const std::string& message) {
	const char* prefix = "[INFO] ";
	auto& out = (level == LogLevel::Error) ? std::cerr : std::cout;
	if (level == LogLevel::Warning)
		prefix = "[WARN] ";
	else if (level == LogLevel::Error)
		prefix = "[ERROR] ";

	out << prefix << message << std::endl;
}

void log_info(const std::string& message) { log_message(LogLevel::Info, message); }
void log_warn(const std::string& message) { log_message(LogLevel::Warning, message); }
void log_error(const std::string& message) { log_message(LogLevel::Error, message); }

std::string describe_load_error(int code) {
	switch (code) {
	case 1:
		return "invalid stream or header";
	case 2:
		return "unsupported NIF version";
	case 3:
		return "encountered unknown block without size information";
	default:
		return "unknown error";
	}
}

std::string describe_save_error(int code) {
	if (code == 1)
		return "unable to open output stream";
	if (code == 0)
		return "success";
	return "unknown error";
}

std::string str_tolower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
		// static_cast<int(*)(int)>(std::tolower)         // wrong
		// [](int c){ return std::tolower(c); }           // wrong
		// [](char c){ return std::tolower(c); }          // wrong
		[](unsigned char c) { return std::tolower(c); } // correct
	);
	return s;
}

Vector2 abs(Vector2 v) {
	return Vector2(abs(v.u), abs(v.v));
}

Vector2 auto_uv_scale(const vector<Vector2>* uvs, const vector<Vector3>* verts, vector<Triangle>& tris) {
	Vector2 scale;
	for (const Triangle& t : tris) {
		auto v1 = (*verts)[t.p1];
		auto v2 = (*verts)[t.p2];
		auto v3 = (*verts)[t.p3];
		auto uv1 = (*uvs)[t.p1];
		auto uv2 = (*uvs)[t.p2];
		auto uv3 = (*uvs)[t.p3];

		//auto cross = (v2 - v1).cross(v3 - v1);
		//auto uv_cross = Vector3((uv2 - uv1).u, (uv2 - uv1).v, 0).cross(Vector3((uv3 - uv1).u, (uv3 - uv1).v, 0));
		//auto s = cross.length() / uv_cross.length();
		//scale += Vector2(s, s);
		//auto s = (abs(uv2 - uv1) / (v2 - v1).length() + abs(uv3 - uv1) / (v3 - v1).length() + abs(uv2 - uv3) / (v2 - v3).length())/3;
		//scale += Vector2(1.0 / s.u, 1.0 / s.v);
		auto s = (abs(uv2 - uv1) + abs(uv3 - uv1)) / ((v2 - v1).length() + (v3 - v1).length());
		scale += Vector2(1.0 / s.u, 1.0 / s.v);
	}
	scale *= 10.0 / 4.0;
	scale /= tris.size();
	scale.u = min(scale.u, scale.v);
	scale.v = min(scale.u, scale.v);
	return scale;
}

bool flag(nlohmann::json_abi_v3_11_3::json& json, const char* key) {
	return json.contains(key) && json[key];
}

bool set_pbr_textures(NifFile& nif, vector<json> js, string& filename) {
	auto modified = false;
	for (const auto shape : nif.GetShapes())
	{
		const auto shapeName = shape && !shape->name.get().empty() ? shape->name.get() : "<unnamed shape>";
		auto shapeModified = false;
		auto paths = nif.GetTexturePathRefs(shape);
		if (paths.size() < 1)
		{
			log_warn("Shape '" + shapeName + "' has no texture paths; skipping.");
			continue;
		}
		const auto shader = nif.GetShader(shape);
		if (!shader)
		{
			log_warn("Shape '" + shapeName + "' has no shader property; skipping.");
			continue;
		}
		const auto bslsp = dynamic_cast<BSLightingShaderProperty*>(shader);
		if (!bslsp)
		{
			log_warn("Shape '" + shapeName + "' is not using BSLightingShaderProperty; skipping.");
			continue;
		}
		auto orig_diff_path = str_tolower(paths[0].get());
		if (orig_diff_path.length() >= 4)
		{
			orig_diff_path.pop_back(); // remove ".dds"
			orig_diff_path.pop_back();
			orig_diff_path.pop_back();
			orig_diff_path.pop_back();
		}
		auto orig_n_path = str_tolower(paths[1].get());
		if (orig_n_path.length() >= 6)
		{
			orig_n_path.pop_back(); // remove "_n.dds"
			orig_n_path.pop_back();
			orig_n_path.pop_back();
			orig_n_path.pop_back();
			orig_n_path.pop_back();
			orig_n_path.pop_back();
		}

		for (auto& settings : js)
			for (auto& element : settings) {
				//std::cout << element << '\n';
				if (element.contains("nif_filter") && filename.find(element["nif_filter"]) == string::npos)
					continue;
				auto contains_match = element.contains("path_contains") && orig_diff_path.find(element["path_contains"]) != string::npos;
				auto name_match = false;
				string matched_path;
				if (element.contains("match_normal") && orig_n_path.ends_with(element["match_normal"])) {
					name_match = true;
					matched_path = orig_n_path;
				}
				else if (element.contains("match_diffuse") && orig_diff_path.ends_with(element["match_diffuse"])) {
					name_match = true;
					matched_path = orig_diff_path;
				}
				if (!contains_match && !name_match)
					continue;
				if (element.contains("delete") && element["delete"]) {
					// delete this fkin mesh bro
					log_info("Deleting shape '" + shapeName + "' due to config match.");
					nif.DeleteShape(shape);
					modified = true;
					shapeModified = true;
					break;
				}
				if (element.contains("smooth_angle")) {
					nif.CalcNormalsForShape(shape, true, true, element["smooth_angle"]);
					nif.CalcTangentsForShape(shape);
					modified = true;
					shapeModified = true;
				}
				if (element.contains("auto_uv")) {
					vector<Triangle> tris;
					shape->GetTriangles(tris);
					bslsp->uvScale = auto_uv_scale(nif.GetUvsForShape(shape), nif.GetVertsForShape(shape), tris) / element["auto_uv"];
					modified = true;
					shapeModified = true;
				}
				if (element.contains("vertex_colors")) {
					shape->SetVertexColors(element["vertex_colors"]);
					if (element["vertex_colors"])
						bslsp->shaderFlags2 |= SLSF2_VERTEX_COLORS;
					else
						bslsp->shaderFlags2 &= ~SLSF2_VERTEX_COLORS;
					modified = true;
					shapeModified = true;
				}

				// texture scale values
				if (element.contains("specular_level")) {
					shader->SetGlossiness(element["specular_level"]);
					modified = true;
					shapeModified = true;
				}
				if (element.contains("subsurface_color") && element["subsurface_color"].size() > 2) {
					shader->SetSpecularColor(Vector3(element["subsurface_color"][0], element["subsurface_color"][1], element["subsurface_color"][2]));
					modified = true;
					shapeModified = true;
				}
				if (element.contains("roughness_scale")) {
					shader->SetSpecularStrength(element["roughness_scale"]);
					modified = true;
					shapeModified = true;
				}
				if (element.contains("subsurface_opacity")) {
					bslsp->softlighting = element["subsurface_opacity"];
					modified = true;
					shapeModified = true;
				}
				if (element.contains("displacement_scale")) {
					bslsp->rimlightPower = element["displacement_scale"];
					modified = true;
					shapeModified = true;
				}
				if (element.contains("env_mapping")) {
					if (element["env_mapping"]) {
						bslsp->bslspShaderType = BSLSP_ENVMAP;
						bslsp->shaderFlags1 |= SLSF1_ENVIRONMENT_MAPPING;
						bslsp->shaderFlags2 &= ~SLSF2_GLOW_MAP;
						modified = true;
					}
				}
				if (element.contains("env_map_scale") && bslsp->bslspShaderType == BSLSP_ENVMAP) {
					bslsp->environmentMapScale = element["env_map_scale"];
					modified = true;
				}
				if (element.contains("env_map_scale_mult") && bslsp->bslspShaderType == BSLSP_ENVMAP) {
					bslsp->environmentMapScale *= element["env_map_scale_mult"];
					modified = true;
				}
				if (element.contains("cubemap") && bslsp->bslspShaderType == BSLSP_ENVMAP && !flag(element, "lock_cubemap")) {
					string cubemap = element["cubemap"];
					nif.SetTextureSlot(shape, cubemap, 4);
					modified = true;
				}
				if (element.contains("emissive_scale")) {
					shader->SetEmissiveMultiple(element["emissive_scale"]);
					modified = true;
				}
				if (element.contains("emissive_color") && element["emissive_color"].size() > 3) {
					shader->SetEmissiveColor(Color4(element["emissive_color"][0], element["emissive_color"][1], element["emissive_color"][2], element["emissive_color"][3]));
					modified = true;
				}
				if (element.contains("uv_scale")) {
					bslsp->uvScale = Vector2(element["uv_scale"], element["uv_scale"]);
					modified = true;
				}
				if (element.contains("parallax_envmap_strength")) {
					bslsp->parallaxEnvmapStrength = element["parallax_envmap_strength"];
					modified = true;
				}

				if (element.contains("pbr") && !element["pbr"]) {
					bslsp->shaderFlags2 &= ~SLSF2_UNUSED01; // "PBR FLAG"
				}
				else if (name_match)
				{

					modified = true;

					string tex_path = string(matched_path);
					if (!tex_path.starts_with("textures\\pbr\\"))
						tex_path.insert(9, "pbr\\");

					if (element.contains("rename")) {
						string orig = element.contains("match_normal") ? element["match_normal"] : element["match_diffuse"];
						tex_path.erase(tex_path.length() - orig.length(), orig.length());
						tex_path.append(element["rename"]);
					}

					string empty_path = "";
					if (!flag(element, "lock_diffuse")) {
						auto diffuse = tex_path + ".dds";
						nif.SetTextureSlot(shape, diffuse, 0);
					}

					if (!flag(element, "lock_normal")) {
						auto normal = tex_path + "_n.dds";
						nif.SetTextureSlot(shape, normal, 1);
					}

					if (element.contains("emissive") && !flag(element, "lock_emissive"))
					{
						if (element["emissive"]) {
							auto glow = tex_path + "_g.dds";
							nif.SetTextureSlot(shape, glow, 2);
							bslsp->shaderFlags1 |= SLSF1_EXTERNAL_EMITTANCE;
						}
						else {
							nif.SetTextureSlot(shape, empty_path, 2);
							bslsp->shaderFlags1 &= ~SLSF1_EXTERNAL_EMITTANCE;
						}
					}

					if (element.contains("parallax") && !flag(element, "lock_parallax")) {
						if (element["parallax"]) {
							auto parallax = tex_path + "_p.dds";
							nif.SetTextureSlot(shape, parallax, 3);
						}
						else {
							nif.SetTextureSlot(shape, empty_path, 3);
						}
					}
					nif.SetTextureSlot(shape, empty_path, 4); // unused
					if (!flag(element, "lock_rmaos")) {
						auto rmaos = tex_path + "_rmaos.dds";
						nif.SetTextureSlot(shape, rmaos, 5);
					}
					if (!flag(element, "lock_cnr")) {
						if (element.contains("coat_normal") && element["coat_normal"]) { // coat normal roughness

							auto cnr = tex_path + "_cnr.dds";
							nif.SetTextureSlot(shape, cnr, 6);
						}
						else {
							nif.SetTextureSlot(shape, empty_path, 6);
						}
					}
					if (!flag(element, "lock_subsurface")) {
						if ((element.contains("subsurface_foliage") && element["subsurface_foliage"])
							|| (element.contains("subsurface") && element["subsurface"])
							|| (element.contains("coat_diffuse") && element["coat_diffuse"])
							) {
							auto subsurface = tex_path + "_s.dds";
							nif.SetTextureSlot(shape, subsurface, 7);
						}
						else {
							nif.SetTextureSlot(shape, empty_path, 7);
						}
					}


					// revert to default shader type, remove flags used in other types
					bslsp->bslspShaderType = BSLSP_DEFAULT;
					bslsp->shaderFlags1 &= ~SLSF1_ENVIRONMENT_MAPPING;
					bslsp->shaderFlags1 &= ~SLSF1_PARALLAX;
					bslsp->shaderFlags2 &= ~SLSF2_GLOW_MAP;
					bslsp->shaderFlags2 &= ~SLSF2_BACK_LIGHTING;
					bslsp->shaderFlags2 &= ~SLSF2_MULTI_LAYER_PARALLAX;

					bslsp->shaderFlags2 |= SLSF2_UNUSED01; // "PBR FLAG"
					// pbr shader switch
					if (element.contains("subsurface_foliage") && element["subsurface_foliage"] && element.contains("subsurface") && element["subsurface"]) {
						log_error("Subsurface and foliage shader chosen at once on shape '" + shapeName + "', undefined behavior!");
					}
					if (element.contains("subsurface_foliage")) {
						if (element["subsurface_foliage"]) {
							bslsp->shaderFlags2 |= SLSF2_SOFT_LIGHTING;
						}
						else {
							bslsp->shaderFlags2 &= ~SLSF2_SOFT_LIGHTING;
						}
					}
					if (element.contains("subsurface")) {
						if (element["subsurface"]) {
							bslsp->shaderFlags2 |= SLSF2_RIM_LIGHTING;
						}
						else {
							bslsp->shaderFlags2 &= ~SLSF2_RIM_LIGHTING;
						}
					}
					if (element.contains("multilayer") && element["multilayer"]) {
						bslsp->bslspShaderType = BSLSP_MULTILAYERPARALLAX;
						bslsp->shaderFlags2 |= SLSF2_MULTI_LAYER_PARALLAX;
						if (element.contains("coat_color") && element["coat_color"].size() > 2) {
							shader->SetSpecularColor(Vector3(element["coat_color"][0], element["coat_color"][1], element["coat_color"][2]));
						}
						if (element.contains("coat_specular_level")) {
							bslsp->parallaxRefractionScale = element["coat_specular_level"];
						}
						if (element.contains("coat_roughness")) {
							bslsp->parallaxInnerLayerThickness = element["coat_roughness"];
						}
						if (element.contains("coat_strength")) {
							bslsp->softlighting = element["coat_strength"];
						}
						if (element.contains("coat_diffuse")) {
							if (element["coat_diffuse"]) {
								bslsp->shaderFlags2 |= SLSF2_EFFECT_LIGHTING;
							}
							else {
								bslsp->shaderFlags2 &= ~SLSF2_EFFECT_LIGHTING;
							}
						}
						if (element.contains("coat_parallax")) {
							if (element["coat_parallax"]) {
								bslsp->shaderFlags2 |= SLSF2_SOFT_LIGHTING;
							}
							else {
								bslsp->shaderFlags2 &= ~SLSF2_SOFT_LIGHTING;
							}
						}
						if (element.contains("coat_normal")) {
							if (element["coat_normal"]) {
								bslsp->shaderFlags2 |= SLSF2_BACK_LIGHTING;
							}
							else {
								bslsp->shaderFlags2 &= ~SLSF2_BACK_LIGHTING;
							}
						}
						if (element.contains("inner_uv_scale")) {
							bslsp->parallaxInnerLayerTextureScale = Vector2(element["inner_uv_scale"], element["inner_uv_scale"]);
						}
					}
					shapeModified = true;
				}
				if (element.contains("slot1")) {
					string p = element["slot1"];
					nif.SetTextureSlot(shape, p, 0);
					modified = true;
					shapeModified = true;
				}
				if (element.contains("slot2")) {
					string p = element["slot2"];
					nif.SetTextureSlot(shape, p, 1);
					modified = true;
					shapeModified = true;
				}
				if (element.contains("slot3")) {
					string p = element["slot3"];
					nif.SetTextureSlot(shape, p, 2);
					modified = true;
					shapeModified = true;
				}
				if (element.contains("slot4")) {
					string p = element["slot4"];
					nif.SetTextureSlot(shape, p, 3);
					modified = true;
					shapeModified = true;
				}
				if (element.contains("slot5")) {
					string p = element["slot5"];
					nif.SetTextureSlot(shape, p, 4);
					modified = true;
					shapeModified = true;
				}
				if (element.contains("slot6")) {
					string p = element["slot6"];
					nif.SetTextureSlot(shape, p, 5);
					modified = true;
					shapeModified = true;
				}
				if (element.contains("slot7")) {
					string p = element["slot7"];
					nif.SetTextureSlot(shape, p, 6);
					modified = true;
					shapeModified = true;
				}
				if (element.contains("slot8")) {
					string p = element["slot8"];
					nif.SetTextureSlot(shape, p, 7);
					modified = true;
					shapeModified = true;
				}
			}
		if (shapeModified) {
			log_info("Shape '" + shapeName + "' updated while processing '" + filename + "'.");
		}
	}
	return modified;
}

int main(int argc, char* argv[])
{
	try
	{
		log_info("Starting PBR NIF patcher.");
		vector<json> js;
		if (!exists(".\\PBRNifPatcher")) {
			log_error("Config directory PBRNifPatcher does not exist at '" + absolute(".\\PBRNifPatcher").string() + "'.");
			getchar();
			return 1;
		}
		if (!exists(".\\meshes")) {
			log_error("Meshes directory '.\\\\meshes' not found at '" + absolute(".\\meshes").string() + "'.");
			getchar();
			return 1;
		}
		size_t configCount = 0;
		for (recursive_directory_iterator i(".\\PBRNifPatcher"), end; i != end; ++i) {
			if (!is_directory(i->path()) && i->path().extension().compare(".json") == 0) {
				configCount++;
				std::ifstream f(i->path());
				if (!f.is_open()) {
					log_error("Failed to open config file '" + i->path().string() + "'.");
					continue;
				}
				try {
					js.push_back(json::parse(f));
					log_info("Config " + i->path().filename().string() + " loaded.");
				}
				catch (json::parse_error& ex)
				{
					log_error("Json file '" + i->path().filename().string() + "' parse error at byte " + std::to_string(ex.byte) + ": " + ex.what());
					log_error("Error in configuration, quitting.");
					getchar();
					return 1;
				}
			}
		}

		auto item_count = 0;
		for (auto& j : js)
			for (auto& element : j) {
				item_count++;
				if (element.contains("texture")) {
					element["match_diffuse"] = element["texture"];
				}
				if (element.contains("match_normal"))
					element["match_normal"] = str_tolower(element["match_normal"]).insert(0, 1, '\\');
				if (element.contains("match_diffuse"))
					element["match_diffuse"] = str_tolower(element["match_diffuse"]).insert(0, 1, '\\');
				if (element.contains("rename"))
					element["rename"] = str_tolower(element["rename"]).insert(0, 1, '\\');
				if (element.contains("path_contains"))
					element["path_contains"] = str_tolower(element["path_contains"]);
				if (element.contains("nif_filter"))
					element["nif_filter"] = str_tolower(element["nif_filter"]);
			}
		log_info("Total config items found: " + std::to_string(item_count) + " across " + std::to_string(configCount) + " config file(s).");

		auto save_options = NifSaveOptions();
		save_options.optimize = false;
		save_options.sortBlocks = false;
		auto out_dir = ".\\pbr_output";
		size_t processedFiles = 0;
		size_t modifiedFiles = 0;
		size_t failedFiles = 0;
		log_info("Scanning meshes under '" + absolute(".\\meshes").string() + "'.");
		for (recursive_directory_iterator i(".\\meshes"), end; i != end; ++i) {
			if (i->path().string().starts_with(out_dir))
				continue;
			if (!is_directory(i->path()) && i->path().extension().compare(".nif") == 0) {
				processedFiles++;
				log_info("Processing " + i->path().string());
				NifFile nif;
				const auto loadResult = nif.Load(i->path());
				if (loadResult == 0) {
					auto fn = i->path().string();
					if (set_pbr_textures(nif, js, fn)) {
						log_info("Modified " + i->path().string());
						path out_path;
						out_path = path(out_dir) / path(i->path().lexically_normal());
						std::error_code ec;
						create_directories(out_path.parent_path(), ec);
						if (ec) {
							log_error("Failed to create directory '" + out_path.parent_path().string() + "': " + ec.message());
							failedFiles++;
							continue;
						}
						const auto saveResult = nif.Save(out_path, save_options);
						if (saveResult != 0) {
							log_error("Error saving " + out_path.string() + " (code " + std::to_string(saveResult) + ": " + describe_save_error(saveResult) + ").");
							failedFiles++;
						} else {
							log_info("Saved patched file to " + out_path.string());
							modifiedFiles++;
						}
					}
					else {
						log_info("No changes applied to " + i->path().string());
					}
				}
				else
				{
					log_error("Error opening " + i->path().string() + " (code " + std::to_string(loadResult) + ": " + describe_load_error(loadResult) + ").");
					failedFiles++;
				}
			}
		}
		log_info("Processing complete. Files scanned: " + std::to_string(processedFiles) + ", modified: " + std::to_string(modifiedFiles) + ", failures: " + std::to_string(failedFiles) + ".");
	}
	catch (const std::exception& exc)
	{
		log_error(std::string("EXCEPTION: ") + exc.what());
	}
	log_info("Finished!");
	getchar();
	return 0;
}
