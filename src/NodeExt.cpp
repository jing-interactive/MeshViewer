#include "NodeExt.h"
#include "cinder/GeomIo.h"
#include "cinder/gl/gl.h"
#include "cinder/TriMesh.h"

using namespace std;
using namespace melo;

GridNode::Ref GridNode::create(float meters)
{
    return std::make_shared<GridNode>(meters);
}

GridNode::GridNode(float meters) : mMeters(meters)
{
    vertBatch = gl::VertBatch::create(GL_LINES);
    vertBatch->begin(GL_LINES);
    for (int i = -meters; i <= meters; ++i)
    {
        vertBatch->color(Color(0.25f, 0.25f, 0.25f));
        vertBatch->color(Color(0.25f, 0.25f, 0.25f));
        vertBatch->color(Color(0.25f, 0.25f, 0.25f));
        vertBatch->color(Color(0.25f, 0.25f, 0.25f));

        vertBatch->vertex(float(i), 0.0f, -meters);
        vertBatch->vertex(float(i), 0.0f, +meters);
        vertBatch->vertex(-meters, 0.0f, float(i));
        vertBatch->vertex(+meters, 0.0f, float(i));
    }
    vertBatch->end();

    shader = gl::getStockShader(gl::ShaderDef().color());

    setName("GridNode");
}

void GridNode::draw()
{
    gl::ScopedDepthTest depthTest(false);
    gl::ScopedGlslProg glsl(shader);
    vertBatch->draw();
    gl::drawCoordinateFrame(mMeters * 0.5f, mMeters * 0.05f, mMeters * 0.005f);
}

BuiltinMeshNode::Ref BuiltinMeshNode::create(TriMeshRef triMesh)
{
    return std::make_shared<BuiltinMeshNode>(triMesh);
}

BuiltinMeshNode::BuiltinMeshNode(TriMeshRef triMesh)
{
    vboMesh = gl::VboMesh::create(*triMesh);
    shader = gl::getStockShader(gl::ShaderDef().color());
    setName("BuiltinMeshNode");
}

BuiltinMeshNode::Ref BuiltinMeshNode::create(const geom::Source& source)
{
    auto triMesh = TriMesh::create(source);
    return make_shared<BuiltinMeshNode>(triMesh);
}

void BuiltinMeshNode::draw()
{
    if (vboMesh)
    {
        gl::ScopedGlslProg glsl(shader);
        gl::draw(vboMesh);
    }
}