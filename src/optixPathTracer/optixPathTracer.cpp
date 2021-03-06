//
// Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include <glad/glad.h>  // Needs to be included before gl_interop

#include <cuda_gl_interop.h>
#include "performance_timer.h"

#include <glm/glm.hpp>
#include <glm/gtx/transform.hpp>

#include <optix.h>
#include <optix_function_table_definition.h>
#include <optix_stubs.h>

#include <sampleConfig.h>

#include <sutil/CUDAOutputBuffer.h>
#include <sutil/Camera.h>
#include <sutil/Exception.h>
#include <sutil/GLDisplay.h>
#include <sutil/Matrix.h>
#include <sutil/Trackball.h>
#include <sutil/sutil.h>
#include <sutil/vec_math.h>
#include <optix_stack_size.h>

#include <GLFW/glfw3.h>
#include "optixPathTracer.h"
#include "tiny_obj_loader.h"
#include <map>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <set>
#include <vector>
//#include <opencv2/dnn.hpp>
//#include <opencv2/imgproc.hpp>
//#include <opencv2/highgui.hpp>


#define TINYOBJLOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION

#include "stb_image.h"

#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#define PORT 8080
int python_sock = 0;


namespace std {
    inline bool operator<(const tinyobj::index_t &a,
                          const tinyobj::index_t &b) {
        if (a.vertex_index < b.vertex_index) return true;
        if (a.vertex_index > b.vertex_index) return false;

        if (a.normal_index < b.normal_index) return true;
        if (a.normal_index > b.normal_index) return false;

        if (a.texcoord_index < b.texcoord_index) return true;
        if (a.texcoord_index > b.texcoord_index) return false;

        return false;
    }
}
//using namespace cv;
//using namespace dnn;
//using namespace dnn_superres;

bool resize_dirty = false;
bool minimized = false;
bool saveRequestedFull = false;
bool saveRequestedQuarter = false;
bool re_render = true;

// Camera state
bool camera_changed = true;
bool free_view = false;
sutil::Camera camera;
sutil::Trackball trackball;

// Mouse state
int32_t mouse_button = -1;

// Scene parameters

int32_t samples_per_launch = 4;
int depth = 3;
int width = 768;
int height = 768;

bool denoiser_enabled = true;
bool scene_changed = false;
std::string new_scene_file;

//------------------------------------------------------------------------------
//
// Local types
// TODO: some of these should move to sutil or optix util header
//
//------------------------------------------------------------------------------

template<typename T>
struct Record {
    __align__(OPTIX_SBT_RECORD_ALIGNMENT) char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    T data;
};

typedef Record<RayGenData> RayGenRecord;
typedef Record<MissData> MissRecord;
typedef Record<HitGroupData> HitGroupRecord;

struct Vertex {
    float x, y, z, pad;
};


struct IndexedTriangle {
    uint32_t v1, v2, v3, pad;
};


struct Instance {
    float transform[12];
};

struct Triangle {
    glm::vec3 vertex[3];     // Vertices
    glm::vec3 normal;        // Normal
    glm::vec2 texcoord;
    //Material data;
    glm::vec3 diffuse;
};

struct Texture {
    ~Texture() {
        if (pixel) delete[] pixel;
    }

    uint32_t *pixel{nullptr};
    glm::ivec2 resolution{-1};
};

struct Mesh {
    std::vector<Triangle *> triangles;
    std::vector<glm::vec3> vertex;
    std::vector<glm::vec3> normal;
    std::vector<glm::vec2> texcoord;
    std::vector<glm::ivec3> index;

    glm::vec3 diffuse;
    int diffuseTextureID{-1};
};

struct Model {
    ~Model() {
        for (auto mesh: meshes) delete mesh;
    }

    bool material;
    std::vector<Mesh *> meshes;
    std::vector<Texture *> textures;
};

struct PathTracerState {
    OptixDeviceContext context = 0;

    OptixTraversableHandle gas_handle = 0;  // Traversable handle for triangle AS
    CUdeviceptr d_gas_output_buffer = 0;  // Triangle AS memory
    CUdeviceptr d_vertices = 0;
    CUdeviceptr d_texcoords = 0;
    CUdeviceptr d_lights = 0;

    OptixModule ptx_module = 0;
    OptixPipelineCompileOptions pipeline_compile_options = {};
    OptixPipeline pipeline = 0;

    OptixProgramGroup raygen_prog_group = 0;
    OptixProgramGroup radiance_miss_group = 0;
    OptixProgramGroup occlusion_miss_group = 0;
    OptixProgramGroup radiance_hit_group = 0;
    OptixProgramGroup occlusion_hit_group = 0;

    CUstream stream = 0;
    Params params;
    Params *d_params;

    OptixShaderBindingTable sbt = {};
    Model *model;

    OptixDenoiser denoiser = nullptr;
    CUdeviceptr denoiserScratch = 0;
    uint32_t denoiserScratchSize;
    CUdeviceptr denoiserState = 0;
    uint32_t denoiserStateSize;
    OptixDenoiserParams denoiserParams;

    int frameID = 0;
};

// Timer
PerformanceTimer &timer() {
    static PerformanceTimer timer;
    return timer;
}


//------------------------------------------------------------------------------
//
// Scene data
//
//------------------------------------------------------------------------------
// Buffers - These are initially dynamic
int32_t TRIANGLE_COUNT = 0;
int32_t MAT_COUNT = 0;

std::vector<int> d_textureIds;
std::vector<Vertex> d_vertices;
std::vector<float2> d_texcoords;
std::vector<Material> d_mat_types;
std::vector<uint32_t> d_material_indices;
std::vector<float3> d_emission_colors;
std::vector<float3> d_diffuse_colors;
std::vector<float3> d_spec_colors;
std::vector<float> d_spec_exp;
std::vector<float> d_ior;
std::vector<Triangle> d_triangles;
std::vector<Light> d_lights;
std::vector<cudaArray_t> textureArrays;
std::vector<cudaTextureObject_t> textureObjects;
const Model *MODEL;

static Vertex toVertex(glm::vec3 &v, glm::mat4 &t) {
    // transform the v
    v = glm::vec3(t * glm::vec4(v, 1.f));
    return {v.x, v.y, v.z, 0.f};
}

static glm::vec3 randomColor(int i) {
    {
        int r = unsigned(i) * 13 * 17 + 0x234235;
        int g = unsigned(i) * 7 * 3 * 5 + 0x773477;
        int b = unsigned(i) * 11 * 19 + 0x223766;
        return glm::vec3((r & 255) / 255.f,
                         (g & 255) / 255.f,
                         (b & 255) / 255.f);
    }
}

static Vertex fixToUnitSphere(Vertex v) {
    // fix vertex position to be on unit sphere
    float length = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return {v.x / length, v.y / length, v.z / length, 0.f};
}

static int addMaterial(Material m,
                       float3 dif_col,
                       float3 spec_col,
                       float3 em_col,
                       float spec_exp,
                       float ior) {
    d_mat_types.push_back(m);
    d_diffuse_colors.push_back(dif_col);
    d_spec_colors.push_back(spec_col);
    d_emission_colors.push_back(em_col);
    d_spec_exp.push_back(spec_exp);
    d_ior.push_back(ior);
    MAT_COUNT++;
    return MAT_COUNT - 1;
}

int addVertex(Mesh *mesh,
              tinyobj::attrib_t &attributes,
              const tinyobj::index_t &idx,
              std::map<tinyobj::index_t, int> &knownVertices) {
    const glm::vec3 *vertex_array = (const glm::vec3 *) attributes.vertices.data();
    const glm::vec3 *normal_array = (const glm::vec3 *) attributes.normals.data();
    const glm::vec2 *texcoord_array = (const glm::vec2 *) attributes.texcoords.data();

    int newID = (int) mesh->vertex.size();
    knownVertices[idx] = newID;

    mesh->vertex.push_back(vertex_array[idx.vertex_index]);
    if (idx.normal_index >= 0) {
        while (mesh->normal.size() < mesh->vertex.size())
            mesh->normal.push_back(normal_array[idx.normal_index]);
    }
    if (idx.texcoord_index >= 0) {
        while (mesh->texcoord.size() < mesh->vertex.size())
            mesh->texcoord.push_back(texcoord_array[idx.texcoord_index]);
    }

    // just for sanity's sake:
    if (mesh->texcoord.size() > 0)
        mesh->texcoord.resize(mesh->vertex.size());
    // just for sanity's sake:
    if (mesh->normal.size() > 0)
        mesh->normal.resize(mesh->vertex.size());
    return newID;
}

/*! load a texture (if not already loaded), and return its ID in the
      model's textures[] vector. Textures that could not get loaded
      return -1 */
int loadTexture(Model *model,
                std::map<std::string, int> &knownTextures,
                const std::string &inFileName,
                const std::string &modelPath) {
    if (inFileName == "")
        return -1;

    if (knownTextures.find(inFileName) != knownTextures.end())
        return knownTextures[inFileName];

    std::string fileName = inFileName;
    // first, fix backspaces:
    for (auto &c: fileName)
        if (c == '\\') c = '/';
    fileName = modelPath + "/" + fileName;

    glm::ivec2 res;
    int comp;
    unsigned char *image = stbi_load(fileName.c_str(),
                                     &res.x, &res.y, &comp, STBI_rgb_alpha);
    int textureID = -1;
    if (image) {
        textureID = (int) model->textures.size();
        Texture *texture = new Texture;
        texture->resolution = res;
        texture->pixel = (uint32_t *) image;

        /* iw - actually, it seems that stbi loads the pictures
           mirrored along the y axis - mirror them here */
        for (int y = 0; y < res.y / 2; y++) {
            uint32_t *line_y = texture->pixel + y * res.x;
            uint32_t *mirrored_y = texture->pixel + (res.y - 1 - y) * res.x;
            int mirror_y = res.y - 1 - y;
            for (int x = 0; x < res.x; x++) {
                std::swap(line_y[x], mirrored_y[x]);
            }
        }
        model->textures.push_back(texture);
    } else {
        std::cout << "Could not load texture from " << fileName << "!" << std::endl;
    }

    knownTextures[inFileName] = textureID;
    return textureID;
}


