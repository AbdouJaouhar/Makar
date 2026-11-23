"""
Builds the materials of the procedural flora and rocks (AOceanIslandFlora, Source/Makar/.../OceanFlora.cpp):
/Game/Ocean/Flora/M_Bark, M_Foliage, M_Rock, M_PalmBark, and the leaf cards textured with Infinigen renders
(Saved/Infinigen, fetched by Scripts/infinigen/fetch_exports.ps1): M_PalmFrond, M_LeafCards_Broadleaf and
M_LeafCards_SeaGrape. Colours come from the generated vertex colours.

Run before create_ocean_map.py, from the editor (Tools > Execute Python Script...) or headless:
    UnrealEditor-Cmd.exe Makar.uproject -run=pythonscript -script=Scripts/create_flora_materials.py
"""

import os

import unreal

DIRECTORY = "/Game/Ocean/Flora"
MEL = unreal.MaterialEditingLibrary


def fresh_material(name):
    path = f"{DIRECTORY}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, DIRECTORY, unreal.Material, unreal.MaterialFactoryNew())
    # Set the usages up front: the flora is drawn by instancers (and the near trees by static meshes); without the flag
    # the editor sets it while rendering and shows the default material until the shaders are rebuilt.
    material.set_editor_property("used_with_instanced_static_meshes", True)
    return material


def node(material, cls, x, y):
    return MEL.create_material_expression(material, cls, x, y)


def constant(material, value, x, y):
    expression = node(material, unreal.MaterialExpressionConstant, x, y)
    expression.set_editor_property("r", value)
    return expression


def connect(source, target, input_name, source_output=""):
    if not MEL.connect_material_expressions(source, source_output, target, input_name):
        raise RuntimeError(f"Could not connect {source.get_name()} -> {target.get_name()}.{input_name}")


# Wind (Shaders/Flora.ush): every plant part bends with the same law, so leaves stay on their twigs; WIND_BEND in cm
# per square metre of height above the plant's foot (a 10 m palm sways ~35 cm), flutter in cm per metre from its axis.
WIND_BEND = 0.35  # trade wind ~25 km/h: a 10 m palm sways ~35 cm (0.2 was barely visible)


