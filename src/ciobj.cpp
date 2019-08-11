#include "../include/ciobj.h"
#include "AssetManager.h"
#include "MiniConfig.h"
#include "cinder/Log.h"
#include "cinder/app/App.h"

using namespace std;

MeshObj::Ref MeshObj::create(ModelObjRef modelObj, const tinyobj::shape_t& property)
{
    auto ref = make_shared<MeshObj>();
    ref->property = property;
    ref->setName(property.name);

    const auto& attrib = modelObj->attrib;

    CI_ASSERT_MSG(property.lines.indices.empty(), "TODO: support line");
    CI_ASSERT_MSG(property.points.indices.empty(), "TODO: support points");
    const auto& indices = property.mesh.indices;
    vector<vec3> positions;
    vector<vec3> normals;
    vector<vec2> texcoords;
    vector<Color> colors;
    vector<uint32_t> indexArray;

    int i = 0;
    for (const auto& index : indices)
    {
        if (!attrib.vertices.empty())
        {
            positions.push_back({ attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]
            });
        }
        if (index.normal_index >= 0 && !attrib.normals.empty())
        {
            normals.push_back({ attrib.normals[3 * index.normal_index + 0],
                attrib.normals[3 * index.normal_index + 1],
                attrib.normals[3 * index.normal_index + 2]
            });
        }
        if (index.texcoord_index >=0 && !attrib.texcoords.empty())
        {
            texcoords.push_back({ attrib.texcoords[2 * index.texcoord_index + 0],
                attrib.texcoords[2 * index.texcoord_index + 1]
            });
        }
        if (!attrib.colors.empty())
        {
            colors.push_back({ attrib.colors[3 * index.vertex_index + 0],
                attrib.colors[3 * index.vertex_index + 1],
                attrib.colors[3 * index.vertex_index + 2]
            });
        }
        indexArray.push_back(i);
        i++;
    }

    TriMesh::Format fmt;
    fmt.positions();
    fmt.normals();
    if (!attrib.texcoords.empty()) fmt.texCoords();
    if (!attrib.colors.empty()) fmt.colors();
    TriMesh triMesh(fmt);

    CI_ASSERT(!attrib.vertices.empty());
    triMesh.appendPositions(positions.data(), positions.size());
    if (!normals.empty())
        triMesh.appendNormals(normals.data(), normals.size());
    if (!texcoords.empty())
        triMesh.appendTexCoords0(texcoords.data(), texcoords.size());
    if (!colors.empty())
        triMesh.appendColors(colors.data(), colors.size());
    triMesh.appendIndices(indexArray.data(), indexArray.size());
    if (normals.empty())
    {
        triMesh.recalculateNormals();
    }
    triMesh.recalculateTangents();
    ref->mBoundBoxMin = triMesh.calcBoundingBox().getMin();
    ref->mBoundBoxMax = triMesh.calcBoundingBox().getMax();

    ref->vboMesh = gl::VboMesh::create(triMesh);
    int mtrl = property.mesh.material_ids[0];
    if (mtrl == -1) mtrl = 0;
    ref->material = modelObj->materials[mtrl];

    return ref;
}

void MeshObj::draw()
{
    if (!vboMesh) return;

    material->preDraw();
    gl::draw(vboMesh);
    material->postDraw();
}

void MaterialObj::preDraw()
{
    ciShader->bind();
    if (diffuseTexture)
        diffuseTexture->bind(0);

    ciShader->uniform("u_flipV", modelObj->flipV);
    ciShader->uniform("u_Camera", modelObj->cameraPosition);
}

void MaterialObj::postDraw()
{
    if (diffuseTexture)
        diffuseTexture->unbind(0);
}

