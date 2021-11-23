#include "mesh.h"
#include "PathCch.h"
#include "stb_image.h"
#include "DirectXTex.h"
#include <algorithm>

using namespace DirectX;

mesh::asset_data
mesh::process_mesh(aiMesh *mesh, const aiScene *scene)
{
    asset_data asset;

    // Vertices.
    std::vector<mesh::vertex> vertices;
    for (UINT i = 0; i < mesh->mNumVertices; i++)
    {
        mesh::vertex vertex;
        if (mesh->HasPositions())
        {
            vertex.position.x = mesh->mVertices[i].x;
            vertex.position.y = mesh->mVertices[i].y;
            vertex.position.z = mesh->mVertices[i].z;
        }

        if (mesh->HasNormals())
        {
            vertex.normal.x = mesh->mNormals[i].x;
            vertex.normal.y = mesh->mNormals[i].y;
            vertex.normal.z = mesh->mNormals[i].z;
        }

        if (mesh->HasTangentsAndBitangents())
        {
            vertex.tangent.x = mesh->mTangents[i].x;
            vertex.tangent.y = mesh->mTangents[i].y;
            vertex.tangent.z = mesh->mTangents[i].z;

            vertex.bitangent.x = mesh->mBitangents[i].x;
            vertex.bitangent.y = mesh->mBitangents[i].y;
            vertex.bitangent.z = mesh->mBitangents[i].z;
        }

        if (mesh->HasTextureCoords(0))
        {
            vertex.texcoord.x = mesh->mTextureCoords[0][i].x;
            vertex.texcoord.y = mesh->mTextureCoords[0][i].y;
        }

        vertices.push_back(vertex);
    }
    asset.vertices = vertices;

    // Indices.
    std::vector<UINT16> indices;
    for (UINT i = 0; i < mesh->mNumFaces; i++)
    {
        aiFace face = mesh->mFaces[i];
        for (UINT j = 0; j < face.mNumIndices; j++)
        {
            indices.push_back(face.mIndices[j]);
        }
    }
    asset.indices = indices;

    // Materials.
    aiMaterial *material = scene->mMaterials[mesh->mMaterialIndex];
    asset.texture_paths[albedo] = load_material_textures(aiTextureType_BASE_COLOR, material);
    asset.texture_paths[normal] = load_material_textures(aiTextureType_NORMALS, material);
    asset.texture_paths[roughness] = load_material_textures(aiTextureType_DIFFUSE_ROUGHNESS, material);
    asset.texture_paths[metalness] = load_material_textures(aiTextureType_METALNESS, material);
    asset.texture_paths[metallic_roughness] = load_material_textures(aiTextureType_UNKNOWN, material);
    asset.texture_paths[ambient] = load_material_textures(aiTextureType_AMBIENT, material);
    asset.texture_paths[diffuse] = load_material_textures(aiTextureType_DIFFUSE, material);
    asset.texture_paths[emissive_color] = load_material_textures(aiTextureType_EMISSION_COLOR, material);
    asset.texture_paths[emissive] = load_material_textures(aiTextureType_EMISSIVE, material);
    asset.texture_paths[specular] = load_material_textures(aiTextureType_SPECULAR, material);
    asset.texture_paths[shininess] = load_material_textures(aiTextureType_SHININESS, material);

    // Name.
    asset.name = mesh->mName.C_Str();
    return asset;
}

inline std::string mesh::load_material_textures(aiTextureType type, const aiMaterial *material)
{
    aiString path;
    material->GetTexture(type, 0, &path);
    return std::string(path.C_Str());
}

