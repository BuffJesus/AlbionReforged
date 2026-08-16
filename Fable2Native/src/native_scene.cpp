#include "f2/native_scene.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace f2 {
namespace {

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

template <typename T>
bool read_value(std::istringstream& line, T& value) {
    return static_cast<bool>(line >> value);
}

bool read_vec3(std::istringstream& line, std::array<float, 3>& value) {
    return read_value(line, value[0]) && read_value(line, value[1]) &&
           read_value(line, value[2]);
}

template <std::size_t N>
bool read_csv_floats(const std::string& text, std::array<float, N>& values) {
    std::istringstream input(text);
    std::string token;
    for (std::size_t i = 0; i < N; ++i) {
        if (!std::getline(input, token, ',')) return false;
        std::istringstream number(token);
        if (!(number >> values[i])) return false;
        number >> std::ws;
        if (!number.eof()) return false;
    }
    return !std::getline(input, token, ',');
}

}  // namespace

bool NativeScene::validate(std::string* error) const {
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        const NativeMesh& mesh = meshes[i];
        if (mesh.vertices.empty()) {
            return fail(error, "mesh " + std::to_string(i) + " has no vertices");
        }
        if (mesh.indices.empty() || mesh.indices.size() % 3 != 0) {
            return fail(error, "mesh " + std::to_string(i) + " has invalid triangle indices");
        }
        if (mesh.material >= materials.size()) {
            return fail(error, "mesh " + std::to_string(i) + " references an invalid material");
        }
        for (std::uint32_t index : mesh.indices) {
            if (index >= mesh.vertices.size()) {
                return fail(error, "mesh " + std::to_string(i) + " has an out-of-range index");
            }
        }
    }
    for (std::size_t i = 0; i < instances.size(); ++i) {
        if (instances[i].mesh >= meshes.size()) {
            return fail(error, "instance " + std::to_string(i) + " references an invalid mesh");
        }
        if (instances[i].scale <= 0.0f) {
            return fail(error, "instance " + std::to_string(i) + " has a non-positive scale");
        }
    }
    return true;
}