// Reference: TinyOBJ Sample code: https://github.com/tinyobjloader/tinyobjloader
Model *loadMesh(std::string filename) {

    const std::string mtlDir
            = filename.substr(0, filename.rfind('/') + 1);
    Model *model = new Model;
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn;
    std::string err;
    bool material = false;
    // load obj
    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, filename.c_str(), mtlDir.c_str());

    if (!warn.empty()) {
        std::cout << warn << std::endl;
    }

    if (!err.empty()) {
        std::cerr << err << std::endl;
    }

    if (!ret) {
        exit(1);
    }

    if (!materials.empty()) {
        material = true;
        std::cout << "mtl file loaded!" << std::endl;
    }
    // Loop over shapes
    for (size_t s = 0; s < shapes.size(); s++) {
        // Loop over faces(polygon)

        if (material) {
            model->material = true;
            tinyobj::shape_t &shape = shapes[s];
            size_t index_offset = 0;
            std::set<int> materialIDs;
            for (auto faceMatID: shape.mesh.material_ids) {
                materialIDs.insert(faceMatID);
            }
            std::map<tinyobj::index_t, int> knownVertices;
            std::map<std::string, int> knownTextures;
            for (int materialID: materialIDs) {
                Mesh *mesh = new Mesh;

                for (int f = 0; f < shape.mesh.material_ids.size(); f++) {
                    if (shape.mesh.material_ids[f] != materialID) continue;
                    Triangle *t = new Triangle;
                    int fv = shape.mesh.num_face_vertices[f];

                    for (size_t v = 0; v < fv; v++) {
                        // access to vertex
                        // Here only indices and vertices are useful
                        tinyobj::index_t idx = shapes[s].mesh.indices[index_offset + v];
                        tinyobj::real_t vx = attrib.vertices[3 * idx.vertex_index + 0];
                        tinyobj::real_t vy = attrib.vertices[3 * idx.vertex_index + 1];
                        tinyobj::real_t vz = attrib.vertices[3 * idx.vertex_index + 2];

                        t->vertex[v] = glm::vec3(vx, vy, vz);
                    }
                    index_offset += fv;

                    // Compute the initial normal using glm::normalize
                    t->normal = glm::normalize(glm::cross(t->vertex[1] - t->vertex[0], t->vertex[2] - t->vertex[0]));
                    //t->diffuse = (const vec3f&)materials[materialID].diffuse;
                    mesh->triangles.push_back(t);
                    tinyobj::index_t idx0 = shape.mesh.indices[3 * f + 0];
                    tinyobj::index_t idx1 = shape.mesh.indices[3 * f + 1];
                    tinyobj::index_t idx2 = shape.mesh.indices[3 * f + 2];

                    glm::ivec3 idx(addVertex(mesh, attrib, idx0, knownVertices),
                                   addVertex(mesh, attrib, idx1, knownVertices),
                                   addVertex(mesh, attrib, idx2, knownVertices));
                    mesh->index.push_back(idx);
                    mesh->diffuse = glm::vec3(1.f);
                    mesh->diffuseTextureID = loadTexture(model,
                                                         knownTextures,
                                                         materials[materialID].diffuse_texname,
                                                         mtlDir);
                }
                mesh->diffuse = randomColor(materialID);
                if (mesh->triangles.empty())
                    delete mesh;
                else
                    model->meshes.push_back(mesh);
            }
        }
        if (!material) {
            model->material = false;
            Mesh *mesh = new Mesh;
            size_t index_offset = 0;
            for (size_t f = 0; f < shapes[s].mesh.num_face_vertices.size(); f++) {
                int fv = shapes[s].mesh.num_face_vertices[f];
                // Loop over vertices in the face.
                Triangle *t = new Triangle;

                for (size_t v = 0; v < fv; v++) {
                    // access to vertex
                    // Here only indices and vertices are useful
                    tinyobj::index_t idx = shapes[s].mesh.indices[index_offset + v];
                    tinyobj::real_t vx = attrib.vertices[3 * idx.vertex_index + 0];
                    tinyobj::real_t vy = attrib.vertices[3 * idx.vertex_index + 1];
                    tinyobj::real_t vz = attrib.vertices[3 * idx.vertex_index + 2];

                    t->vertex[v] = glm::vec3(vx, vy, vz);
                    mesh->vertex.push_back(glm::vec3(vx, vy, vz));
                }

                index_offset += fv;

                // Compute the initial normal using glm::normalize
                t->normal = glm::normalize(glm::cross(t->vertex[1] - t->vertex[0], t->vertex[2] - t->vertex[0]));
                mesh->triangles.push_back(t);
                d_triangles.push_back(*t);
            }

            model->meshes.push_back(mesh);
        }
    }
    std::cout << "Loaded mesh with " << d_triangles.size() << " triangles from " << filename.c_str() << std::endl;
    return model;
}