MaterialObj::Ref MaterialObj::create(ModelObjRef modelObj, const tinyobj::material_t& property)
{
    auto ref = make_shared<MaterialObj>();
    ref->property = property;
    ref->modelObj = modelObj;
    ref->name = property.name;

    auto fmt = gl::GlslProg::Format();
    fmt.define("HAS_TANGENTS");
    fmt.define("HAS_NORMALS");
    fmt.define("PBR_SPECCULAR_GLOSSINESS_WORKFLOW");

    //if (ref->baseColorTexture)
    //    fmt.define(ref->materialType == MATERIAL_PBR_METAL_ROUGHNESS ? "HAS_BASECOLORMAP" : "HAS_DIFFUSEMAP");
    //if (ref->metallicRoughnessTexture)
    //    fmt.define(ref->materialType == MATERIAL_PBR_METAL_ROUGHNESS ? "HAS_METALROUGHNESSMAP" : "HAS_SPECULARGLOSSINESSMAP");
    //if (ref->emissiveTexture)
    //    fmt.define("HAS_EMISSIVEMAP");
    //if (ref->normalTexture)
    //    fmt.define("HAS_NORMALMAP");
    //if (ref->occlusionTexture)
    //    fmt.define("HAS_OCCLUSIONMAP");

    if (!property.diffuse_texname.empty())
    {
        ref->diffuseTexture = am::texture2d(property.diffuse_texname);
        if (!ref->diffuseTexture)
        {
            auto path = modelObj->baseDir / property.diffuse_texname;
            ref->diffuseTexture = am::texture2d(path.string());
        }

        fmt.define("HAS_UV");
        fmt.define("HAS_DIFFUSEMAP");
    }

    ref->diffuseFactor = { property.diffuse[0], property.diffuse[1], property.diffuse[2], 1 };
    //ref->ambientFactor = { property.ambient[0], property.ambient[1], property.ambient[2] };
    ref->specularFactor = { property.specular[0], property.specular[1], property.diffuse[2] };
    ref->emissiveFactor = { property.emission[0], property.emission[1], property.emission[2] };
    ref->glossinessFactor = property.shininess;

    fmt.vertex(DataSourcePath::create(app::getAssetPath("pbr.vert")));
    fmt.fragment(DataSourcePath::create(app::getAssetPath("pbr.frag")));
    fmt.label("pbr.vert/pbr.frag");

#if 0 // use stock shader for the moment
    auto ciShader = gl::GlslProg::create(fmt);
#else
    auto ciShader = am::glslProg("lambert texture");
#endif
    CI_ASSERT_MSG(ciShader, "Shader compile fails");
    ref->ciShader = ciShader;

    ciShader->uniform("u_DiffuseFactor", ref->diffuseFactor);
    ciShader->uniform("u_SpecularGlossinessValues", vec4(ref->specularFactor, ref->glossinessFactor));
    ciShader->uniform("u_EmissiveFactor", ref->emissiveFactor);
    ciShader->uniform("u_BaseColorSampler", 0);

    ciShader->uniform("u_LightDirection", vec3(1.0f, 1.0f, 1.0f));
    ciShader->uniform("u_LightColor", vec3(1.0f, 1.0f, 1.0f));
    ciShader->uniform("u_Camera", vec3(1.0f, 1.0f, 1.0f));

    ciShader->uniform("u_NormalScale", 1.0f);
    ciShader->uniform("u_OcclusionStrength", 1.0f);

    //if (ref->normalTexture)
    //    ciShader->uniform("u_NormalSampler", 1);
    //if (ref->emissiveTexture)
    //    ciShader->uniform("u_EmissiveSampler", 2);
    //if (ref->occlusionTexture)
    //    ciShader->uniform("u_OcclusionSampler", 4);

    ref->ciShader = ciShader;

    return ref;
}


ModelObjRef ModelObj::create(const fs::path& meshPath, std::string* loadingError)
{
    if (!fs::exists(meshPath))
    {
        CI_LOG_F("File doesn't exist: ") << meshPath;
        return{};
    }

    auto ref = make_shared<ModelObj>();
    ref->meshPath = meshPath;
    ref->baseDir = meshPath.parent_path().string();

    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;

    std::string warn;
    std::string err;
    bool ret = tinyobj::LoadObj(&ref->attrib, &shapes, &materials, &warn, &err,
        meshPath.string().c_str(), ref->baseDir.string().c_str());
    if (!warn.empty())
    {
        CI_LOG_W(warn);
    }
    if (!err.empty())
    {
        CI_LOG_E(err);
    }
    if (loadingError) *loadingError = err;

    if (!ret)
    {
        CI_LOG_E("Failed to load ") << meshPath;
        return{};
    }

    CI_LOG_I("# of vertices  ") << (ref->attrib.vertices.size() / 3);
    CI_LOG_I("# of normals   ") << (ref->attrib.normals.size() / 3);
    CI_LOG_I("# of texcoords ") << (ref->attrib.texcoords.size() / 2);
    CI_LOG_I("# of materials ") << materials.size();
    CI_LOG_I("# of shapes    ") << shapes.size();

    // Append `default` material
    materials.push_back(tinyobj::material_t());
    for (auto& item : materials)
        ref->materials.emplace_back(MaterialObj::create(ref, item));

    for (auto& item : shapes)
    {
        auto mesh = MeshObj::create(ref, item);
        ref->mBoundBoxMin = mesh->mBoundBoxMin; // TODO: support union of allboxes
        ref->mBoundBoxMax = mesh->mBoundBoxMax; // TODO: support union of allboxes
        ref->addChild(mesh);
    }

    return ref;
}