bool load_native_scene(const std::filesystem::path& path,
                       NativeScene& scene,
                       std::string& error) {
    std::ifstream input(path);
    if (!input) return fail(&error, "unable to open scene: " + path.string());

    NativeScene parsed;
    std::string raw;
    std::size_t line_number = 0;
    while (std::getline(input, raw)) {
        ++line_number;
        if (raw.empty() || raw[0] == '#') continue;
        std::istringstream line(raw);
        std::string opcode;
        line >> opcode;
        if (opcode == "F2SCENE") {
            int version = 0;
            if (!read_value(line, version) || version != 1) {
                return fail(&error, "unsupported F2SCENE version at line " +
                                      std::to_string(line_number));
            }
        } else if (opcode == "material") {
            NativeMaterial material;
            if (!read_value(line, material.name) ||
                !read_value(line, material.base_color[0]) ||
                !read_value(line, material.base_color[1]) ||
                !read_value(line, material.base_color[2]) ||
                !read_value(line, material.base_color[3])) {
                return fail(&error, "invalid material at line " + std::to_string(line_number));
            }
            // Texture paths are optional so older F2SCENE packages remain valid.
            // New cookers emit explicit key=value tokens to keep the text format
            // extensible without making empty fields positional.
            std::string option;
            while (line >> option) {
                const auto separator = option.find('=');
                if (separator == std::string::npos) {
                    return fail(&error, "invalid material option at line " +
                                          std::to_string(line_number));
                }
                const auto key = option.substr(0, separator);
                const auto value = option.substr(separator + 1);
                if (key == "albedo") material.albedo = value;
                else if (key == "normal") material.normal = value;
                else if (key == "material") material.material = value;
                else if (key == "water_params") {
                    if (!read_csv_floats(value, material.water_params)) {
                        return fail(&error, "invalid water_params at line " +
                                             std::to_string(line_number));
                    }
                    material.has_water_params = true;
                } else if (key == "water_opacity") {
                    std::istringstream number(value);
                    if (!(number >> material.water_opacity)) {
                        return fail(&error, "invalid water_opacity at line " +
                                             std::to_string(line_number));
                    }
                }
                else return fail(&error, "unknown material option '" + key + "' at line " +
                                 std::to_string(line_number));
            }
            parsed.materials.push_back(std::move(material));
        } else if (opcode == "mesh") {
            NativeMesh mesh;
            std::size_t vertex_count = 0;
            std::size_t index_count = 0;
            if (!read_value(line, mesh.name) || !read_value(line, vertex_count) ||
                !read_value(line, index_count) || !read_value(line, mesh.material)) {
                return fail(&error, "invalid mesh at line " + std::to_string(line_number));
            }
            mesh.vertices.reserve(vertex_count);
            mesh.indices.reserve(index_count);
            parsed.meshes.push_back(std::move(mesh));
        } else if (opcode == "vertex") {
            if (parsed.meshes.empty()) return fail(&error, "vertex before mesh");
            NativeVertex vertex;
            auto& position = vertex.position;
            auto& normal = vertex.normal;
            if (!read_vec3(line, position) || !read_vec3(line, normal) ||
                !read_value(line, vertex.uv[0]) || !read_value(line, vertex.uv[1])) {
                return fail(&error, "invalid vertex at line " + std::to_string(line_number));
            }
            parsed.meshes.back().vertices.push_back(vertex);
        } else if (opcode == "index") {
            if (parsed.meshes.empty()) return fail(&error, "index before mesh");
            std::uint32_t index = 0;
            if (!read_value(line, index)) {
                return fail(&error, "invalid index at line " + std::to_string(line_number));
            }
            parsed.meshes.back().indices.push_back(index);
        } else if (opcode == "instance") {
            NativeInstance instance;
            std::string mesh_name;
            if (!read_value(line, mesh_name) || !read_vec3(line, instance.position) ||
                !read_vec3(line, instance.rotation) || !read_value(line, instance.scale)) {
                return fail(&error, "invalid instance at line " + std::to_string(line_number));
            }
            bool found = false;
            for (std::size_t i = 0; i < parsed.meshes.size(); ++i) {
                if (parsed.meshes[i].name == mesh_name) {
                    instance.mesh = static_cast<std::uint32_t>(i);
                    found = true;
                    break;
                }
            }
            if (!found) return fail(&error, "instance references unknown mesh: " + mesh_name);
            // Optional trailing `sh <12 floats>` = per-instance baked order-1 SH probe (.lmp).
            std::string extra;
            if (read_value(line, extra) && extra == "sh") {
                for (float& coeff : instance.sh) {
                    if (!read_value(line, coeff)) {
                        return fail(&error, "invalid instance sh probe at line " +
                                    std::to_string(line_number));
                    }
                }
                instance.has_probe = true;
            }
            parsed.instances.push_back(instance);
        } else if (opcode == "sun") {
            if (!read_vec3(line, parsed.sun_direction)) {
                return fail(&error, "invalid sun at line " + std::to_string(line_number));
            }
        } else if (opcode == "sunlight") {
            if (!read_vec3(line, parsed.sun_color)) {
                return fail(&error, "invalid sunlight at line " + std::to_string(line_number));
            }
        } else if (opcode == "sky") {
            if (!read_value(line, parsed.sky_color[0]) ||
                !read_value(line, parsed.sky_color[1]) ||
                !read_value(line, parsed.sky_color[2]) ||
                !read_value(line, parsed.sky_color[3])) {
                return fail(&error, "invalid sky at line " + std::to_string(line_number));
            }
        } else if (opcode == "sky_horizon") {
            // Sky-gradient horizon tint (theme complementary_colour, display-mapped). Optional:
            // scenes without it keep the default hardcoded horizon (native_scene.h).
            if (!read_vec3(line, parsed.sky_horizon_color)) {
                return fail(&error, "invalid sky_horizon at line " + std::to_string(line_number));
            }
        } else if (opcode == "sky_sunset") {
            // Sky-gradient sunset tint (theme sunset_colour). Optional: enables the dawn/dusk
            // sun-halo term. Absent -> has_sky_sunset stays false -> the term is a no-op.
            if (!read_vec3(line, parsed.sky_sunset_color)) {
                return fail(&error, "invalid sky_sunset at line " + std::to_string(line_number));
            }
            parsed.has_sky_sunset = true;
        } else if (opcode == "sky_bias") {
            // Sky-gradient ramp bias (theme complementary_bias). Optional; 0 = linear (default).
            if (!read_value(line, parsed.sky_bias)) {
                return fail(&error, "invalid sky_bias at line " + std::to_string(line_number));
            }
        } else if (opcode == "sky_atmos") {
            // sky_atmos <sun_intensity> <rayleigh> <mie> — enables the analytic atmosphere sky.
            if (!read_value(line, parsed.sky_sun_intensity) ||
                !read_value(line, parsed.sky_rayleigh) ||
                !read_value(line, parsed.sky_mie)) {
                return fail(&error, "invalid sky_atmos at line " + std::to_string(line_number));
            }
        } else if (opcode == "fog_color") {
            if (!read_vec3(line, parsed.fog_color)) {
                return fail(&error, "invalid fog_color at line " + std::to_string(line_number));
            }
        } else if (opcode == "fog_range") {
            // fog_range <start> <end> <max> — linear distance fog; max>0 enables it.
            if (!read_value(line, parsed.fog_start) || !read_value(line, parsed.fog_end) ||
                !read_value(line, parsed.fog_max)) {
                return fail(&error, "invalid fog_range at line " + std::to_string(line_number));
            }
        } else if (opcode == "fog_curve") {
            // fog_curve <start> <inv_span2> <power> <amp> — grounded exponential distance fog.
            if (!read_value(line, parsed.fog_curve[0]) || !read_value(line, parsed.fog_curve[1]) ||
                !read_value(line, parsed.fog_curve[2]) || !read_value(line, parsed.fog_curve[3])) {
                return fail(&error, "invalid fog_curve at line " + std::to_string(line_number));
            }
            parsed.has_fog_curve = true;
        } else if (opcode == "ground_mist") {
            // ground_mist <strength> <depth_scale> <height_offset> <falloff> — height-based mist.
            if (!read_value(line, parsed.mist_strength) ||
                !read_value(line, parsed.mist_depth_scale) ||
                !read_value(line, parsed.mist_height_offset) ||
                !read_value(line, parsed.mist_falloff)) {
                return fail(&error, "invalid ground_mist at line " + std::to_string(line_number));
            }
            parsed.has_ground_mist = true;
        } else if (opcode == "ambient") {
            // ambient <r> <g> <b> — flat AmbientColour (theme Lighting sub-record). Optional; paired
            // with `sky_bounce`. Sets has_ambient so the world PS uses the theme ambient model
            // instead of its hardcoded hemisphere.
            if (!read_vec3(line, parsed.ambient_flat)) {
                return fail(&error, "invalid ambient at line " + std::to_string(line_number));
            }
            parsed.has_ambient = true;
        } else if (opcode == "sky_bounce") {
            // sky_bounce <topR topG topB> <botR botG botB> — the hemisphere sky-bounce gradient
            // (SkyColourFinalBounce Top/Bottom). Optional; scenes without it keep the hardcoded
            // hemisphere defaults (native_scene.h).
            if (!read_vec3(line, parsed.sky_bounce_top) ||
                !read_vec3(line, parsed.sky_bounce_bottom)) {
                return fail(&error, "invalid sky_bounce at line " + std::to_string(line_number));
            }
        } else if (opcode == "focus") {
            // focus <cx> <cy> <cz> <radius> — camera-fit bounds over the town geometry only
            // (excludes horizon backdrop props). The world renderers frame this instead of the
            // full vertex AABB.
            if (!read_vec3(line, parsed.focus_center) ||
                !read_value(line, parsed.focus_radius)) {
                return fail(&error, "invalid focus at line " + std::to_string(line_number));
            }
            parsed.has_focus = true;
        } else if (opcode == "hero_start") {
            // hero_start <px> <py> <pz> <yaw> — render-space PlayerStart for inspection framing.
            if (!read_vec3(line, parsed.hero_start) || !read_value(line, parsed.hero_yaw)) {
                return fail(&error, "invalid hero_start at line " + std::to_string(line_number));
            }
            parsed.has_hero_start = true;
        } else if (opcode == "light") {
            // light <px> <py> <pz> <r> <g> <b> <range> <intensity>
            // (render-space position, linear-ish colour 0..1, wu radius, brightness).
            NativeLight light;
            if (!read_vec3(line, light.position) || !read_vec3(line, light.color) ||
                !read_value(line, light.range) || !read_value(line, light.intensity)) {
                return fail(&error, "invalid light at line " + std::to_string(line_number));
            }
            parsed.lights.push_back(light);
        } else if (opcode == "cloud_globals") {
            // cloud_globals <global_brightness> <alpha_ref> (cloud_global.x/.z).
            if (!read_value(line, parsed.cloud_global_brightness) ||
                !read_value(line, parsed.cloud_alpha_ref)) {
                return fail(&error, "invalid cloud_globals at line " + std::to_string(line_number));
            }
        } else if (opcode == "cloud_layer") {
            // cloud_layer <density_dds> <height> <size_x> <size_y> <scale_x> <scale_y>
            //   <vel_x> <vel_y> <transparency> <brightness> <ambient> <normal_strength>
            NativeCloudLayer layer;
            if (!(line >> layer.density_map) || !read_value(line, layer.height) ||
                !read_value(line, layer.size_x) || !read_value(line, layer.size_y) ||
                !read_value(line, layer.texture_scale_x) ||
                !read_value(line, layer.texture_scale_y) ||
                !read_value(line, layer.velocity_x) || !read_value(line, layer.velocity_y) ||
                !read_value(line, layer.transparency) || !read_value(line, layer.brightness) ||
                !read_value(line, layer.ambient) || !read_value(line, layer.normal_strength)) {
                return fail(&error, "invalid cloud_layer at line " + std::to_string(line_number));
            }
            parsed.clouds.push_back(std::move(layer));
        } else if (opcode == "sky_moon") {
            // sky_moon <moon_dds> <glare_dds|-> <dir.x> <dir.y> <dir.z> <intensity> <size>
            //   <transparency> <glare_intensity> <glare_size> <exposure> <phase>
            NativeMoon moon;
            std::string glare;
            if (!(line >> moon.texture) || !(line >> glare) ||
                !read_vec3(line, moon.direction) || !read_value(line, moon.intensity) ||
                !read_value(line, moon.size) || !read_value(line, moon.transparency) ||
                !read_value(line, moon.glare_intensity) || !read_value(line, moon.glare_size) ||
                !read_value(line, moon.exposure) || !read_value(line, moon.phase)) {
                return fail(&error, "invalid sky_moon at line " + std::to_string(line_number));
            }
            if (glare != "-") moon.glare_texture = glare;
            parsed.moon = std::move(moon);
            parsed.has_moon = true;
        } else if (opcode == "sky_stars") {
            if (!read_value(line, parsed.star_brightness)) {
                return fail(&error, "invalid sky_stars at line " + std::to_string(line_number));
            }
        } else {
            return fail(&error, "unknown opcode '" + opcode + "' at line " +
                                  std::to_string(line_number));
        }
    }

    if (!parsed.validate(&error)) return false;
    scene = std::move(parsed);
    return true;
}