static void addSceneGeometry(Geom type,
                             int mat_id,
                             glm::vec3 pos,
                             glm::vec3 rot,
                             glm::vec3 s,
                             std::string objfile) {
    // create a transform matrix from the pos, rot and s
    glm::mat4 translate = glm::translate(glm::mat4(), pos);
    glm::mat4 rotateX = glm::rotate(rot.x, glm::vec3(1.0, 0.0, 0.0));
    glm::mat4 rotateY = glm::rotate(rot.y, glm::vec3(0.0, 1.0, 0.0));
    glm::mat4 rotateZ = glm::rotate(rot.z, glm::vec3(0.0, 0.0, 1.0));
    glm::mat4 scale = glm::scale(s);
    glm::mat4 transform = translate * rotateX * rotateY * rotateZ * scale;

    // determine what kind of geometry is added

    if (type == CUBE) {
        // A cube is made of 12 triangles -> 36 vertices
        // First create a unit cube, then transform the vertices. A unit cube has an edge length of 1.
        glm::vec3 v[] = {glm::vec3(-0.5f, 0.5f, -0.5f), glm::vec3(0.5f, 0.5f, -0.5f), glm::vec3(0.5f, 0.5f, 0.5f),
                         glm::vec3(-0.5f, 0.5f, -0.5f), glm::vec3(-0.5f, 0.5f, 0.5f), glm::vec3(0.5f, 0.5f, 0.5f),
                         glm::vec3(-0.5f, 0.5f, 0.5f), glm::vec3(-0.5f, -0.5f, 0.5f), glm::vec3(0.5f, -0.5f, 0.5f),
                         glm::vec3(-0.5f, 0.5f, 0.5f), glm::vec3(0.5f, -0.5f, 0.5f), glm::vec3(0.5f, 0.5f, 0.5f),
                         glm::vec3(0.5f, 0.5f, 0.5f), glm::vec3(0.5f, -0.5f, 0.5f), glm::vec3(0.5f, 0.5f, -0.5f),
                         glm::vec3(0.5f, 0.5f, -0.5f), glm::vec3(0.5f, -0.5f, 0.5f), glm::vec3(0.5f, -0.5f, -0.5f),
                         glm::vec3(-0.5f, 0.5f, -0.5f), glm::vec3(-0.5f, -0.5f, -0.5f), glm::vec3(-0.5f, 0.5f, 0.5f),
                         glm::vec3(-0.5f, 0.5f, 0.5f), glm::vec3(-0.5f, -0.5f, 0.5f), glm::vec3(-0.5f, -0.5f, -0.5f),
                         glm::vec3(-0.5f, -0.5f, 0.5f), glm::vec3(0.5f, -0.5f, 0.5f), glm::vec3(-0.5f, -0.5f, -0.5f),
                         glm::vec3(-0.5f, -0.5f, -0.5f), glm::vec3(0.5f, -0.5f, -0.5f), glm::vec3(0.5f, -0.5f, 0.5f),
                         glm::vec3(-0.5f, 0.5f, -0.5f), glm::vec3(0.5f, 0.5f, -0.5f), glm::vec3(0.5f, -0.5f, -0.5f),
                         glm::vec3(-0.5f, 0.5f, -0.5f), glm::vec3(-0.5f, -0.5f, -0.5f), glm::vec3(0.5f, -0.5f, -0.5f)};
        // Add the vertices
        for (auto elem: v) {
            d_vertices.push_back(toVertex(elem, transform));
        }
        TRIANGLE_COUNT += 12;

        // Add dummy texture coordinates for cube - we must add for each vertex
        for (int i = 0; i < 36; ++i)
            d_texcoords.push_back(make_float2(0.f));

        // Add material id to mat indices
        for (int i = 0; i < 12; ++i)
            d_material_indices.push_back(mat_id);
    } else if (type == ICOSPHERE) {
        // a sphere can be created by subdividing an icosahedron
        // Source: http://blog.andreaskahler.com/2009/06/creating-icosphere-mesh-in-code.html

        // First, create the 12 vertices of an icosahedron -> an icosahedron has 20 faces
        float t = (1.f + sqrtf(5.f)) / 2.f;
        Vertex p0 = fixToUnitSphere({-1.f, t, 0.f, 0.f});
        Vertex p1 = fixToUnitSphere({1.f, t, 0.f, 0.f});
        Vertex p2 = fixToUnitSphere({-1.f, -t, 0.f, 0.f});
        Vertex p3 = fixToUnitSphere({1.f, -t, 0.f, 0.f});

        Vertex p4 = fixToUnitSphere({0.f, -1.f, t, 0.f});
        Vertex p5 = fixToUnitSphere({0.f, 1.f, t, 0.f});
        Vertex p6 = fixToUnitSphere({0.f, -1.f, -t, 0.f});
        Vertex p7 = fixToUnitSphere({0.f, 1.f, -t, 0.f});

        Vertex p8 = fixToUnitSphere({t, 0.f, -1.f, 0.f});
        Vertex p9 = fixToUnitSphere({t, 0.f, 1.f, 0.f});
        Vertex p10 = fixToUnitSphere({-t, 0.f, -1.f, 0.f});
        Vertex p11 = fixToUnitSphere({-t, 0.f, 1.f, 0.f});

        // create a temporary triangles vector and put all the 20 triangles into it
        // Each triangle is made of 3 consecutive vertices
        std::vector<Vertex> temp_triangles = {p0, p11, p5, p0, p5, p1, p0, p1, p7, p0, p7, p10, p0, p10, p11,
                                              p1, p5, p9, p5, p11, p4, p11, p10, p2, p10, p7, p6, p7, p1, p8,
                                              p3, p9, p4, p3, p4, p2, p3, p2, p6, p3, p6, p8, p3, p8, p9,
                                              p4, p9, p5, p2, p4, p11, p6, p2, p10, p8, p6, p7, p9, p8, p1};

        // Each edge of the icosphere will be split in half -> this will create 4 subtriangles from 1 triangle
        int rec_level = 3; // default subdivision level is set to 3, we can change it later
        for (int i = 0; i < rec_level; ++i) {
            std::vector<Vertex> temp_triangles_2;
            for (int j = 0; j < temp_triangles.size(); j += 3) {

                // get the 3 vertices of the triangle
                Vertex v1 = temp_triangles[j];
                Vertex v2 = temp_triangles[j + 1];
                Vertex v3 = temp_triangles[j + 2];

                // replace current triangle with 4 triangles
                Vertex mid1 = fixToUnitSphere({(v1.x + v2.x) / 2.f, (v1.y + v2.y) / 2.f, (v1.z + v2.z) / 2.f, 0.f});
                Vertex mid2 = fixToUnitSphere({(v2.x + v3.x) / 2.f, (v2.y + v3.y) / 2.f, (v2.z + v3.z) / 2.f, 0.f});
                Vertex mid3 = fixToUnitSphere({(v1.x + v3.x) / 2.f, (v1.y + v3.y) / 2.f, (v1.z + v3.z) / 2.f, 0.f});

                temp_triangles_2.push_back(v1);
                temp_triangles_2.push_back(mid1);
                temp_triangles_2.push_back(mid3);
                temp_triangles_2.push_back(v2);
                temp_triangles_2.push_back(mid2);
                temp_triangles_2.push_back(mid1);
                temp_triangles_2.push_back(v3);
                temp_triangles_2.push_back(mid3);
                temp_triangles_2.push_back(mid2);
                temp_triangles_2.push_back(mid1);
                temp_triangles_2.push_back(mid2);
                temp_triangles_2.push_back(mid3);
            }
            // ping-pong vectors
            temp_triangles = temp_triangles_2;
        }

        // Done with subdivision - now add the resulting vertices to the buffer as well the material indices per triangle
        int num_triangles = 0;
        for (int i = 0; i < temp_triangles.size(); ++i) {
            if (i % 3 == 0) {
                d_material_indices.push_back(mat_id);
                num_triangles++;
            }
            Vertex v = temp_triangles[i];
            auto v_temp = glm::vec3(v.x, v.y, v.z);
            d_vertices.push_back(toVertex(v_temp, transform));
            // Add dummy texture coordinate per vertex
            d_texcoords.push_back(make_float2(0.f));
        }
        TRIANGLE_COUNT += num_triangles;
    } else if (type == MESH) {
        if (objfile == "") {
            return;
        }
        // store vertex count before mesh is added
        int pre_vertex_count = d_vertices.size();
        int pre_tex_count = d_texcoords.size();
        Model *model = loadMesh(objfile);

        for (int i = 0; i < model->meshes.size(); ++i) {
            Mesh *mesh = model->meshes[i];
            int material_id = (model->material) ? addMaterial(TEXTURE, make_float3(mesh->diffuse.x, mesh->diffuse.y,
                                                                                   mesh->diffuse.z), make_float3(0.f),
                                                              make_float3(0.f), 0.f, 0.f) : mat_id;
            d_textureIds.push_back(mesh->diffuseTextureID);
            int triangle_count = 0;
            for (int j = 0; j < mesh->vertex.size(); ++j) {
                d_vertices.push_back(toVertex(mesh->vertex[j], transform));
                if (j % 3 == 0) {
                    d_material_indices.push_back(material_id);
                    TRIANGLE_COUNT += 1;
                }
            }
            for (int k = 0; k < mesh->texcoord.size(); ++k) {
                d_texcoords.push_back(make_float2(mesh->texcoord[k].x, mesh->texcoord[k].y));
            }
        }
        d_triangles.clear();
        MODEL = model;
        // calculate the difference between pre and post mesh vertex count
        // use this to add dummy texture coordinates (if necessary)
        int tex_to_add = d_vertices.size() - pre_vertex_count;
        if (pre_tex_count == d_texcoords.size()) {
            for (int i = 0; i < tex_to_add; ++i)
                d_texcoords.push_back(make_float2(0.f));
        }
    } else if (type == AREA_LIGHT) {
        // We create area lights from 2-D planes
        // A plane is made of 2 triangles -> 6 vertices
        glm::vec3 v[] = {glm::vec3(-0.5f, 0.f, -0.5f), glm::vec3(0.5f, 0.f, -0.5f), glm::vec3(0.5f, 0.f, 0.5f),
                         glm::vec3(-0.5f, 0.f, -0.5f), glm::vec3(-0.5f, 0.f, 0.5f), glm::vec3(0.5f, 0.f, 0.5f)};
        Vertex v1 = toVertex(v[0], transform);
        Vertex corner = toVertex(v[1], transform);
        Vertex v2 = toVertex(v[2], transform);
        d_vertices.push_back(v1);
        d_vertices.push_back(corner);
        d_vertices.push_back(v2);

        d_vertices.push_back(toVertex(v[3], transform));
        d_vertices.push_back(toVertex(v[4], transform));
        d_vertices.push_back(toVertex(v[5], transform));

        // Add dummy texture coordinates per vertex
        for (int i = 0; i < 6; ++i)
            d_texcoords.push_back(make_float2(0.f));

        // Push the material id twice, one per triangle
        d_material_indices.push_back(mat_id);
        d_material_indices.push_back(mat_id);

        TRIANGLE_COUNT += 2;
        // Create a light if material is emissive
        if (d_mat_types[mat_id] == EMISSIVE) {
            float3 c = make_float3(corner.x, corner.y, corner.z); //corner
            float3 v1f = make_float3(v1.x - c.x, 0.f, 0.f); //v1
            float3 v2f = make_float3(0.f, 0.f, v2.z - c.z); //v2
            float3 n = normalize(-cross(v1f, v2f));
            d_lights.push_back({AREA_LIGHT, c, v1f, v2f, n, d_emission_colors[mat_id], 0.f, 0.f});
        }
    } else if (type == POINT_LIGHT) {
        // We only allow point geometry for light sources
        if (d_mat_types[mat_id] != EMISSIVE) return;
        auto temp = glm::vec3(0.f, 0.f, 0.f);
        Vertex pos = toVertex(temp, transform);
        // We can't have the point light itself to be visible since points are not supported by our triangle GAS so we won't be adding it to d_vertices

        float3 pos_f = make_float3(pos.x, pos.y, pos.z);
        d_lights.push_back({POINT_LIGHT, pos_f, pos_f, pos_f, make_float3(0.f), d_emission_colors[mat_id], 0.f, 0.f});
    } else if (type == SPOT_LIGHT) {
        // We only allow spot light geometry for light sources
        if (d_mat_types[mat_id] != EMISSIVE) return;
        // A spotlight is very similar to a point light in terms of being represented by a single point rather than triangle(s)
        // However, a spotlight needs additional light parameters to be set
        auto temp = glm::vec3(0.f, 0.f, 0.f);
        Vertex pos = toVertex(temp, transform);
        float3 pos_f = make_float3(pos.x, pos.y, pos.z);
        // The normal of point lights is simply the direction the spot light cone is facing
        glm::vec4 norm = rotateX * rotateY * rotateZ * glm::vec4(0.f, -1.f, 0.f, 1.f);
        float3 n = normalize(make_float3(norm.x, norm.y, norm.z));
        d_lights.push_back(
                {SPOT_LIGHT, pos_f, pos_f, pos_f, n, d_emission_colors[mat_id], glm::cos(25.f * (float) M_PI / 180.f),
                 glm::cos(20.f * (float) M_PI / 180.f)});
    }
}

//------------------------------------------------------------------------------
//
// GLFW callbacks
//
//------------------------------------------------------------------------------

static void mouseButtonCallback(GLFWwindow *window, int button, int action, int mods) {
    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);

    if (action == GLFW_PRESS) {
        mouse_button = button;
        trackball.startTracking(static_cast<int>( xpos ), static_cast<int>( ypos ));
    } else {
        mouse_button = -1;
    }
}

void initOptixDenoiser(PathTracerState &state) {
    if (!state.params.denoiser) return;
    OptixDenoiserSizes denoiserReturnSizes;
    OPTIX_CHECK(optixDenoiserComputeMemoryResources(state.denoiser, state.params.width, state.params.height,
                                                    &denoiserReturnSizes));

    if (state.denoiserScratch) {
        CUDA_CHECK(cudaFree(reinterpret_cast<void *>( state.denoiserScratch )));
    }
    state.denoiserScratchSize = denoiserReturnSizes.withoutOverlapScratchSizeInBytes;
    CUDA_CHECK(cudaMalloc(
            reinterpret_cast<void **>( &state.denoiserScratch ),
            state.denoiserScratchSize
    ));

    if (state.denoiserState) {
        CUDA_CHECK(cudaFree(reinterpret_cast<void *>( state.denoiserState )));
    }
    CUDA_CHECK(cudaMalloc(
            reinterpret_cast<void **>( &state.denoiserState ),
            denoiserReturnSizes.stateSizeInBytes
    ));
    state.denoiserStateSize = denoiserReturnSizes.stateSizeInBytes;

    OPTIX_CHECK(optixDenoiserSetup(state.denoiser, state.stream,
                                   state.params.width, state.params.height,
                                   state.denoiserState,
                                   state.denoiserStateSize,
                                   state.denoiserScratch,
                                   state.denoiserScratchSize));
}