def add_wind(material, flutter):
    wind = node(material, unreal.MaterialExpressionCustom, -600, 900)
    wind.set_editor_property("description", "Flora Wind")
    wind.set_editor_property("include_file_paths", ["/Makar/Flora.ush"])
    wind.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    wind.set_editor_property("code", f"return FloraWind(WorldPos, Pivot, Time, {WIND_BEND}, {flutter});")
    inputs = []
    for input_name in ("WorldPos", "Pivot", "Time"):
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", input_name)
        inputs.append(custom_input)
    wind.set_editor_property("inputs", inputs)
    connect(node(material, unreal.MaterialExpressionWorldPosition, -900, 900), wind, "WorldPos")
    connect(node(material, unreal.MaterialExpressionObjectPositionWS, -900, 980), wind, "Pivot")
    connect(node(material, unreal.MaterialExpressionTime, -900, 1060), wind, "Time")
    MEL.connect_material_property(wind, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    # Bounds: the crowns move up to a few decimetres.
    material.set_editor_property("max_world_position_offset_displacement", 120.0)


def save(material, name, flutter=None):
    """Saves the material; flutter (cm per metre from the plant's axis, 0 for bark) adds the wind."""
    if flutter is not None:
        add_wind(material, flutter)
    MEL.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(f"{DIRECTORY}/{name}")


def bark():
    material = fresh_material("M_Bark")
    material.set_editor_property("two_sided", True)
    color = node(material, unreal.MaterialExpressionVertexColor, -400, 0)
    MEL.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(constant(material, 0.9, -400, 200), "", unreal.MaterialProperty.MP_ROUGHNESS)
    save(material, "M_Bark", flutter=0.0)


def foliage(name="M_Foliage", tint=(0.35, 0.5, 0.1), roughness=0.6, flutter=0.8):
    """Leaves let light through: two-sided foliage shading, the transmitted light a warmer, yellower green.
    M_StemF (the generated stems, trunks and coconuts) is the same with next to no light through. Built from scratch:
    a duplicate of M_Foliage drew the vertex colours white. Roughness 0.6: tropical leaves are waxy, a little of the sky shows on
    them (matte, the crowns read as flat lime green)."""
    material = fresh_material(name)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    color = node(material, unreal.MaterialExpressionVertexColor, -600, 0)
    MEL.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    transmission = node(material, unreal.MaterialExpressionConstant3Vector, -600, 200)
    # Light through the leaves: a darker, yellower green (a bright tint makes the crowns look pale and plastic).
    transmission.set_editor_property("constant", unreal.LinearColor(tint[0], tint[1], tint[2], 1.0))
    transmitted = node(material, unreal.MaterialExpressionMultiply, -300, 150)
    connect(color, transmitted, "A")
    connect(transmission, transmitted, "B")
    MEL.connect_material_property(transmitted, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    MEL.connect_material_property(constant(material, roughness, -300, 300), "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(constant(material, 0.3, -300, 380), "", unreal.MaterialProperty.MP_SPECULAR)
    save(material, name, flutter=flutter)




LEAF_BLADE_CODE = """
// One leaf on a quad (OceanFlora::AddLeafSpray): U across (0.5 = midrib), V = 1 at the stalk, 0 at the tip.
const float Along = 1.0 - UV.y;           // 0 at the stalk, 1 at the tip
const float Across = abs(UV.x - 0.5) * 2.0; // 0 on the midrib, 1 at the quad's edge
// Outline: a short stalk, then an elliptic blade rounded at its base, pointed at its tip, widest a little before the
// middle (sea grape and almond leaves are broader towards the tip: the vertex colour's species does not change this).
const float S = saturate((Along - 0.07) / 0.93);
const float Blade = Along > 0.07 ? pow(sin(3.14159 * pow(S, 0.8)), 0.6) : 0.0;
const float Width = max(Blade, Along < 0.1 ? 0.06 : 0.0);
const float Inside = Width - Across;
Opacity = saturate(Inside / max(fwidth(Inside), 1e-4) + 0.5);
// Veins: the midrib and secondary veins arching from it towards the margin, a little lighter; faded where they would
// alias (fwidth: vein periods per pixel).
const float Relative = Across / max(Width, 1e-3);
const float Midrib = 1.0 - smoothstep(0.015, 0.045, Across);
const float Phase = Along * 10.0 - pow(Relative, 0.7) * 1.6;
const float VeinWidth = max(0.06, fwidth(Phase));
const float Vein = (1.0 - smoothstep(0.0, VeinWidth, abs(frac(Phase) - 0.5) - 0.44)) * smoothstep(0.08, 0.2, Relative)
    * (1.0 - smoothstep(0.8, 0.97, Relative)) * saturate(1.5 - 6.0 * fwidth(Phase));
// The blade is a little paler towards its margin and along the veins; the midrib paler still, yellow-green.
float3 C = Color.rgb * lerp(0.9, 1.06, Relative) * (1.0 + 0.1 * Vein);
C = lerp(C, Color.rgb * float3(1.6, 1.5, 1.0), 0.6 * Midrib);
return C;
"""


def leaf_blade():
    """Real leaves of the trees and shrubs up close (OceanFlora::AddLeafSpray): each a curved quad whose outline and
    veins are drawn here from its UVs, cut out by the opacity mask, so the leaves have smooth outlines at any distance
    (modelled blades showed their polygons). Vertex colour = linear albedo; two-sided foliage, waxy."""
    material = fresh_material("M_LeafBlade")
    material.set_editor_property("two_sided", True)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property("opacity_mask_clip_value", 0.5)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    uv = node(material, unreal.MaterialExpressionTextureCoordinate, -900, 0)
    vertex = node(material, unreal.MaterialExpressionVertexColor, -900, 150)
    blade = node(material, unreal.MaterialExpressionCustom, -600, 0)
    blade.set_editor_property("description", "Leaf Blade")
    blade.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    blade.set_editor_property("code", LEAF_BLADE_CODE)
    inputs = []
    for name in ("UV", "Color"):
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        inputs.append(custom_input)
    blade.set_editor_property("inputs", inputs)
    opacity = unreal.CustomOutput()
    opacity.set_editor_property("output_name", "Opacity")
    opacity.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    blade.set_editor_property("additional_outputs", [opacity])
    connect(uv, blade, "UV")
    connect(vertex, blade, "Color")
    MEL.connect_material_property(blade, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(blade, "Opacity", unreal.MaterialProperty.MP_OPACITY_MASK)
    # Vertex alpha = occlusion inside the leaf mass (OceanFlora::AddLeafSpray): dims the sky light and its reflection.
    MEL.connect_material_property(vertex, "A", unreal.MaterialProperty.MP_AMBIENT_OCCLUSION)
    transmission = node(material, unreal.MaterialExpressionConstant3Vector, -600, 250)
    transmission.set_editor_property("constant", unreal.LinearColor(0.4, 0.55, 0.12, 1.0))
    transmitted = node(material, unreal.MaterialExpressionMultiply, -300, 250)
    connect(blade, transmitted, "A")
    connect(transmission, transmitted, "B")
    MEL.connect_material_property(transmitted, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    MEL.connect_material_property(constant(material, 0.65, -300, 350), "", unreal.MaterialProperty.MP_ROUGHNESS)
    # Specular fades out with the occlusion (UE's Fresnel term scales with Specular too): leaves inside the mass, in the
    # shade, mirrored the open sky at grazing angles and the bushes looked frosted white.
    occluded = node(material, unreal.MaterialExpressionMultiply, -450, 420)
    connect(vertex, occluded, "A", "A")
    connect(vertex, occluded, "B", "A")
    specular = node(material, unreal.MaterialExpressionMultiply, -300, 420)
    connect(occluded, specular, "A")
    connect(constant(material, 0.35, -450, 500), specular, "B")
    MEL.connect_material_property(specular, "", unreal.MaterialProperty.MP_SPECULAR)
    save(material, "M_LeafBlade", flutter=1.2)


def rock():
    """Caribbean stones (OceanFlora::MakeCaribbeanRock): the vertex colour of each kind (limestone, ironshore, beachrock,
    andesite, coral rubble) over the terrain's limestone tile in triplanar projection (the same texture as the cliffs,
    so loose blocks match the rock they fell from), with its normals for the small relief. Near sea level the black
    band of the spray zone and the wet sheen; under water a film of algae."""
    material = fresh_material("M_Rock")
    material.set_editor_property("tangent_space_normal", False)  # the custom node returns a world normal
    color = node(material, unreal.MaterialExpressionVertexColor, -900, 0)
    world_pos = node(material, unreal.MaterialExpressionWorldPosition, -900, 120)
    vertex_normal = node(material, unreal.MaterialExpressionVertexNormalWS, -900, 200)
    albedo_tex = node(material, unreal.MaterialExpressionTextureObject, -900, 280)
    albedo_tex.set_editor_property("texture", unreal.load_asset("/Game/Ocean/Ground/T_Ground_Limestone_BaseColor"))
    normal_tex = node(material, unreal.MaterialExpressionTextureObject, -900, 360)
    normal_tex.set_editor_property("texture", unreal.load_asset("/Game/Ocean/Ground/T_Ground_Limestone_Normal"))
    normal_tex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    custom = node(material, unreal.MaterialExpressionCustom, -500, 100)
    custom.set_editor_property("description", "Rock Surface")
    custom.set_editor_property("include_file_paths", ["/Makar/Ocean.ush"])
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property("code", (
        "const float3 P = WorldPos * 0.01;"
        # Procedural, triplanar: mottling at two scales and dark pits (the sampled limestone tile read as black here);
        # Perlin noise: the value terms of the other noises drew a grid of cells on flat tops.
        "float3 W = pow(abs(VertexNormal), 4.0); W /= W.x + W.y + W.z;"
        "const float3 Q = P * 2.5;"
        "const float Mottle = W.x * OceanPerlinNoise(Q.yz) + W.y * OceanPerlinNoise(Q.xz + 7.0) + W.z * OceanPerlinNoise(Q.xy + 13.0);"
        "const float3 F = P * 11.0;"
        "const float Grain = W.x * OceanPerlinNoise(F.yz) + W.y * OceanPerlinNoise(F.xz + 3.0) + W.z * OceanPerlinNoise(F.xy + 5.0);"
        "const float3 G = P * 26.0;"
        "const float Pits = W.x * OceanPerlinNoise(G.yz) + W.y * OceanPerlinNoise(G.xz + 9.0) + W.z * OceanPerlinNoise(G.xy + 11.0);"
        "float3 Base = Color.rgb * (0.7 + 0.4 * Mottle) * (0.85 + 0.3 * Grain) * (1.0 - 0.3 * smoothstep(0.68, 0.84, Pits));"
        "const float Spray = (1.0 - smoothstep(0.4, 1.4, P.z)) * smoothstep(-0.3, 0.1, P.z);"
        "Base *= lerp(1.0, 0.6, Spray);"
        "const float Under = 1.0 - smoothstep(-0.4, -0.05, P.z);"
        "Base *= lerp(float3(1.0, 1.0, 1.0), float3(0.55, 0.7, 0.45), Under * 0.8);"
        "Normal = VertexNormal;"
        "Roughness = lerp(0.88, 0.45, max(Spray * (1.0 - smoothstep(0.1, 0.6, P.z)), Under));"
        "return Base;"))
    inputs = []
    for name in ("Color", "WorldPos", "VertexNormal", "AlbedoTex", "NormalTex"):
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        inputs.append(custom_input)
    custom.set_editor_property("inputs", inputs)
    outputs = []
    for name, kind in (("Normal", unreal.CustomMaterialOutputType.CMOT_FLOAT3), ("Roughness", unreal.CustomMaterialOutputType.CMOT_FLOAT1)):
        output = unreal.CustomOutput()
        output.set_editor_property("output_name", name)
        output.set_editor_property("output_type", kind)
        outputs.append(output)
    custom.set_editor_property("additional_outputs", outputs)
    connect(color, custom, "Color")
    connect(world_pos, custom, "WorldPos")
    connect(vertex_normal, custom, "VertexNormal")
    connect(albedo_tex, custom, "AlbedoTex")
    connect(normal_tex, custom, "NormalTex")
    MEL.connect_material_property(custom, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(custom, "Normal", unreal.MaterialProperty.MP_NORMAL)
    MEL.connect_material_property(custom, "Roughness", unreal.MaterialProperty.MP_ROUGHNESS)
    save(material, "M_Rock")


FROND_SOURCE = os.path.join(unreal.Paths.project_saved_dir(), "Infinigen", "frond")


def import_texture(path, name, normal_map, alpha_coverage=None):
    # Already imported from this very file: keep the asset (re-importing rewrites it, and git sees a change).
    stamp = f"{os.path.getmtime(path):.0f}:{os.path.getsize(path)}"
    asset_path = f"{DIRECTORY}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        existing = unreal.EditorAssetLibrary.load_asset(asset_path)
        if unreal.EditorAssetLibrary.get_metadata_tag(existing, "SourceStamp") == stamp:
            return existing
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", path)
    task.set_editor_property("destination_path", DIRECTORY)
    task.set_editor_property("destination_name", name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture = unreal.EditorAssetLibrary.load_asset(f"{DIRECTORY}/{name}")
    if normal_map:
        texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
        texture.set_editor_property("srgb", False)
        # The frond was rendered with its tip up the image (+Y), Unreal's normal maps point green down (+V).
        texture.set_editor_property("flip_green_channel", True)
    elif alpha_coverage is not None:
        # Cut-out foliage: keep the alpha-tested coverage through the mips, or distant leaves thin out and vanish.
        texture.set_editor_property("do_scale_mips_for_alpha_coverage", True)
        texture.set_editor_property("alpha_coverage_thresholds", unreal.Vector4(0.0, 0.0, 0.0, alpha_coverage))
    unreal.EditorAssetLibrary.set_metadata_tag(texture, "SourceStamp", stamp)
    unreal.EditorAssetLibrary.save_asset(f"{DIRECTORY}/{name}")
    return texture


def palm_frond():
    """Coconut palm leaf cards: a frond rendered from Infinigen's coconut palm (Scripts/infinigen/
    render_frond_atlas.py), cut out by its alpha, lit as two-sided foliage. The vertex colour tints it: young
    fronds keep the rendered green, old drooping ones yellow."""
    if not os.path.isfile(os.path.join(FROND_SOURCE, "frond_basecolor.png")):
        unreal.log_warning(f"No frond textures in {FROND_SOURCE}: M_PalmFrond not built (palms fall back to M_Foliage)")
        return
    color_texture = import_texture(os.path.join(FROND_SOURCE, "frond_basecolor.png"), "T_PalmFrond_BaseColor", False)
    normal_texture = import_texture(os.path.join(FROND_SOURCE, "frond_normal.png"), "T_PalmFrond_Normal", True)

    material = fresh_material("M_PalmFrond")
    material.set_editor_property("two_sided", True)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property("opacity_mask_clip_value", 0.4)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)

    color = node(material, unreal.MaterialExpressionTextureSample, -900, 0)
    color.set_editor_property("texture", color_texture)
    normal = node(material, unreal.MaterialExpressionTextureSample, -900, 400)
    normal.set_editor_property("texture", normal_texture)
    normal.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)

    # Less saturated than the render (sunlit coconut fronds are a yellowish green), then tinted by age: vertex
    # colour over the young frond green.
    desaturate = node(material, unreal.MaterialExpressionDesaturation, -650, 0)
    connect(color, desaturate, "", "RGB")  # first input, unnamed
    connect(constant(material, 0.5, -800, 120), desaturate, "Fraction")
    vertex = node(material, unreal.MaterialExpressionVertexColor, -900, 250)
    young = node(material, unreal.MaterialExpressionConstant3Vector, -900, 330)
    young.set_editor_property("constant", unreal.LinearColor(0.12, 0.2, 0.04, 1.0))
    ratio = node(material, unreal.MaterialExpressionDivide, -650, 250)
    connect(vertex, ratio, "A")
    connect(young, ratio, "B")
    tint = node(material, unreal.MaterialExpressionClamp, -500, 250)
    connect(ratio, tint, "")
    tint.set_editor_property("max_default", 1.6)
    base = node(material, unreal.MaterialExpressionMultiply, -350, 100)
    connect(desaturate, base, "A")
    connect(tint, base, "B")
    MEL.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(color, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
    MEL.connect_material_property(normal, "RGB", unreal.MaterialProperty.MP_NORMAL)

    # Light through the leaflets: a warmer, yellower green.
    transmit = node(material, unreal.MaterialExpressionConstant3Vector, -500, 450)
    transmit.set_editor_property("constant", unreal.LinearColor(0.45, 0.55, 0.18, 1.0))
    transmitted = node(material, unreal.MaterialExpressionMultiply, -250, 400)
    connect(base, transmitted, "A")
    connect(transmit, transmitted, "B")
    MEL.connect_material_property(transmitted, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    MEL.connect_material_property(constant(material, 0.55, -250, 550), "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(constant(material, 0.4, -250, 620), "", unreal.MaterialProperty.MP_SPECULAR)
    save(material, "M_PalmFrond")


PALM_BARK_CODE = """
// Bark coordinates from OceanFlora's AddMappedTube: U once around the trunk (0-1), V metres up from the base.
const float U = UV.x;
const float V = UV.y;
// Noise seamless around the trunk: the samples at U and at U - 1 blended by U agree at U = 0 and U = 1.
#define PALM_BARK_NOISE(F, G, O) lerp(OceanValueNoise(float2(U * (F), V * (G)) + (O)), OceanValueNoise(float2((U - 1.0) * (F), V * (G)) + (O)), U)
// Ring scars left by the fallen fronds, every ~9 cm but irregularly spaced (growth varies from year to year),
// slightly wavy and of uneven depth around the trunk; worn almost smooth on the weathered lower trunk.
const float C = V / 0.09 + 4.0 * OceanValueNoise(float2(3.3, V * 0.5)) + 0.3 * (PALM_BARK_NOISE(4.0, 0.6, float2(0.0, 0.0)) - 0.5);
const float Ring = floor(C);
const float Strength = (0.35 + 0.65 * PALM_BARK_NOISE(3.0, 0.0, float2(Ring * 1.37, Ring * 0.71))) * lerp(0.3, 1.0, smoothstep(0.3, 2.5, V));
const float D = (frac(C) - 0.5) / 0.1;
const float Scar = exp(-D * D) * Strength;
// The scars fade to their average before they alias (fwidth: rings per pixel).
const float Detail = saturate(2.0 - 5.0 * fwidth(C));
// Fine vertical fissures, long and narrow, also faded with distance.
const float Fissure = smoothstep(0.68, 0.9, PALM_BARK_NOISE(70.0, 1.3, float2(5.1, 1.7))) * saturate(2.0 - 5.0 * fwidth(U * 70.0));
// Sun-bleached grey patches over browner bark.
const float Weather = 0.65 * PALM_BARK_NOISE(5.0, 0.9, float2(9.3, 4.1)) + 0.35 * PALM_BARK_NOISE(17.0, 3.0, float2(2.7, 8.8));
#undef PALM_BARK_NOISE
float3 Bark = lerp(float3(0.15, 0.135, 0.115), float3(0.28, 0.27, 0.255), smoothstep(0.3, 0.75, Weather)); // grey bark ~0.2-0.3 (0.4 read white)
Bark *= lerp(1.0, 0.45, lerp(0.13, Scar, Detail)) * lerp(1.0, 0.6, Fissure);
// Darker and sand-stained at the foot.
Bark = lerp(Bark * float3(0.85, 0.8, 0.7) + float3(0.04, 0.035, 0.025), Bark, smoothstep(0.0, 0.7, V));
// Each scar is a shallow groove, 2.5 mm deep: tangent-space slope along V (Y = +V).
const float Slope = 0.0025 * Strength * 2.0 * D * exp(-D * D) / (0.1 * 0.09) * Detail;
Normal = normalize(float3(0.0, -Slope, 1.0));
Roughness = lerp(0.8, 0.95, Scar * Detail);
return Bark;
"""


def palm_bark():
    """Coconut palm trunk, procedural: sun-bleached grey-brown bark ringed every ~9 cm by the scars of fallen fronds,
    with fine vertical fissures. Infinigen builds its trunk inside geometry nodes, together with the crown: there
    is no trunk mesh to render, so the bark is drawn here from the bark coordinates of OceanFlora's AddMappedTube."""
    material = fresh_material("M_PalmBark")
    uv = node(material, unreal.MaterialExpressionTextureCoordinate, -900, 0)
    bark_node = node(material, unreal.MaterialExpressionCustom, -600, 0)
    bark_node.set_editor_property("description", "Palm Bark")
    bark_node.set_editor_property("include_file_paths", ["/Makar/Ocean.ush"])
    bark_node.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    bark_node.set_editor_property("code", PALM_BARK_CODE)
    custom_input = unreal.CustomInput()
    custom_input.set_editor_property("input_name", "UV")
    bark_node.set_editor_property("inputs", [custom_input])
    outputs = []
    for name, kind in (("Normal", unreal.CustomMaterialOutputType.CMOT_FLOAT3),
                       ("Roughness", unreal.CustomMaterialOutputType.CMOT_FLOAT1)):
        custom_output = unreal.CustomOutput()
        custom_output.set_editor_property("output_name", name)
        custom_output.set_editor_property("output_type", kind)
        outputs.append(custom_output)
    bark_node.set_editor_property("additional_outputs", outputs)
    connect(uv, bark_node, "UV")
    MEL.connect_material_property(bark_node, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(bark_node, "Normal", unreal.MaterialProperty.MP_NORMAL)
    MEL.connect_material_property(bark_node, "Roughness", unreal.MaterialProperty.MP_ROUGHNESS)
    save(material, "M_PalmBark", flutter=0.0)


LEAF_SOURCE = os.path.join(unreal.Paths.project_saved_dir(), "Infinigen", "leaves")


def leaf_cards(kind, title, roughness, specular):
    """Leaf cards of the broadleaf trees and sea grapes (OceanFlora's AddLeafCards): a cluster of Infinigen leaves
    rendered from above (Scripts/infinigen/render_leaf_cluster.py), cut out by its alpha, lit as foliage. One-sided:
    the cards are built front and back, both with normals bent outwards from the leaf mass. The vertex colour
    multiplies the render (0.5 = as rendered)."""
    name = f"M_LeafCards_{title}"
    color_path = os.path.join(LEAF_SOURCE, f"{kind}_basecolor.png")
    if not os.path.isfile(color_path):
        unreal.log_warning(f"No {kind} leaf cluster in {LEAF_SOURCE}: {name} not built (falls back to M_Foliage)")
        return
    color_texture = import_texture(color_path, f"T_Leaves{title}_BaseColor", False, alpha_coverage=0.4)
    normal_texture = import_texture(os.path.join(LEAF_SOURCE, f"{kind}_normal.png"), f"T_Leaves{title}_Normal", True)

    material = fresh_material(name)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property("opacity_mask_clip_value", 0.4)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    color = node(material, unreal.MaterialExpressionTextureSample, -900, 0)
    color.set_editor_property("texture", color_texture)
    normal = node(material, unreal.MaterialExpressionTextureSample, -900, 400)
    normal.set_editor_property("texture", normal_texture)
    normal.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    vertex = node(material, unreal.MaterialExpressionVertexColor, -900, 250)
    tinted = node(material, unreal.MaterialExpressionMultiply, -600, 100)
    connect(color, tinted, "A", "RGB")
    connect(vertex, tinted, "B")
    base = node(material, unreal.MaterialExpressionMultiply, -400, 100)
    connect(tinted, base, "A")
    connect(constant(material, 2.0, -600, 220), base, "B")
    MEL.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(color, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
    MEL.connect_material_property(normal, "RGB", unreal.MaterialProperty.MP_NORMAL)
    # Light through the leaves: a warmer, yellower green (muted, as for the palm fronds).
    transmit = node(material, unreal.MaterialExpressionConstant3Vector, -500, 450)
    transmit.set_editor_property("constant", unreal.LinearColor(0.45, 0.55, 0.18, 1.0))
    transmitted = node(material, unreal.MaterialExpressionMultiply, -250, 400)
    connect(base, transmitted, "A")
    connect(transmit, transmitted, "B")
    MEL.connect_material_property(transmitted, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    MEL.connect_material_property(constant(material, roughness, -250, 550), "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(constant(material, specular, -250, 620), "", unreal.MaterialProperty.MP_SPECULAR)
    save(material, name, flutter=0.5)


TUFT_SOURCE = os.path.join(unreal.Paths.project_saved_dir(), "Infinigen", "tuft")


def grass_tuft():
    """Grass cards (OceanFlora::MakeGrassTuft): a tuft of Infinigen grass seen from the side (Scripts/infinigen/
    render_grass_tuft.py), cut out by its alpha, two-sided foliage. The vertex colour multiplies the render (0.5 = as
    rendered); AOceanIslandFlora yellows the tufts of the sunny beach top."""
    color_path = os.path.join(TUFT_SOURCE, "tuft_basecolor.png")
    if not os.path.isfile(color_path):
        unreal.log_warning(f"No grass tuft in {TUFT_SOURCE}: M_GrassTuft not built")
        return
    color_texture = import_texture(color_path, "T_GrassTuft_BaseColor", False, alpha_coverage=0.4)
    normal_texture = import_texture(os.path.join(TUFT_SOURCE, "tuft_normal.png"), "T_GrassTuft_Normal", True)
    material = fresh_material("M_GrassTuft")
    material.set_editor_property("two_sided", True)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property("opacity_mask_clip_value", 0.4)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    color = node(material, unreal.MaterialExpressionTextureSample, -900, 0)
    color.set_editor_property("texture", color_texture)
    normal = node(material, unreal.MaterialExpressionTextureSample, -900, 400)
    normal.set_editor_property("texture", normal_texture)
    normal.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    vertex = node(material, unreal.MaterialExpressionVertexColor, -900, 250)
    tinted = node(material, unreal.MaterialExpressionMultiply, -600, 100)
    connect(color, tinted, "A", "RGB")
    connect(vertex, tinted, "B")
    base = node(material, unreal.MaterialExpressionMultiply, -400, 100)
    connect(tinted, base, "A")
    connect(constant(material, 2.0, -600, 220), base, "B")
    MEL.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(color, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
    MEL.connect_material_property(normal, "RGB", unreal.MaterialProperty.MP_NORMAL)
    transmit = node(material, unreal.MaterialExpressionConstant3Vector, -500, 450)
    transmit.set_editor_property("constant", unreal.LinearColor(0.5, 0.6, 0.2, 1.0))
    transmitted = node(material, unreal.MaterialExpressionMultiply, -250, 400)
    connect(base, transmitted, "A")
    connect(transmit, transmitted, "B")
    MEL.connect_material_property(transmitted, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    MEL.connect_material_property(constant(material, 0.7, -250, 550), "", unreal.MaterialProperty.MP_ROUGHNESS)
    save(material, "M_GrassTuft", flutter=0.3)


bark()
foliage()
foliage("M_StemF", (0.06, 0.06, 0.03), 0.85, flutter=0.0)
# Coconut palm leaflets (OceanFlora::MakeCoconutPalm, near levels): waxy and glossy, the sky sheen shows on them; the
# light through them a warm yellow-green.
foliage("M_PalmLeaflet", (0.5, 0.6, 0.15), 0.42, flutter=5.0)  # the frond tips toss, 25 cm and more
leaf_blade()
rock()
palm_frond()
palm_bark()
grass_tuft()
leaf_cards("broadleaf", "Broadleaf", 0.6, 0.4)
leaf_cards("seagrape", "SeaGrape", 0.38, 0.5)  # sea grape leaves are leathery and glossy
unreal.log(f"Flora materials written to {DIRECTORY}")