bool save_native_scene(const std::filesystem::path& path,
                       const NativeScene& scene,
                       std::string& error) {
    if (!scene.validate(&error)) return false;
    std::ofstream output(path);
    if (!output) return fail(&error, "unable to open scene for writing: " + path.string());
    output << std::setprecision(9);  // round-trips 32-bit floats without loss
    output << "F2SCENE 1\n";
    output << "sun " << scene.sun_direction[0] << ' ' << scene.sun_direction[1] << ' '
           << scene.sun_direction[2] << '\n';
    output << "sunlight " << scene.sun_color[0] << ' ' << scene.sun_color[1] << ' '
           << scene.sun_color[2] << '\n';
    output << "sky " << scene.sky_color[0] << ' ' << scene.sky_color[1] << ' ' << scene.sky_color[2]
           << ' ' << scene.sky_color[3] << '\n';
    if (scene.has_hero_start) {
        output << "hero_start " << scene.hero_start[0] << ' ' << scene.hero_start[1] << ' '
               << scene.hero_start[2] << ' ' << scene.hero_yaw << '\n';
    }
    for (const NativeMaterial& material : scene.materials) {
        output << "material " << material.name << ' ' << material.base_color[0] << ' '
               << material.base_color[1] << ' ' << material.base_color[2] << ' '
               << material.base_color[3];
        if (!material.albedo.empty()) output << " albedo=" << material.albedo;
        if (!material.normal.empty()) output << " normal=" << material.normal;
        if (!material.material.empty()) output << " material=" << material.material;
        if (material.has_water_params) {
            output << " water_params=";
            for (std::size_t i = 0; i < material.water_params.size(); ++i) {
                if (i) output << ',';
                output << material.water_params[i];
            }
            output << " water_opacity=" << material.water_opacity;
        }
        output << '\n';
    }
    for (const NativeMesh& mesh : scene.meshes) {
        output << "mesh " << mesh.name << ' ' << mesh.vertices.size() << ' ' << mesh.indices.size()
               << ' ' << mesh.material << '\n';
        for (const NativeVertex& vertex : mesh.vertices) {
            output << "vertex " << vertex.position[0] << ' ' << vertex.position[1] << ' '
                   << vertex.position[2] << ' ' << vertex.normal[0] << ' ' << vertex.normal[1] << ' '
                   << vertex.normal[2] << ' ' << vertex.uv[0] << ' ' << vertex.uv[1] << '\n';
        }
        for (std::uint32_t index : mesh.indices) output << "index " << index << '\n';
    }
    for (const NativeInstance& instance : scene.instances) {
        output << "instance " << scene.meshes[instance.mesh].name << ' ' << instance.position[0]
               << ' ' << instance.position[1] << ' ' << instance.position[2] << ' '
               << instance.rotation[0] << ' ' << instance.rotation[1] << ' ' << instance.rotation[2]
               << ' ' << instance.scale;
        if (instance.has_probe) {
            output << " sh";
            for (float coeff : instance.sh) output << ' ' << coeff;
        }
        output << '\n';
    }
    for (const NativeLight& light : scene.lights) {
        output << "light " << light.position[0] << ' ' << light.position[1] << ' '
               << light.position[2] << ' ' << light.color[0] << ' ' << light.color[1] << ' '
               << light.color[2] << ' ' << light.range << ' ' << light.intensity << '\n';
    }
    if (!scene.clouds.empty()) {
        output << "cloud_globals " << scene.cloud_global_brightness << ' '
               << scene.cloud_alpha_ref << '\n';
        for (const NativeCloudLayer& c : scene.clouds) {
            output << "cloud_layer " << c.density_map << ' ' << c.height << ' ' << c.size_x << ' '
                   << c.size_y << ' ' << c.texture_scale_x << ' ' << c.texture_scale_y << ' '
                   << c.velocity_x << ' ' << c.velocity_y << ' ' << c.transparency << ' '
                   << c.brightness << ' ' << c.ambient << ' ' << c.normal_strength << '\n';
        }
    }
    if (scene.has_moon) {
        const NativeMoon& m = scene.moon;
        output << "sky_moon " << m.texture << ' '
               << (m.glare_texture.empty() ? "-" : m.glare_texture) << ' ' << m.direction[0] << ' '
               << m.direction[1] << ' ' << m.direction[2] << ' ' << m.intensity << ' ' << m.size
               << ' ' << m.transparency << ' ' << m.glare_intensity << ' ' << m.glare_size << ' '
               << m.exposure << ' ' << m.phase << '\n';
    }
    if (scene.star_brightness > 0.0f) output << "sky_stars " << scene.star_brightness << '\n';
    return static_cast<bool>(output);
}

}  // namespace f2