static void cursorPosCallback(GLFWwindow *window, double xpos, double ypos) {
    Params *params = static_cast<Params *>( glfwGetWindowUserPointer(window));

    if (mouse_button == GLFW_MOUSE_BUTTON_LEFT) {
        trackball.setViewMode(sutil::Trackball::LookAtFixed);
        trackball.updateTracking(static_cast<int>( xpos ), static_cast<int>( ypos ), params->width, params->height);
        camera_changed = true;
    } else if (mouse_button == GLFW_MOUSE_BUTTON_RIGHT || free_view) {
        trackball.setViewMode(sutil::Trackball::EyeFixed);
        trackball.updateTracking(static_cast<int>( xpos ), static_cast<int>( ypos ), params->width, params->height);
        camera_changed = true;
    }
}


static void windowSizeCallback(GLFWwindow *window, int32_t res_x, int32_t res_y) {
    // Keep rendering at the current resolution when the window is minimized.
    if (minimized)
        return;

    // Output dimensions must be at least 1 in both x and y.
    sutil::ensureMinimumSize(res_x, res_y);

    Params *params = static_cast<Params *>( glfwGetWindowUserPointer(window));
    params->width = res_x;
    params->height = res_y;
    camera_changed = true;
    resize_dirty = true;
}


static void windowIconifyCallback(GLFWwindow *window, int32_t iconified) {
    minimized = (iconified > 0);
}


static void keyCallback(GLFWwindow *window, int32_t key, int32_t /*scancode*/, int32_t action, int32_t /*mods*/) {
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ESCAPE) {
            glfwSetWindowShouldClose(window, true);
        } else if (key == GLFW_KEY_V) {
            free_view = !free_view;
        }
    } else if (key == GLFW_KEY_P) {
        // Save the image in full
        saveRequestedFull = true;
    } else if (key == GLFW_KEY_U) {
        // Save the image in quarter
        saveRequestedQuarter = true;
    } else if (key == GLFW_KEY_UP) {
        trackball.wheelEvent(1);
        camera_changed = true;
    } else if (key == GLFW_KEY_RIGHT) {
        trackball.directionalMove(sutil::Direction::ROLL_RIGHT, -1);
        camera_changed = true;
    } else if (key == GLFW_KEY_LEFT) {
        trackball.directionalMove(sutil::Direction::ROLL_LEFT, -1);
        camera_changed = true;
    } else if (key == GLFW_KEY_W) {
        trackball.directionalMove(sutil::Direction::FORWARD, -1);
        camera_changed = true;
    } else if (key == GLFW_KEY_S) {
        trackball.directionalMove(sutil::Direction::BACKWARD, -1);
        camera_changed = true;
    } else if (key == GLFW_KEY_D) {
        trackball.directionalMove(sutil::Direction::RIGHT, -1);
        camera_changed = true;
    } else if (key == GLFW_KEY_A) {
        trackball.directionalMove(sutil::Direction::LEFT, -1);
        camera_changed = true;
    } else if (key == GLFW_KEY_E) {
        trackball.directionalMove(sutil::Direction::UP, -1);
        camera_changed = true;
    } else if (key == GLFW_KEY_Q) {
        trackball.directionalMove(sutil::Direction::DOWN, -1);
        camera_changed = true;
    } else if (key == GLFW_KEY_B) {
        denoiser_enabled = !denoiser_enabled;
    } else if (key == GLFW_KEY_C) {
        scene_changed = true;
        new_scene_file = "../../scenes/living_room.txt";
    }
}


static void scrollCallback(GLFWwindow *window, double xscroll, double yscroll) {
    if (trackball.wheelEvent((int) yscroll))
        camera_changed = true;
}


//------------------------------------------------------------------------------
//
// Helper functions
// TODO: some of these should move to sutil or optix util header
//
//------------------------------------------------------------------------------

void printUsageAndExit(const char *argv0) {
    std::cerr << "Usage  : " << argv0 << " [options]\n";
    std::cerr << "Options: --file | -f <filename>      File for image output\n";
    std::cerr << "         --launch-samples | -s       Number of samples per pixel per launch (default 16)\n";
    std::cerr << "         --no-gl-interop             Disable GL interop for display\n";
    std::cerr << "         --dim=<width>x<height>      Set image dimensions; defaults to 768x768\n";
    std::cerr << "         --help | -h                 Print this usage message\n";
    exit(0);
}


void initLaunchParams(PathTracerState &state) {
    // create the denoiser:
    if (state.params.denoiser) {
        OptixDenoiserOptions denoiserOptions = {};
        OPTIX_CHECK(
                optixDenoiserCreate(state.context, OPTIX_DENOISER_MODEL_KIND_HDR, &denoiserOptions, &state.denoiser));

        state.denoiserParams.denoiseAlpha = 0;
        state.denoiserParams.blendFactor = 0;
        CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&state.denoiserParams.hdrIntensity), sizeof(float)));
        initOptixDenoiser(state);
    }

    /*
    * Copy light data to device
    */
    const size_t lights_size_in_bytes = d_lights.size() * sizeof(Light);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&state.d_lights), lights_size_in_bytes));
    CUDA_CHECK(cudaMemcpy(
            reinterpret_cast<void *>(state.d_lights),
            d_lights.data(), lights_size_in_bytes,
            cudaMemcpyHostToDevice
    ));

    CUDA_CHECK(cudaMalloc(
            reinterpret_cast<void **>( &state.params.accum_buffer ),
            state.params.width * state.params.height * sizeof(float4)
    ));

    state.params.frame_buffer = nullptr;  // Will be set when output buffer is mapped

    state.params.samples_per_launch = samples_per_launch;
    state.params.depth = depth;
    state.params.subframe_index = 0u;

    // Get light sources in the scene
    state.params.lights = reinterpret_cast<Light *>(state.d_lights);
    state.params.num_lights = d_lights.size();
    state.params.handle = state.gas_handle;

    CUDA_CHECK(cudaStreamCreate(&state.stream));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>( &state.d_params ), sizeof(Params)));

}


void handleCameraUpdate(Params &params) {
    if (!camera_changed)
        return;
    camera_changed = false;

    camera.setAspectRatio(static_cast<float>( params.width ) / static_cast<float>( params.height ));
    params.eye = camera.eye();
    camera.UVWFrame(params.U, params.V, params.W);
}


void handleResize(sutil::CUDAOutputBuffer<float4> &output_buffer, PathTracerState &state) {
    if (!resize_dirty)
        return;
    resize_dirty = false;
    initOptixDenoiser(state);

    output_buffer.resize(state.params.width, state.params.height);

    // Realloc accumulation buffer
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>( state.params.accum_buffer )));
    CUDA_CHECK(cudaMalloc(
            reinterpret_cast<void **>( &state.params.accum_buffer ),
            state.params.width * state.params.height * sizeof(float4)
    ));
}


void updateState(sutil::CUDAOutputBuffer<float4> &output_buffer, PathTracerState &state) {
    // Update params on device
    if (camera_changed || resize_dirty)
        state.params.subframe_index = 0;

    handleCameraUpdate(state.params);
    handleResize(output_buffer, state);
}


void launchSubframe(sutil::CUDAOutputBuffer<float4> &output_buffer, PathTracerState &state) {
    // Launch
    float4 *result_buffer_data = output_buffer.map();
    state.params.frame_buffer = result_buffer_data;
    CUDA_CHECK(cudaMemcpyAsync(
            reinterpret_cast<void *>( state.d_params ),
            &state.params, sizeof(Params),
            cudaMemcpyHostToDevice, state.stream
    ));
    OPTIX_CHECK(optixLaunch(
            state.pipeline,
            state.stream,
            reinterpret_cast<CUdeviceptr>( state.d_params ),
            sizeof(Params),
            &state.sbt,
            state.params.width,   // launch width
            state.params.height,  // launch height
            1                     // launch depth
    ));
    output_buffer.unmap();
    CUDA_SYNC_CHECK();

    state.frameID++;
}

void launchDenoisedBuffer(sutil::CUDAOutputBuffer<float4> &denoised_output_buffer, PathTracerState &state) {
    OptixImage2D inputLayer;
    inputLayer.data = reinterpret_cast<CUdeviceptr>(state.params.frame_buffer);
    /// Width of the image (in pixels)
    inputLayer.width = state.params.width;
    /// Height of the image (in pixels)
    inputLayer.height = state.params.height;
    /// Stride between subsequent rows of the image (in bytes).
    inputLayer.rowStrideInBytes = state.params.width * sizeof(float4);
    /// Stride between subsequent pixels of the image (in bytes).
    /// For now, only 0 or the value that corresponds to a dense packing of pixels (no gaps) is supported.
    inputLayer.pixelStrideInBytes = sizeof(float4);
    /// Pixel format.
    inputLayer.format = OPTIX_PIXEL_FORMAT_FLOAT4;

    // -------------------------------------------------------
    OptixImage2D outputLayer;
    outputLayer.data = reinterpret_cast<CUdeviceptr>(denoised_output_buffer.map());
    /// Width of the image (in pixels)
    outputLayer.width = state.params.width;
    /// Height of the image (in pixels)
    outputLayer.height = state.params.height;
    /// Stride between subsequent rows of the image (in bytes).
    outputLayer.rowStrideInBytes = state.params.width * sizeof(float4);
    /// Stride between subsequent pixels of the image (in bytes).
    /// For now, only 0 or the value that corresponds to a dense packing of pixels (no gaps) is supported.
    outputLayer.pixelStrideInBytes = sizeof(float4);
    /// Pixel format.
    outputLayer.format = OPTIX_PIXEL_FORMAT_FLOAT4;

    OptixDenoiserGuideLayer denoiserGuideLayer = {};

    OptixDenoiserLayer denoiserLayer = {};
    denoiserLayer.input = inputLayer;
    denoiserLayer.output = outputLayer;

    OPTIX_CHECK(
            optixDenoiserComputeIntensity(state.denoiser, state.stream, &inputLayer, state.denoiserParams.hdrIntensity,
                                          state.denoiserScratch, state.denoiserScratchSize));

    OPTIX_CHECK(optixDenoiserInvoke(state.denoiser, state.stream,
                                    &state.denoiserParams,
                                    state.denoiserState,
                                    state.denoiserStateSize,
                                    &denoiserGuideLayer,
                                    &denoiserLayer,
                                    1,
                                    0,
                                    0,
                                    state.denoiserScratch,
                                    state.denoiserScratchSize));

}