void mesh::process_node(aiNode *node,
                        const aiScene *scene,
                        const std::vector<std::string> &mesh_ignore_list)
{
    // Process all the node's meshes (if any).
    for (UINT i = 0; i < node->mNumMeshes; i++)
    {
        aiMesh *mesh = scene->mMeshes[node->mMeshes[i]];

        if (!mesh_ignore_list.empty())
        {
            std::string mesh_name = std::string(mesh->mName.C_Str());
            bool ignored_found = std::find(mesh_ignore_list.begin(), mesh_ignore_list.end(),
                                           mesh_name) != mesh_ignore_list.end();
            if (ignored_found)
            {
                continue;
            }
        }

        found_asset_data.push_back(process_mesh(mesh, scene));
    }

    // Then do the same for each of its children.
    for (UINT i = 0; i < node->mNumChildren; i++)
    {
        process_node(node->mChildren[i], scene, mesh_ignore_list);
    }
}

void mesh::from_generator(gpu_interface *gpu,
                          ComPtr<ID3D12GraphicsCommandList> cmd_list,
                          GeometryGenerator::MeshData *mesh_data)
{
    UINT vertex_stride = sizeof(GeometryGenerator::Vertex);
    UINT index_stride = sizeof(UINT16);

    UINT vb_byte_size = (UINT)mesh_data->Vertices.size() * vertex_stride;
    UINT ib_byte_size = (UINT)mesh_data->Indices32.size() * index_stride;

    gpu->default_resource_from_uploader(cmd_list, m_vertices_gpu.GetAddressOf(),
                                        mesh_data->Vertices.data(), vb_byte_size, vertex_stride,
                                        D3D12_RESOURCE_FLAG_NONE);
    gpu->default_resource_from_uploader(cmd_list, m_indices_gpu.GetAddressOf(),
                                        mesh_data->GetIndices16().data(), ib_byte_size, index_stride,
                                        D3D12_RESOURCE_FLAG_NONE);

    mesh::submesh submesh = {};
    submesh.index_count = (UINT)mesh_data->Indices32.size();
    submesh.start_index_location = 0;
    submesh.base_vertex_location = 0;
    submesh.bounds = mesh_data->BBox;
    m_submeshes.push_back(submesh);

    m_vbv.BufferLocation = m_vertices_gpu->GetGPUVirtualAddress();
    m_vbv.SizeInBytes = vb_byte_size;
    m_vbv.StrideInBytes = vertex_stride;

    m_ibv.BufferLocation = m_indices_gpu->GetGPUVirtualAddress();
    m_ibv.SizeInBytes = ib_byte_size;
    m_ibv.Format = DXGI_FORMAT_R16_UINT;
}

