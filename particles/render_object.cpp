#include "render_object.h"

std::vector<ComPtr<ID3D12Resource>>
render_object::resources(std::optional<texture_type> textures)
{
    if (textures.has_value())
    {
        size_t num_textures = textures.value();
        size_t num_submeshes = m_mesh.m_submeshes.size();
        std::vector<ComPtr<ID3D12Resource>> resources;
        resources.reserve(num_textures * num_submeshes);
        for (size_t i = 0; i < num_submeshes; i++)
        {
            for (size_t j = 0; j < num_textures; j++)
            {
                ComPtr<ID3D12Resource> current_texture = m_mesh.m_submeshes[i].m_textures_gpu[j];
                if (current_texture.Get() != nullptr)
                {
                    resources.push_back(current_texture);
                }
            }
        }
        return resources;
    }
    return {};
}