void displaySubframe(sutil::CUDAOutputBuffer<float4> &output_buffer, sutil::GLDisplay &gl_display, GLFWwindow *window) {
    // Display
    int framebuf_res_x = 0;  // The display's resolution (could be HDPI res)
    int framebuf_res_y = 0;  //
    glfwGetFramebufferSize(window, &framebuf_res_x, &framebuf_res_y);
    gl_display.display(
            output_buffer.width(),
            output_buffer.height(),
            framebuf_res_x,
            framebuf_res_y,
            output_buffer.getPBO()
    );
}


static void context_log_cb(unsigned int level, const char *tag, const char *message, void * /*cbdata */ ) {
    std::cerr << "[" << std::setw(2) << level << "][" << std::setw(12) << tag << "]: " << message << "\n";
}

float3 readCameraFile(std::string &scene_file) {
    char *fname = (char *) scene_file.c_str();
    std::ifstream read_scene(fname);
    if (read_scene.is_open()) {
        std::string line;
        std::getline(read_scene, line);
        std::stringstream tokenizer(line);
        std::string token;
        std::vector<std::string> tokens;
        while (std::getline(tokenizer, token, ' ')) {
            tokens.push_back(token);
        }
        if (strcmp(tokens[0].c_str(), "CAMERA") != 0) {
            std::cout << "CAMERA SETTINGS NOT IN FIRST LINE!" << std::endl;
            return make_float3(-1);
        } else {
            std::vector<float> lookat_val;
            std::string float_token;
            std::stringstream vec3_tokenizer(tokens[4]);
            while (std::getline(vec3_tokenizer, float_token, ',')) {
                lookat_val.push_back(atof(float_token.c_str()));
            }
            if (lookat_val.size() != 3) {
                std::cout << "Invalid camera lookat vector " << std::endl;
                return make_float3(-1);
            }
            return make_float3(lookat_val[0], lookat_val[1], lookat_val[2]);
        }
    }
}

void readSceneFile(std::string &scene_file) {
    std::cout << "Reading scene file: " << scene_file << std::endl;
    char *fname = (char *) scene_file.c_str();
    std::ifstream read_scene(fname);
    int line_num = 0; // track the current line number
    bool cam_set = false; // have we set the scene camera yet?
    if (read_scene.is_open()) {
        std::string line;
        // Read lines from the scene file
        while (std::getline(read_scene, line)) {
            line_num++;
            // tokenize each line by space
            std::stringstream tokenizer(line);
            std::string token;
            std::vector<std::string> tokens;
            while (std::getline(tokenizer, token, ' ')) {
                tokens.push_back(token);
            }
            // process the tokens
            if (tokens.size() != 7) {
                std::cout << "Invalid argument count at line " << line_num << std::endl;
                continue;
            }

            // check if we're reading material, geometry or camera
            if (strcmp(tokens[0].c_str(), "MATERIAL") == 0) {
                // read material type
                Material type;
                if (strcmp(tokens[1].c_str(), "EMISSIVE") == 0) {
                    type = EMISSIVE;
                } else if (strcmp(tokens[1].c_str(), "DIFFUSE") == 0) {
                    type = DIFFUSE;
                } else if (strcmp(tokens[1].c_str(), "MIRROR") == 0) {
                    type = MIRROR;
                } else if (strcmp(tokens[1].c_str(), "GLOSSY") == 0) {
                    type = GLOSSY;
                } else if (strcmp(tokens[1].c_str(), "FRESNEL") == 0) {
                    type = FRESNEL;
                } else {
                    std::cout << "Invalid material type at line " << line_num << std::endl;
                    continue;
                }
                // read material diffuse color
                float3 diffuse;
                std::vector<float> diffuse_val;
                std::stringstream vec3_tokenizer(tokens[2]);
                std::string float_token;
                while (std::getline(vec3_tokenizer, float_token, ',')) {
                    diffuse_val.push_back(atof(float_token.c_str()));
                }
                if (diffuse_val.size() != 3) {
                    std::cout << "Invalid material diffuse color at line" << line_num << std::endl;
                    continue;
                }
                diffuse = make_float3(diffuse_val[0], diffuse_val[1], diffuse_val[2]);
                // read material specular color
                float3 specular;
                std::vector<float> specular_val;
                vec3_tokenizer.clear();
                vec3_tokenizer.str(tokens[3]);
                while (std::getline(vec3_tokenizer, float_token, ',')) {
                    specular_val.push_back(atof(float_token.c_str()));
                }
                if (specular_val.size() != 3) {
                    std::cout << "Invalid material specular color at line" << line_num << std::endl;
                    continue;
                }
                specular = make_float3(specular_val[0], specular_val[1], specular_val[2]);
                // read material emissive color
                float3 emissive;
                std::vector<float> emissive_val;
                vec3_tokenizer.clear();
                vec3_tokenizer.str(tokens[4]);
                while (std::getline(vec3_tokenizer, float_token, ',')) {
                    emissive_val.push_back(atof(float_token.c_str()));
                }
                if (emissive_val.size() != 3) {
                    std::cout << "Invalid material emissive color at line" << line_num << std::endl;
                    continue;
                }
                emissive = make_float3(emissive_val[0], emissive_val[1], emissive_val[2]);
                // read material specular exponent
                float spec_exp = atof(tokens[5].c_str());
                // read material ior
                float ior = atof(tokens[6].c_str());
                // add material
                std::cout << type << " material added!" << std::endl;
                addMaterial(type, diffuse, specular, emissive, spec_exp, ior);
            } else if (strcmp(tokens[0].c_str(), "GEOMETRY") == 0) {
                // read geometry type
                Geom type;
                if (strcmp(tokens[1].c_str(), "CUBE") == 0) {
                    type = CUBE;
                } else if (strcmp(tokens[1].c_str(), "ICOSPHERE") == 0) {
                    type = ICOSPHERE;
                } else if (strcmp(tokens[1].c_str(), "MESH") == 0) {
                    type = MESH;
                } else if (strcmp(tokens[1].c_str(), "AREA_LIGHT") == 0) {
                    type = AREA_LIGHT;
                } else if (strcmp(tokens[1].c_str(), "POINT_LIGHT") == 0) {
                    type = POINT_LIGHT;
                } else if (strcmp(tokens[1].c_str(), "SPOT_LIGHT") == 0) {
                    type = SPOT_LIGHT;
                } else {
                    std::cout << "Invalid geometry type at line " << line_num << std::endl;
                    continue;
                }
                // read geometry material id
                int mat_id = atoi(tokens[2].c_str());
                // read geometry translate
                glm::vec3 translate;
                std::vector<float> translate_val;
                std::stringstream vec3_tokenizer(tokens[3]);
                std::string float_token;
                while (std::getline(vec3_tokenizer, float_token, ',')) {
                    translate_val.push_back(atof(float_token.c_str()));
                }
                if (translate_val.size() != 3) {
                    std::cout << "Invalid geometry translate vector at line" << line_num << std::endl;
                    continue;
                }
                translate = glm::vec3(translate_val[0], translate_val[1], translate_val[2]);
                // read geometry rotate
                glm::vec3 rotate;
                std::vector<float> rotate_val;
                vec3_tokenizer.clear();
                vec3_tokenizer.str(tokens[4]);
                while (std::getline(vec3_tokenizer, float_token, ',')) {
                    rotate_val.push_back(atof(float_token.c_str()));
                }
                if (rotate_val.size() != 3) {
                    std::cout << "Invalid geometry rotate vector at line" << line_num << std::endl;
                    continue;
                }
                rotate = glm::vec3(rotate_val[0], rotate_val[1], rotate_val[2]);
                // read geometry scale
                glm::vec3 scale;
                std::vector<float> scale_val;
                vec3_tokenizer.clear();
                vec3_tokenizer.str(tokens[5]);
                while (std::getline(vec3_tokenizer, float_token, ',')) {
                    scale_val.push_back(atof(float_token.c_str()));
                }
                if (scale_val.size() != 3) {
                    std::cout << "Invalid geometry scale vector at line" << line_num << std::endl;
                    continue;
                }
                scale = glm::vec3(scale_val[0], scale_val[1], scale_val[2]);
                // read obj file path
                std::string obj_file = tokens[6];
                // create geometry
                std::cout << type << " geometry added!" << std::endl;
                addSceneGeometry(type, mat_id, translate, rotate, scale, obj_file);
            } else if (strcmp(tokens[0].c_str(), "CAMERA") == 0) {
                if (cam_set) {
                    // A camera for this scene is already set
                    std::cout << "A camera for this scene is already set - ignoring line " << line_num << std::endl;
                    continue;
                }
                // read scene width & height
                width = atoi(tokens[1].c_str());
                height = atoi(tokens[2].c_str());
                // read camera eye
                std::vector<float> eye_val;
                std::stringstream vec3_tokenizer(tokens[3]);
                std::string float_token;
                while (std::getline(vec3_tokenizer, float_token, ',')) {
                    eye_val.push_back(atof(float_token.c_str()));
                }
                if (eye_val.size() != 3) {
                    std::cout << "Invalid camera eye vector at line" << line_num << std::endl;
                    continue;
                }
                camera.setEye(make_float3(eye_val[0], eye_val[1], eye_val[2]));
                // read camera look at
                std::vector<float> lookat_val;
                vec3_tokenizer.clear();
                vec3_tokenizer.str(tokens[4]);
                while (std::getline(vec3_tokenizer, float_token, ',')) {
                    lookat_val.push_back(atof(float_token.c_str()));
                }
                if (lookat_val.size() != 3) {
                    std::cout << "Invalid camera lookat vector at line" << line_num << std::endl;
                    continue;
                }
                camera.setLookat(make_float3(lookat_val[0], lookat_val[1], lookat_val[2]));
                // read camera up
                std::vector<float> up_val;
                vec3_tokenizer.clear();
                vec3_tokenizer.str(tokens[5]);
                while (std::getline(vec3_tokenizer, float_token, ',')) {
                    up_val.push_back(atof(float_token.c_str()));
                }
                if (up_val.size() != 3) {
                    std::cout << "Invalid camera up vector at line" << line_num << std::endl;
                    continue;
                }
                camera.setUp(make_float3(up_val[0], up_val[1], up_val[2]));
                // read camera fovy
                camera.setFovY(atof(tokens[6].c_str()));
                // setup trackball
                camera_changed = true;
                trackball.setCamera(&camera);
                trackball.setMoveSpeed(10.0f);
                trackball.setDirMoveSpeed(1.0f);
                trackball.setReferenceFrame(
                        make_float3(1.0f, 0.0f, 0.0f),
                        make_float3(0.0f, 0.0f, 1.0f),
                        make_float3(0.0f, 1.0f, 0.0f)
                );
                trackball.setGimbalLock(true);
                cam_set = true;
            } else {
                std::cout << "Invalid item at line " << line_num << std::endl;
                continue;
            }
        }
    }
}