void mesh::from_asset(gpu_interface *gpu,
                      const std::string &file_name,
                      UINT import_flags,
                      const std::vector<std::string> &ignore_list)
{
    // Create the heap that will hold all of the textures for this mesh.
    const int max_tex_width = 2048;
    const int max_tex_height = 2048;
    const int max_num_mips = int_log2(max_tex_width);
    const int bytes_per_pixel = 4;
    const int max_tex_size = (max_tex_width * max_tex_height) * (max_num_mips * bytes_per_pixel);

    D3D12_HEAP_DESC heap_desc = {};
    heap_desc.Flags = D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES | D3D12_HEAP_FLAG_DENY_BUFFERS;
    heap_desc.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_desc.SizeInBytes = all_textures * max_tex_size;
    check_hr(gpu->device->CreateHeap(&heap_desc, IID_PPV_ARGS(&m_texture_heap)));

    // Import the asset data.
    Assimp::Importer importer;
    import_flags |= aiProcessPreset_TargetRealtime_MaxQuality;
    const aiScene *scene = importer.ReadFile(file_name, import_flags);
    OutputDebugStringA(importer.GetErrorString());

    // Collect the scene data from the asset file.
    if (scene && scene->HasMeshes())
    {
        process_node(scene->mRootNode, scene, ignore_list);
    }

    // Get the name of the folder that contains the asset file,
    // the asset data is usually found in that folder in our case.
    std::wstring textures_folder = std::wstring(file_name.begin(), file_name.end());
    size_t last_backslash_position = textures_folder.rfind(L"\\");
    textures_folder.erase(textures_folder.begin() + last_backslash_position, textures_folder.end());

    // Assign the data found in the asset file to the mesh struct.
    std::vector<mesh::vertex> total_mesh_vertices;
    std::vector<UINT16> total_mesh_indices;

    size_t texture_heap_offset = 0;
    for (int k = 0; k < found_asset_data.size(); k++)
    {
        // Assign the vertices and indices.
        asset_data *asset = &found_asset_data[k];

        mesh::submesh submesh = {};
        submesh.name = asset->name;
        submesh.index_count = (UINT)asset->indices.size();
        submesh.start_index_location = (UINT)total_mesh_indices.size();
        submesh.base_vertex_location = (INT)total_mesh_vertices.size();

        total_mesh_vertices.insert(total_mesh_vertices.end(),
                                   asset->vertices.begin(), asset->vertices.end());
        total_mesh_indices.insert(total_mesh_indices.end(),
                                  asset->indices.begin(), asset->indices.end());

        for (int i = 0; i < all_textures; i++)
        {
            submesh.SRVs[i] = gpu->m_null_srv;

            std::string path = asset->texture_paths[i];
            if (!path.empty())
            {
                // Convert to DDS and upload to the GPU.
                std::wstring file_name = std::wstring(path.begin(), path.end());
                std::wstring full_path = textures_folder + L"\\" + file_name;
                D3D12_RESOURCE_ALLOCATION_INFO alloc_info = gpu->upload_dds(full_path,
                                                                            gpu->get_frame_resource()->cmd_list, submesh.m_textures_gpu[i].GetAddressOf(),
                                                                            true,
                                                                            m_texture_heap, texture_heap_offset);
                size_t texture_aligned_size = align_up(alloc_info.SizeInBytes, alloc_info.Alignment);
                texture_heap_offset += texture_aligned_size;

                // Create an SRV for the current texture resource.
                D3D12_RESOURCE_DESC tex_desc = submesh.m_textures_gpu[i]->GetDesc();
                D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
                srv_desc.Format = tex_desc.Format;
                srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv_desc.Texture2D.MipLevels = tex_desc.MipLevels;
                srv_desc.Texture2D.MostDetailedMip = 0;
                srv_desc.Texture2D.PlaneSlice = 0;
                srv_desc.Texture2D.ResourceMinLODClamp = 0.f;

                submesh.SRVs[i].ptr = gpu->csu_allocator.allocate();
                gpu->device->CreateShaderResourceView(submesh.m_textures_gpu[i].Get(),
                                                      &srv_desc,
                                                      submesh.SRVs[i]);
            }
        }
        m_submeshes.push_back(submesh);
    }

    size_t vertex_stride = sizeof(mesh::vertex);
    size_t vb_byte_size = vertex_stride * total_mesh_vertices.size();
    gpu->default_resource_from_uploader(gpu->get_frame_resource()->cmd_list, m_vertices_gpu.GetAddressOf(),
                                        total_mesh_vertices.data(), vb_byte_size, vertex_stride);

    size_t index_stride = sizeof(UINT16);
    size_t ib_byte_size = index_stride * total_mesh_indices.size();
    gpu->default_resource_from_uploader(gpu->get_frame_resource()->cmd_list, m_indices_gpu.GetAddressOf(),
                                        total_mesh_indices.data(), ib_byte_size, index_stride);

    m_vbv.BufferLocation = m_vertices_gpu->GetGPUVirtualAddress();
    m_vbv.SizeInBytes = (UINT)vb_byte_size;
    m_vbv.StrideInBytes = (UINT)vertex_stride;

    m_ibv.BufferLocation = m_indices_gpu->GetGPUVirtualAddress();
    m_ibv.SizeInBytes = (UINT)ib_byte_size;
    m_ibv.Format = DXGI_FORMAT_R16_UINT;
}