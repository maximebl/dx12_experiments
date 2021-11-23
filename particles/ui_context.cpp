#include "ui_context.h"
#include "particles_graphics.h"
#include "imgui.h"
#include "imgui_helpers.h"

void ui_context::draw(particles_graphics *graphics)
{
    ImGui::Text("%f time", (float)g_cpu_timer.get_current_time());
    ImGui::Text("%d FPS", g_cpu_timer.fps);

    if (ImGui::BeginTable("metrics", 3,
                          ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_BordersOuterH |
                              ImGuiTableFlags_BordersOuterV |
                              ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("CPU (ms)");
        ImGui::TableSetupColumn("GPU (ms)");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();

        for (auto &timer : graphics->m_gpu.timers)
        {
            std::string event_name = timer.first;
            gpu_interface::timer_entry &entries = timer.second;

            // GPU result for the event.
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%f", graphics->m_gpu.result_gpu(event_name));

            // CPU results for the event.
            for (std::string &cpu_event : entries.cpu_event_names)
            {
                // Event name.
                ImGui::TableSetColumnIndex(0);
                ImGui::Text(cpu_event.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%f", graphics->m_gpu.result_cpu(cpu_event));
                ImGui::TableNextRow();
            }
        }

        ImGui::EndTable();
    }

    imgui_mouse_pos();
    bool show_demo = true;
    ImGui::ShowDemoWindow(&show_demo);

    ImGui::Combo("Camera", (int *)&graphics->selected_cam, "main camera\0debug camera\0\0");
    ImGui::Checkbox("Show debug camera", &graphics->show_debug_camera);

    ImGui::Spacing();
    ImGui::Checkbox("Soft point shadows", &graphics->use_pcf_point_shadows);
    if (graphics->use_pcf_point_shadows)
    {
        ImGui::Indent(10.f);
        ImGui::Checkbox("Max quality PCF", &graphics->use_pcf_max_quality);
        ImGui::Unindent(10.f);
    }
    ImGui::Checkbox("Tonemapping", &graphics->do_tonemapping);
    if (graphics->do_tonemapping)
    {
        ImGui::SliderFloat("Exposure", &graphics->exposure, 0.1f, 5.f);
    }
        ImGui::SliderFloat("Pixel clip delta", &graphics->clip_delta, 0.0f, 1.f);


    if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_None))
    {
        ImGui::Text("Indirect lighting");
        ImGui::Combo("Indirect diffuse BRDF", (int *)&graphics->m_indirect_diffuse_brdf, "Lambertian PDF IBL\0Lambertian constant\0\0");
        ImGui::Combo("Indirect specular BRDF", (int *)&graphics->m_indirect_specular_brdf, "GGX PDF IBL\0Simple reflection (not implemented yet)\0\0");

        ImGui::Spacing();

        ImGui::Text("Direct lighting");
        ImGui::Combo("Direct diffuse BRDF", (int *)&graphics->m_direct_diffuse_brdf, "Lambertian\0\0");
        ImGui::Combo("Direct specular BRDF", (int *)&graphics->m_direct_specular_brdf, "Cook-Torrance\0Blinn-Phong\0\0");

        // Cook-Torrance options.
        if (graphics->m_direct_specular_brdf == direct_specular_brdf::cook_torrance)
        {
            ImGui::Combo("Specular shading", (int *)&graphics->specular_shading,
                         "All combined\0Distribution of normals\0Fresnel\0Geometric attenuation\0Distribution of visible normals\0\0");
        }
    }

    ImGui::Spacing();
    ImGui::Checkbox("Draw as billboards", &graphics->draw_billboards);
    ImGui::Checkbox("Draw bounds", &graphics->is_drawing_bounds);
    ImGui::Spacing();

    // Particle systems and their lights.
    ImGui::Text("Particle systems");
    for (int i = 0; i < num_particle_systems; i++)
    {
        ImGui::PushID(i);
        particle_system_info *ps_info = &graphics->particle_systems_infos[i];
        std::string header_title = "Particle system " + std::to_string(ps_info->particle_system_index);
        if (ImGui::CollapsingHeader(header_title.c_str(), ImGuiTreeNodeFlags_None))
        {
            // Particle system data.
            ImGui::SliderFloat("Speed", &ps_info->speed, 0.f, 5.f);
            ImGui::SliderFloat("Amplitude", &ps_info->amplitude, 0.f, 5.f);
            ImGui::SliderFloat("Frequency", &ps_info->frequency, 0.f, 5.f);
            ImGui::SliderFloat("Depth fade", &ps_info->depth_fade, 0.0f, 10.f);

            ImGui::Checkbox("Particle lights enabled", (bool *)&ps_info->particle_lights_enabled);

            if (ps_info->particle_lights_enabled)
            {
                float light_color[3] = {ps_info->light.color.x, ps_info->light.color.y, ps_info->light.color.z};
                ImGui::PushItemWidth(150.f);
                if (ImGui::ColorPicker3("Particle light color", light_color, ImGuiColorEditFlags__OptionsDefault | ImGuiColorEditFlags_Float))
                {
                    ps_info->light.color = {light_color[0], light_color[1], light_color[2]};
                }
                ImGui::PopItemWidth();

                float light_strenght = ps_info->light.strenght.x;
                if (ImGui::SliderFloat("Particle light strenght", &light_strenght, 0.f, 50.f))
                {
                    ps_info->light.strenght = {light_strenght, light_strenght, light_strenght};
                }

                ImGui::SliderFloat("Particle light falloff start", &ps_info->light.falloff_start, 0.01f, 5.f);
                ImGui::SliderFloat("Particle light falloff end", &ps_info->light.falloff_end, 0.01f, 5.f);
            }

            // Volume light data.
            volume_light *vl = &graphics->volume_lights[i];
            ImGui::SliderFloat("Volume light radius", &vl->m_radius, 0.02f, 2.5f);
            ImGui::Combo("Blending mode", (int *)&vl->m_blend_mode, "Alpha transparency\0Additive transparency\0\0");
            float vlight_color[4] = {vl->m_color.x, vl->m_color.y, vl->m_color.z, vl->m_color.w};
            ImGui::PushItemWidth(150.f);
            if (ImGui::ColorPicker4("Volume light color", vlight_color,
                                    ImGuiColorEditFlags__OptionsDefault |
                                        ImGuiColorEditFlags_Float |
                                        ImGuiColorEditFlags_AlphaBar))
            {
                vl->m_color = {vlight_color[0], vlight_color[1], vlight_color[2], vlight_color[3]};
            }
            ImGui::PopItemWidth();

            // Light data.
            attractor_point_light *attractor = &graphics->attractors[i];
            point_light *light = &attractor->light;

            // Falloff start.
            float falloff_start = light->falloff_start;
            if (ImGui::SliderFloat("Attractor light falloff start", &falloff_start, 0.f, 2.f))
            {
                light->falloff_start = falloff_start;
            }

            // Falloff end.
            float falloff_end = light->falloff_end;
            if (ImGui::SliderFloat("Attractor light falloff end", &falloff_end, falloff_start, 20.f))
            {
                light->falloff_end = falloff_end;
            }

            // Position.
            float light_pos[3] = {light->position_ws.x, light->position_ws.y, light->position_ws.z};
            if (ImGui::SliderFloat3("Attractor light position", light_pos, -20.f, 20.f))
            {
                transform t;
                t.set_translation(light_pos[0], light_pos[1], light_pos[2]);
                light->position_ws = t.m_translation;
                attractor->world = t.m_transposed_world;
            }

            // Strenght.
            float light_strenght = light->strenght.x;
            if (ImGui::SliderFloat("Attractor light strenght", &light_strenght, 0.f, 50.f))
            {
                light->strenght = {light_strenght, light_strenght, light_strenght};
            }

            // Color.
            float light_color[3] = {light->color.x, light->color.y, light->color.z};
            ImGui::PushItemWidth(150.f);
            if (ImGui::ColorPicker3("Attractor light color", light_color, ImGuiColorEditFlags__OptionsDefault | ImGuiColorEditFlags_Float))
            {
                light->color = {light_color[0], light_color[1], light_color[2]};
            }
            ImGui::PopItemWidth();
        }
        ImGui::PopID();
    }

    ImGui::Spacing();

    // Spot lights data.
    ImGui::Text("Spot lights");
    for (int i = 0; i < _countof(graphics->spotlights); i++)
    {
        ImGui::PushID(i);
        spot_light *spot_light = &graphics->spotlights[i];

        std::string header_title = "Spot light " + std::to_string(spot_light->id);
        if (ImGui::CollapsingHeader(header_title.c_str(), ImGuiTreeNodeFlags_None))
        {
            // Position.
            float light_pos[3] = {spot_light->position_ws.x, spot_light->position_ws.y, spot_light->position_ws.z};
            if (ImGui::SliderFloat3("Spot light position", light_pos, -20.f, 20.f))
            {
                spot_light->position_ws = {light_pos[0], light_pos[1], light_pos[2]};
            }

            // Direction.
            float light_dir[3] = {spot_light->direction.x, spot_light->direction.y, spot_light->direction.z};
            if (ImGui::SliderFloat3("Spot light direction", light_dir, -1.f, 1.f))
            {
                spot_light->direction = {light_dir[0], light_dir[1], light_dir[2]};
            }

            // Strenght.
            float light_strenght = spot_light->strenght.x;
            if (ImGui::SliderFloat("Spot light strenght", &light_strenght, 0.f, 50.f))
            {
                spot_light->strenght = {light_strenght, light_strenght, light_strenght};
            }

            // Spot power.
            float spot_power = spot_light->spot_power;
            if (ImGui::SliderFloat("Spot power", &spot_power, 1.f, 1024.f))
            {
                spot_light->spot_power = spot_power;
            }

            // Color.
            float light_color[3] = {spot_light->color.x, spot_light->color.y, spot_light->color.z};
            ImGui::PushItemWidth(150.f);
            if (ImGui::ColorPicker3("Spot light color", light_color, ImGuiColorEditFlags__OptionsDefault | ImGuiColorEditFlags_Float))
            {
                spot_light->color = {light_color[0], light_color[1], light_color[2]};
            }
            ImGui::PopItemWidth();
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Text("Render objects");

    int ro_id = 0;
    for (auto &ro_pair : graphics->m_render_objects)
    {
        std::string ro_name = ro_pair.first;
        render_object &ro = ro_pair.second;
        transform *ro_transform = &ro.m_transform;

        ImGui::PushID(ro_id);
        if (ImGui::CollapsingHeader(ro_name.c_str(), ImGuiTreeNodeFlags_None))
        {
            // Scale.
            float ro_scale[3] = {ro_transform->m_scale.x,
                                 ro_transform->m_scale.y,
                                 ro_transform->m_scale.z};

            if (ImGui::SliderFloat3("Scale", ro_scale, 0.f, 20.f))
            {
                ro_transform->set_scale(ro_scale[0], ro_scale[1], ro_scale[2]);
            }

            // Rotation.
            float ro_rotation[3] = {ro_transform->m_rotation.x, ro_transform->m_rotation.y, ro_transform->m_rotation.z};
            if (ImGui::SliderFloat3("Rotation", ro_rotation, -DirectX::XM_PI, DirectX::XM_PI))
            {
                ro_transform->set_rotation(ro_rotation[0], ro_rotation[1], ro_rotation[2]);
            }

            // Translation.
            float ro_translation[3] = {ro_transform->m_translation.x, ro_transform->m_translation.y, ro_transform->m_translation.z};
            if (ImGui::SliderFloat3("Translation", ro_translation, -20.f, 20.f))
            {
                ro_transform->set_translation(ro_translation[0], ro_translation[1], ro_translation[2]);
            }

            // Roughness.
            ImGui::SliderFloat("Roughness", &ro.m_roughness_metalness.x, 0.f, 1.f);

            // Metalness.
            ImGui::SliderFloat("Metalness", &ro.m_roughness_metalness.y, 0.f, 1.f);
        }

        ImGui::PopID();
        ro_id++;
    }

    ImGui::Spacing();
    ImGui::Text("Camera");
    camera *cam = graphics->get_camera();

    float cam_pos[3] = {cam->m_transform.m_translation.x, cam->m_transform.m_translation.y, cam->m_transform.m_translation.z};
    if (ImGui::SliderFloat3("Position", cam_pos, -100.f, 100.f))
    {
        cam->m_transform.set_translation(cam_pos[0], cam_pos[1], cam_pos[2]);
    }

    float cam_rot[3] = {cam->m_transform.m_rotation.x, cam->m_transform.m_rotation.y, cam->m_transform.m_rotation.z};
    if (ImGui::SliderFloat3("Rotation", cam_rot, -DirectX::XM_PI, DirectX::XM_PI))
    {
        cam->m_transform.set_rotation(cam_rot[0], cam_rot[1], cam_rot[2]);
    }

    float cam_right[3] = {cam->m_transform.m_right.x, cam->m_transform.m_right.y, cam->m_transform.m_right.z};
    ImGui::SliderFloat3("Right", cam_right, -100.f, 100.f);

    float cam_up[3] = {cam->m_transform.m_up.x, cam->m_transform.m_up.y, cam->m_transform.m_up.z};
    ImGui::SliderFloat3("Up", cam_up, -100.f, 100.f);

    float cam_forward[3] = {cam->m_transform.m_forward.x, cam->m_transform.m_forward.y, cam->m_transform.m_forward.z};
    ImGui::SliderFloat3("Forward", cam_forward, -100.f, 100.f);
}