void createContext(PathTracerState &state) {
    // Initialize CUDA
    CUDA_CHECK(cudaFree(0));

    OptixDeviceContext context;
    CUcontext cu_ctx = 0;  // zero means take the current context
    OPTIX_CHECK(optixInit());
    OptixDeviceContextOptions options = {};
    options.logCallbackFunction = &context_log_cb;
    options.logCallbackLevel = 4;
    OPTIX_CHECK(optixDeviceContextCreate(cu_ctx, &options, &context));

    state.context = context;
}

void createTextures() {
    if (MODEL == NULL) {
        return;
    }
    int numTextures = (int) MODEL->textures.size();

    textureArrays.resize(numTextures);
    textureObjects.resize(numTextures);

    for (int textureID = 0; textureID < numTextures; textureID++) {
        auto texture = MODEL->textures[textureID];

        cudaResourceDesc res_desc = {};

        cudaChannelFormatDesc channel_desc;
        int32_t width = texture->resolution.x;
        int32_t height = texture->resolution.y;
        int32_t numComponents = 4;
        int32_t pitch = width * numComponents * sizeof(uint8_t);
        channel_desc = cudaCreateChannelDesc<uchar4>();

        cudaArray_t &pixelArray = textureArrays[textureID];
        CUDA_CHECK(cudaMallocArray(&pixelArray,
                                   &channel_desc,
                                   width, height));

        CUDA_CHECK(cudaMemcpy2DToArray(pixelArray,
                /* offset */0, 0,
                                       texture->pixel,
                                       pitch, pitch, height,
                                       cudaMemcpyHostToDevice));

        res_desc.resType = cudaResourceTypeArray;
        res_desc.res.array.array = pixelArray;

        cudaTextureDesc tex_desc = {};
        tex_desc.addressMode[0] = cudaAddressModeWrap;
        tex_desc.addressMode[1] = cudaAddressModeWrap;
        tex_desc.filterMode = cudaFilterModeLinear;
        tex_desc.readMode = cudaReadModeNormalizedFloat;
        tex_desc.normalizedCoords = 1;
        tex_desc.maxAnisotropy = 1;
        tex_desc.maxMipmapLevelClamp = 99;
        tex_desc.minMipmapLevelClamp = 0;
        tex_desc.mipmapFilterMode = cudaFilterModePoint;
        tex_desc.borderColor[0] = 1.0f;
        tex_desc.sRGB = 0;

        // Create texture object
        cudaTextureObject_t cuda_tex = 0;
        CUDA_CHECK(cudaCreateTextureObject(&cuda_tex, &res_desc, &tex_desc, nullptr));
        textureObjects[textureID] = cuda_tex;
    }
}

void buildMeshAccel(PathTracerState &state) {
    //
    // copy mesh data to device
    //
    const size_t vertices_size_in_bytes = d_vertices.size() * sizeof(Vertex);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>( &state.d_vertices ), vertices_size_in_bytes));
    CUDA_CHECK(cudaMemcpy(
            reinterpret_cast<void *>( state.d_vertices ),
            d_vertices.data(), vertices_size_in_bytes,
            cudaMemcpyHostToDevice
    ));
    const size_t textcoords_size_in_bytes = d_texcoords.size() * sizeof(float2);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&state.d_texcoords), textcoords_size_in_bytes));
    CUDA_CHECK(cudaMemcpy(
            reinterpret_cast<void *>(state.d_texcoords),
            d_texcoords.data(), textcoords_size_in_bytes,
            cudaMemcpyHostToDevice
    ));

    CUdeviceptr d_mat_indices = 0;
    const size_t mat_indices_size_in_bytes = d_material_indices.size() * sizeof(uint32_t);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>( &d_mat_indices ), mat_indices_size_in_bytes));
    CUDA_CHECK(cudaMemcpy(
            reinterpret_cast<void *>( d_mat_indices ),
            d_material_indices.data(),
            mat_indices_size_in_bytes,
            cudaMemcpyHostToDevice
    ));

    //
    // Build triangle GAS
    //

    std::vector<uint32_t> triangle_input_flags; // One per SBT record for this build input
    for (int i = 0; i < MAT_COUNT; ++i) {
        triangle_input_flags.push_back(OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
    }

    OptixBuildInput triangle_input = {};
    triangle_input.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    triangle_input.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
    triangle_input.triangleArray.vertexStrideInBytes = sizeof(Vertex);
    triangle_input.triangleArray.numVertices = static_cast<uint32_t>( d_vertices.size());
    triangle_input.triangleArray.vertexBuffers = &state.d_vertices;
    triangle_input.triangleArray.flags = triangle_input_flags.data();
    triangle_input.triangleArray.numSbtRecords = MAT_COUNT;
    triangle_input.triangleArray.sbtIndexOffsetBuffer = d_mat_indices;
    triangle_input.triangleArray.sbtIndexOffsetSizeInBytes = sizeof(uint32_t);
    triangle_input.triangleArray.sbtIndexOffsetStrideInBytes = sizeof(uint32_t);

    OptixAccelBuildOptions accel_options = {};
    accel_options.buildFlags = OPTIX_BUILD_FLAG_ALLOW_COMPACTION;
    accel_options.operation = OPTIX_BUILD_OPERATION_BUILD;

    OptixAccelBufferSizes gas_buffer_sizes;
    OPTIX_CHECK(optixAccelComputeMemoryUsage(
            state.context,
            &accel_options,
            &triangle_input,
            1,  // num_build_inputs
            &gas_buffer_sizes
    ));

    CUdeviceptr d_temp_buffer;
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>( &d_temp_buffer ), gas_buffer_sizes.tempSizeInBytes));

    // non-compacted output
    CUdeviceptr d_buffer_temp_output_gas_and_compacted_size;
    size_t compactedSizeOffset = roundUp<size_t>(gas_buffer_sizes.outputSizeInBytes, 8ull);
    CUDA_CHECK(cudaMalloc(
            reinterpret_cast<void **>( &d_buffer_temp_output_gas_and_compacted_size ),
            compactedSizeOffset + 8
    ));

    OptixAccelEmitDesc emitProperty = {};
    emitProperty.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
    emitProperty.result = (CUdeviceptr) ((char *) d_buffer_temp_output_gas_and_compacted_size + compactedSizeOffset);

    OPTIX_CHECK(optixAccelBuild(
            state.context,
            0,                                  // CUDA stream
            &accel_options,
            &triangle_input,
            1,                                  // num build inputs
            d_temp_buffer,
            gas_buffer_sizes.tempSizeInBytes,
            d_buffer_temp_output_gas_and_compacted_size,
            gas_buffer_sizes.outputSizeInBytes,
            &state.gas_handle,
            &emitProperty,                      // emitted property list
            1                                   // num emitted properties
    ));

    CUDA_CHECK(cudaFree(reinterpret_cast<void *>( d_temp_buffer )));
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>( d_mat_indices )));

    size_t compacted_gas_size;
    CUDA_CHECK(cudaMemcpy(&compacted_gas_size, (void *) emitProperty.result, sizeof(size_t), cudaMemcpyDeviceToHost));

    if (compacted_gas_size < gas_buffer_sizes.outputSizeInBytes) {
        CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>( &state.d_gas_output_buffer ), compacted_gas_size));

        // use handle as input and output
        OPTIX_CHECK(optixAccelCompact(state.context, 0, state.gas_handle, state.d_gas_output_buffer, compacted_gas_size,
                                      &state.gas_handle));

        CUDA_CHECK(cudaFree((void *) d_buffer_temp_output_gas_and_compacted_size));
    } else {
        state.d_gas_output_buffer = d_buffer_temp_output_gas_and_compacted_size;
    }
}


void createModule(PathTracerState &state) {
    OptixModuleCompileOptions module_compile_options = {};
    module_compile_options.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    module_compile_options.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    module_compile_options.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_DEFAULT;

    state.pipeline_compile_options.usesMotionBlur = false;
    state.pipeline_compile_options.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    state.pipeline_compile_options.numPayloadValues = 2;
    state.pipeline_compile_options.numAttributeValues = 2;
#ifdef DEBUG // Enables debug exceptions during optix launches. This may incur significant performance cost and should only be done during development.
    state.pipeline_compile_options.exceptionFlags = OPTIX_EXCEPTION_FLAG_DEBUG | OPTIX_EXCEPTION_FLAG_TRACE_DEPTH | OPTIX_EXCEPTION_FLAG_STACK_OVERFLOW;
#else
    state.pipeline_compile_options.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
#endif
    state.pipeline_compile_options.pipelineLaunchParamsVariableName = "params";

    const std::string ptx = sutil::getPtxString(OPTIX_SAMPLE_NAME, OPTIX_SAMPLE_DIR, "optixPathTracer.cu");

    char log[2048];
    size_t sizeof_log = sizeof(log);
    OPTIX_CHECK_LOG(optixModuleCreateFromPTX(
            state.context,
            &module_compile_options,
            &state.pipeline_compile_options,
            ptx.c_str(),
            ptx.size(),
            log,
            &sizeof_log,
            &state.ptx_module
    ));
}


