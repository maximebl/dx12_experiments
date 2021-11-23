#pragma once
#include "common.h"
#include "gpu_interface.h"
#include "GeometryGenerator.h"
#include <vector>
#include "directx12_include.h"
#include "DirectXCollision.h"
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/Importer.hpp>
#include <assimp/pbrmaterial.h>

enum texture_type
{
    albedo,
    ambient,
    diffuse,
    emissive_color,
    emissive,
    specular,
    shininess,
    normal,
    roughness,
    metalness,
    metallic_roughness,
    all_textures
};

struct mesh
{
    mesh() = default;
    ~mesh() = default;

    COMMON_API void from_generator(gpu_interface *gpu,
                                   ComPtr<ID3D12GraphicsCommandList> cmd_list,
                                   GeometryGenerator::MeshData *mesh_data);
    COMMON_API void from_asset(gpu_interface *gpu,
                               const std::string &file_name,
                               UINT import_flags = 0,
                               const std::vector<std::string> &mesh_ignore_list = std::vector<std::string>());

    D3D12_VERTEX_BUFFER_VIEW m_vbv;
    D3D12_INDEX_BUFFER_VIEW m_ibv;

    ComPtr<ID3DBlob> m_vertices_cpu;
    ComPtr<ID3DBlob> m_indices_cpu;

    ComPtr<ID3D12Resource> m_vertices_gpu;
    ComPtr<ID3D12Resource> m_indices_gpu;
    ComPtr<ID3D12Heap> m_texture_heap;

    struct submesh
    {
        submesh() = default;
        ~submesh() = default;

        std::string name;
        UINT index_count;
        UINT start_index_location;
        INT base_vertex_location;
        DirectX::BoundingBox bounds;
        D3D12_CPU_DESCRIPTOR_HANDLE SRVs[all_textures];
        ComPtr<ID3D12Resource> m_textures_gpu[all_textures];
    };
    std::vector<submesh> m_submeshes;

    struct vertex
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT3 tangent;
        DirectX::XMFLOAT3 bitangent;
        DirectX::XMFLOAT2 texcoord;
    };

    struct asset_data
    {
        std::string name;
        std::vector<vertex> vertices;
        std::vector<UINT16> indices;
        std::string texture_paths[all_textures];
    };
    std::vector<asset_data> found_asset_data;

private:
    void process_node(aiNode *node,
                      const aiScene *scene,
                      const std::vector<std::string> &mesh_ignore_list);
    asset_data process_mesh(aiMesh *mesh, const aiScene *scene);
    inline std::string load_material_textures(aiTextureType type, const aiMaterial *material);
};