void createProgramGroups(PathTracerState &state) {
    OptixProgramGroupOptions program_group_options = {};

    char log[2048];
    size_t sizeof_log = sizeof(log);

    {
        OptixProgramGroupDesc raygen_prog_group_desc = {};
        raygen_prog_group_desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        raygen_prog_group_desc.raygen.module = state.ptx_module;
        raygen_prog_group_desc.raygen.entryFunctionName = "__raygen__rg";

        OPTIX_CHECK_LOG(optixProgramGroupCreate(
                state.context, &raygen_prog_group_desc,
                1,  // num program groups
                &program_group_options,
                log,
                &sizeof_log,
                &state.raygen_prog_group
        ));
    }

    {
        OptixProgramGroupDesc miss_prog_group_desc = {};
        miss_prog_group_desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        miss_prog_group_desc.miss.module = state.ptx_module;
        miss_prog_group_desc.miss.entryFunctionName = "__miss__radiance";
        sizeof_log = sizeof(log);
        OPTIX_CHECK_LOG(optixProgramGroupCreate(
                state.context, &miss_prog_group_desc,
                1,  // num program groups
                &program_group_options,
                log, &sizeof_log,
                &state.radiance_miss_group
        ));

        memset(&miss_prog_group_desc, 0, sizeof(OptixProgramGroupDesc));
        miss_prog_group_desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        miss_prog_group_desc.miss.module = nullptr;  // NULL miss program for occlusion rays
        miss_prog_group_desc.miss.entryFunctionName = nullptr;
        sizeof_log = sizeof(log);
        OPTIX_CHECK_LOG(optixProgramGroupCreate(
                state.context, &miss_prog_group_desc,
                1,  // num program groups
                &program_group_options,
                log,
                &sizeof_log,
                &state.occlusion_miss_group
        ));
    }

    {
        OptixProgramGroupDesc hit_prog_group_desc = {};
        hit_prog_group_desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        hit_prog_group_desc.hitgroup.moduleCH = state.ptx_module;
        hit_prog_group_desc.hitgroup.entryFunctionNameCH = "__closesthit__radiance";
        sizeof_log = sizeof(log);
        OPTIX_CHECK_LOG(optixProgramGroupCreate(
                state.context,
                &hit_prog_group_desc,
                1,  // num program groups
                &program_group_options,
                log,
                &sizeof_log,
                &state.radiance_hit_group
        ));

        memset(&hit_prog_group_desc, 0, sizeof(OptixProgramGroupDesc));
        hit_prog_group_desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        hit_prog_group_desc.hitgroup.moduleCH = state.ptx_module;
        hit_prog_group_desc.hitgroup.entryFunctionNameCH = "__closesthit__occlusion";
        sizeof_log = sizeof(log);
        OPTIX_CHECK(optixProgramGroupCreate(
                state.context,
                &hit_prog_group_desc,
                1,  // num program groups
                &program_group_options,
                log,
                &sizeof_log,
                &state.occlusion_hit_group
        ));
    }
}


void createPipeline(PathTracerState &state) {
    OptixProgramGroup program_groups[] =
            {
                    state.raygen_prog_group,
                    state.radiance_miss_group,
                    state.occlusion_miss_group,
                    state.radiance_hit_group,
                    state.occlusion_hit_group
            };

    OptixPipelineLinkOptions pipeline_link_options = {};
    pipeline_link_options.maxTraceDepth = 2;
    pipeline_link_options.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_FULL;

    char log[2048];
    size_t sizeof_log = sizeof(log);
    OPTIX_CHECK_LOG(optixPipelineCreate(
            state.context,
            &state.pipeline_compile_options,
            &pipeline_link_options,
            program_groups,
            sizeof(program_groups) / sizeof(program_groups[0]),
            log,
            &sizeof_log,
            &state.pipeline
    ));

    // We need to specify the max traversal depth.  Calculate the stack sizes, so we can specify all
    // parameters to optixPipelineSetStackSize.
    OptixStackSizes stack_sizes = {};
    OPTIX_CHECK(optixUtilAccumulateStackSizes(state.raygen_prog_group, &stack_sizes));
    OPTIX_CHECK(optixUtilAccumulateStackSizes(state.radiance_miss_group, &stack_sizes));
    OPTIX_CHECK(optixUtilAccumulateStackSizes(state.occlusion_miss_group, &stack_sizes));
    OPTIX_CHECK(optixUtilAccumulateStackSizes(state.radiance_hit_group, &stack_sizes));
    OPTIX_CHECK(optixUtilAccumulateStackSizes(state.occlusion_hit_group, &stack_sizes));

    uint32_t max_trace_depth = 2;
    uint32_t max_cc_depth = 0;
    uint32_t max_dc_depth = 0;
    uint32_t direct_callable_stack_size_from_traversal;
    uint32_t direct_callable_stack_size_from_state;
    uint32_t continuation_stack_size;
    OPTIX_CHECK(optixUtilComputeStackSizes(
            &stack_sizes,
            max_trace_depth,
            max_cc_depth,
            max_dc_depth,
            &direct_callable_stack_size_from_traversal,
            &direct_callable_stack_size_from_state,
            &continuation_stack_size
    ));

    const uint32_t max_traversal_depth = 1;
    OPTIX_CHECK(optixPipelineSetStackSize(
            state.pipeline,
            direct_callable_stack_size_from_traversal,
            direct_callable_stack_size_from_state,
            continuation_stack_size,
            max_traversal_depth
    ));
}


void createSBT(PathTracerState &state) {
    CUdeviceptr d_raygen_record;
    const size_t raygen_record_size = sizeof(RayGenRecord);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>( &d_raygen_record ), raygen_record_size));

    RayGenRecord rg_sbt = {};
    OPTIX_CHECK(optixSbtRecordPackHeader(state.raygen_prog_group, &rg_sbt));

    CUDA_CHECK(cudaMemcpy(
            reinterpret_cast<void *>( d_raygen_record ),
            &rg_sbt,
            raygen_record_size,
            cudaMemcpyHostToDevice
    ));


    CUdeviceptr d_miss_records;
    const size_t miss_record_size = sizeof(MissRecord);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>( &d_miss_records ), miss_record_size * RAY_TYPE_COUNT));

    MissRecord ms_sbt[2];
    OPTIX_CHECK(optixSbtRecordPackHeader(state.radiance_miss_group, &ms_sbt[0]));
    ms_sbt[0].data.bg_color = make_float4(0.0f);
    OPTIX_CHECK(optixSbtRecordPackHeader(state.occlusion_miss_group, &ms_sbt[1]));
    ms_sbt[1].data.bg_color = make_float4(0.0f);

    CUDA_CHECK(cudaMemcpy(
            reinterpret_cast<void *>( d_miss_records ),
            ms_sbt,
            miss_record_size * RAY_TYPE_COUNT,
            cudaMemcpyHostToDevice
    ));

    CUdeviceptr d_hitgroup_records;
    const size_t hitgroup_record_size = sizeof(HitGroupRecord);
    CUDA_CHECK(cudaMalloc(
            reinterpret_cast<void **>( &d_hitgroup_records ),
            hitgroup_record_size * RAY_TYPE_COUNT * MAT_COUNT
    ));

    std::vector<HitGroupRecord> hitgroup_records;
    for (int i = 0; i < RAY_TYPE_COUNT * MAT_COUNT; ++i) {
        hitgroup_records.push_back(HitGroupRecord());
    }
    int texture_id = 0;
    for (int i = 0; i < MAT_COUNT; ++i) {
        {
            const int sbt_idx = i * RAY_TYPE_COUNT + 0;  // SBT for radiance ray-type for ith material

            OPTIX_CHECK(optixSbtRecordPackHeader(state.radiance_hit_group, &hitgroup_records[sbt_idx]));
            hitgroup_records[sbt_idx].data.emission_color = d_emission_colors[i];
            hitgroup_records[sbt_idx].data.diffuse_color = d_diffuse_colors[i];
            hitgroup_records[sbt_idx].data.specular_color = d_spec_colors[i];
            hitgroup_records[sbt_idx].data.spec_exp = d_spec_exp[i];
            hitgroup_records[sbt_idx].data.ior = d_ior[i];
            hitgroup_records[sbt_idx].data.vertices = reinterpret_cast<float4 *>( state.d_vertices );
            hitgroup_records[sbt_idx].data.mat = d_mat_types[i];
            if (d_mat_types[i] == TEXTURE) {
                if (textureObjects.size() > texture_id) {
                    hitgroup_records[sbt_idx].data.texture = textureObjects[texture_id];
                } else {
                    hitgroup_records[sbt_idx].data.texture = textureObjects[0];
                }
                hitgroup_records[sbt_idx].data.texcoord = reinterpret_cast<float2 *>(state.d_texcoords);
                texture_id++;
            }
        }

        {
            const int sbt_idx = i * RAY_TYPE_COUNT + 1;  // SBT for occlusion ray-type for ith material
            memset(&hitgroup_records[sbt_idx], 0, hitgroup_record_size);

            OPTIX_CHECK(optixSbtRecordPackHeader(state.occlusion_hit_group, &hitgroup_records[sbt_idx]));
        }
    }

    CUDA_CHECK(cudaMemcpy(
            reinterpret_cast<void *>( d_hitgroup_records ),
            hitgroup_records.data(),
            hitgroup_record_size * RAY_TYPE_COUNT * MAT_COUNT,
            cudaMemcpyHostToDevice
    ));

    state.sbt.raygenRecord = d_raygen_record;
    state.sbt.missRecordBase = d_miss_records;
    state.sbt.missRecordStrideInBytes = static_cast<uint32_t>( miss_record_size );
    state.sbt.missRecordCount = RAY_TYPE_COUNT;
    state.sbt.hitgroupRecordBase = d_hitgroup_records;
    state.sbt.hitgroupRecordStrideInBytes = static_cast<uint32_t>( hitgroup_record_size );
    state.sbt.hitgroupRecordCount = RAY_TYPE_COUNT * MAT_COUNT;
}


void cleanupState(PathTracerState &state) {
    OPTIX_CHECK(optixPipelineDestroy(state.pipeline));
    OPTIX_CHECK(optixProgramGroupDestroy(state.raygen_prog_group));
    OPTIX_CHECK(optixProgramGroupDestroy(state.radiance_miss_group));
    OPTIX_CHECK(optixProgramGroupDestroy(state.radiance_hit_group));
    OPTIX_CHECK(optixProgramGroupDestroy(state.occlusion_hit_group));
    OPTIX_CHECK(optixProgramGroupDestroy(state.occlusion_miss_group));
    OPTIX_CHECK(optixModuleDestroy(state.ptx_module));
    OPTIX_CHECK(optixDeviceContextDestroy(state.context));


    CUDA_CHECK(cudaFree(reinterpret_cast<void *>( state.sbt.raygenRecord )));
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>( state.sbt.missRecordBase )));
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>( state.sbt.hitgroupRecordBase )));
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>( state.d_vertices )));
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>(state.d_texcoords)));
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>( state.d_lights )));
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>( state.d_gas_output_buffer )));
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>( state.params.accum_buffer )));
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>( state.d_params )));
}


//------------------------------------------------------------------------------
//
// Main
//
//------------------------------------------------------------------------------

int main(int argc, char *argv[]) {
//    my_init_code();
    PathTracerState state;
    sutil::CUDAOutputBufferType output_buffer_type = sutil::CUDAOutputBufferType::ZERO_COPY;
    float3 prev_lookat;

    //
    // Parse command line options
    //
    std::string outfile;
    std::string scene_file;

    FILE * ffmpeg_file = popen("ffmpeg -y -pixel_format rgb24 -r 60 -i - -pix_fmt yuv420p -f flv rtmp://rtmp_server:1935/live/stream1", "w");
    if ( !ffmpeg_file ) {
        std::cout << "popen error" << std::endl;
        exit(1);
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsageAndExit(argv[0]);
        } else if (arg == "--no-gl-interop") {
            output_buffer_type = sutil::CUDAOutputBufferType::ZERO_COPY;
        } else if (arg == "--file" || arg == "-f") {
            if (i >= argc - 1)
                printUsageAndExit(argv[0]);
            outfile = argv[++i];
        } else if (arg == "--scene" || arg == "-s") {
            if (i >= argc - 1)
                printUsageAndExit(argv[0]);
            scene_file = argv[++i];
        } else if (arg.substr(0, 6) == "--dim=") {
            const std::string dims_arg = arg.substr(6);
            int w, h;
            sutil::parseDimensions(dims_arg.c_str(), w, h);
            state.params.width = w;
            state.params.height = h;
        } else if (arg == "--launch-samples" || arg == "-s") {
            if (i >= argc - 1)
                printUsageAndExit(argv[0]);
            samples_per_launch = atoi(argv[++i]);
        } else {
            std::cerr << "Unknown option '" << argv[i] << "'\n";
            printUsageAndExit(argv[0]);
        }
    }

    /*char filename[] = "..\\..\\..\\scripts\\setup_rtmp.py";
    FILE* fp;

    Py_Initialize();

    fp = _Py_fopen(filename, "r");
    PyRun_SimpleFile(fp, filename);

    Py_Finalize();*/

    try {
        // Set up the scene
        readSceneFile(scene_file);
        prev_lookat = camera.lookat();
        state.params.width = width;
        state.params.height = height;
        state.params.denoiser = 1;

        //
        // Set up OptiX state
        //
        createContext(state);
        buildMeshAccel(state);
        createModule(state);
        createProgramGroups(state);
        createPipeline(state);
        if (MODEL && !MODEL->textures.empty()) {
            createTextures();
        }
        createSBT(state);
        initLaunchParams(state);


        if (outfile.empty()) {
            GLFWwindow *window = sutil::initUI("optixPathTracer", state.params.width, state.params.height);
            glfwSetMouseButtonCallback(window, mouseButtonCallback);
            glfwSetCursorPosCallback(window, cursorPosCallback);
            glfwSetWindowSizeCallback(window, windowSizeCallback);
            glfwSetWindowIconifyCallback(window, windowIconifyCallback);
            glfwSetKeyCallback(window, keyCallback);
            glfwSetScrollCallback(window, scrollCallback);
            glfwSetWindowUserPointer(window, &state.params);

            outfile = "../../frames/output.ppm";

            //
            // Render loop
            //
            {
                sutil::CUDAOutputBuffer<float4> output_buffer(
                        output_buffer_type,
                        state.params.width,
                        state.params.height
                );

                output_buffer.setStream(state.stream);


                sutil::CUDAOutputBuffer<float4> denoised_output_buffer(
                        output_buffer_type,
                        state.params.width,
                        state.params.height
                );

                denoised_output_buffer.setStream(state.stream);


//                // Full image buffer
                sutil::ImageBuffer buffer;
                sutil::GLDisplay gl_display;

////                // 4-way split buffer
//                sutil::ImageBuffer q_buf;
//                q_buf.data = output_buffer.getHostPointer();
//                q_buf.width = output_buffer.width();
//                q_buf.height = output_buffer.height() / 4;
//                q_buf.pixel_format = sutil::BufferImageFormat::UNSIGNED_BYTE4;

                // Timer variables
                std::chrono::duration<double> state_update_time(0.0);
                std::chrono::duration<double> render_time(0.0);
                std::chrono::duration<double> display_time(0.0);
                std::chrono::duration<double> save_time(0.0);
                do {
                    float3 curr_lookat = readCameraFile(scene_file);
                    float3 diff = curr_lookat - prev_lookat;
                    if (diff.x * diff.x + diff.y * diff.y + diff.z * diff.z >= 1) {
                        std::cout << "camera changed!" << std::endl;
                        trackball.setViewMode(sutil::Trackball::EyeFixed);
                        camera.setLookat(curr_lookat);
                        camera_changed = true;
                        prev_lookat = curr_lookat;
                    }

                    auto t0 = std::chrono::steady_clock::now();
                    glfwPollEvents();

                    updateState(output_buffer, state);
                    auto t1 = std::chrono::steady_clock::now();
                    state_update_time += t1 - t0;
                    t0 = t1;
                    state.params.denoiser = denoiser_enabled ? 1 : 0;
//                    if (saveRequestedQuarter) { // D key
//                        // row 1
//                        timer().startCpuTimer();
//                        sutil::saveImage("../../../quarter_1.ppm", q_buf, false);
//                        // row 2
//                        q_buf.data = output_buffer.getHostPointer() + state.params.width * (state.params.height / 4);
//                        sutil::saveImage("../../../quarter_2.ppm", q_buf, false);
//                        // row 3
//                        q_buf.data =
//                                output_buffer.getHostPointer() + 2 * state.params.width * (state.params.height / 4);
//                        sutil::saveImage("../../../quarter_3.ppm", q_buf, false);
//                        // row 4
//                        q_buf.data =
//                                output_buffer.getHostPointer() + 3 * state.params.width * (state.params.height / 4);
//                        sutil::saveImage("../../../quarter_4.ppm", q_buf, false);
//                        timer().endCpuTimer();
//                        saveRequestedQuarter = false;
//                        std::cout << "4-way split elapsed time: " << timer().getCpuElapsedTimeForPreviousOperation()
//                                  << " ms" << std::endl;
//                    } else {
//                    }
                    t1 = std::chrono::steady_clock::now();
                    save_time += t1 - t0;
                    t0 = t1;
                    launchSubframe(output_buffer, state);
                    if (state.params.denoiser) {
                        launchDenoisedBuffer(denoised_output_buffer, state);
                        t1 = std::chrono::steady_clock::now();
                        render_time += t1 - t0;
                        t0 = t1;
                        displaySubframe(denoised_output_buffer, gl_display, window);
                        buffer.data = denoised_output_buffer.getHostPointer();
                        buffer.width = denoised_output_buffer.width();
                        buffer.height = denoised_output_buffer.height();
                        buffer.pixel_format = sutil::BufferImageFormat::FLOAT4;
                    } else {
                        t1 = std::chrono::steady_clock::now();
                        render_time += t1 - t0;
                        t0 = t1;
                        displaySubframe(output_buffer, gl_display, window);
                        buffer.data = output_buffer.getHostPointer();
                        buffer.width = output_buffer.width();
                        buffer.height = output_buffer.height();
                        buffer.pixel_format = sutil::BufferImageFormat::FLOAT4;
                    }
                    sutil::sendImage(outfile.c_str(), buffer, false, python_sock, ffmpeg_file);

                    t1 = std::chrono::steady_clock::now();
                    display_time += t1 - t0;

                    sutil::displayStats(state_update_time, render_time, display_time, save_time);

                    glfwSwapBuffers(window);

                    ++state.params.subframe_index;
                    /*if (scene_changed) {
                        scene_changed = false;
                        scene_file = new_scene_file;
                        readSceneFile(scene_file);
                        prev_lookat = camera.lookat();
                        if (MODEL && !MODEL->textures.empty()) {
                            createTextures();
                        }
                    }*/
                } while (!glfwWindowShouldClose(window));
                CUDA_SYNC_CHECK();
            }

            sutil::cleanupUI(window);
        } else {
            if (output_buffer_type == sutil::CUDAOutputBufferType::GL_INTEROP) {
                sutil::initGLFW();  // For GL context
                sutil::initGL();
            }

            sutil::CUDAOutputBuffer<float4> output_buffer(
                    output_buffer_type,
                    state.params.width,
                    state.params.height
            );

            handleCameraUpdate(state.params);
            handleResize(output_buffer, state);
            launchSubframe(output_buffer, state);

            sutil::ImageBuffer buffer;
            buffer.data = output_buffer.getHostPointer();
            buffer.width = output_buffer.width();
            buffer.height = output_buffer.height();
            buffer.pixel_format = sutil::BufferImageFormat::UNSIGNED_BYTE4;

            sutil::saveImage(outfile.c_str(), buffer, false);

            if (output_buffer_type == sutil::CUDAOutputBufferType::GL_INTEROP) {
                glfwTerminate();
            }
        }

        cleanupState(state);
    }
    catch (std::exception &e) {
        std::cerr << "Caught exception: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
