/*
 * Copyright (c) 2021, Jesse Buhagiar <jooster669@gmail.com>
 * Copyright (c) 2021, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2022, Jelle Raaijmakers <jelle@gmta.nl>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibGPU/Vertex.h>
#include <LibGfx/Vector4.h>
#include <LibSoftGPU/Clipper.h>

namespace SoftGPU {

template<Clipper::ClipFrustrum plane>
static constexpr bool point_within_frustrum(FloatVector4 const& vertex)
{
    if constexpr (plane == Clipper::ClipFrustrum::LEFT)
        return vertex.x() >= -vertex.w();
    else if constexpr (plane == Clipper::ClipFrustrum::RIGHT)
        return vertex.x() <= vertex.w();
    else if constexpr (plane == Clipper::ClipFrustrum::TOP)
        return vertex.y() <= vertex.w();
    else if constexpr (plane == Clipper::ClipFrustrum::BOTTOM)
        return vertex.y() >= -vertex.w();
    else if constexpr (plane == Clipper::ClipFrustrum::NEAR)
        return vertex.z() >= -vertex.w();
    else if constexpr (plane == Clipper::ClipFrustrum::FAR)
        return vertex.z() <= vertex.w();
    return false;
}

static bool point_within_plane(FloatVector4 const& vertex, FloatVector4 const& user_plane)
{
    return vertex.dot(user_plane) >= 0;
}

template<Clipper::ClipFrustrum plane>
static constexpr GPU::Vertex clip_intersection_point_frustrum(GPU::Vertex const& p1, GPU::Vertex const& p2)
{
    constexpr FloatVector4 clip_plane_normals[] = {
        { 1, 0, 0, 1 },  // Left Plane
        { -1, 0, 0, 1 }, // Right Plane
        { 0, -1, 0, 1 }, // Top Plane
        { 0, 1, 0, 1 },  // Bottom plane
        { 0, 0, 1, 1 },  // Near Plane
        { 0, 0, -1, 1 }  // Far Plane
    };
    constexpr auto clip_plane_normal = clip_plane_normals[to_underlying(plane)];

    // See https://www.microsoft.com/en-us/research/wp-content/uploads/1978/01/p245-blinn.pdf
    // "Clipping Using Homogeneous Coordinates" Blinn/Newell, 1978
    // Clip plane normals have W=1 so the vertices' W coordinates are included in x1 and x2.

    auto const x1 = clip_plane_normal.dot(p1.clip_coordinates);
    auto const x2 = clip_plane_normal.dot(p2.clip_coordinates);
    auto const a = x1 / (x1 - x2);

    GPU::Vertex out;
    out.position = mix(p1.position, p2.position, a);
    out.eye_coordinates = mix(p1.eye_coordinates, p2.eye_coordinates, a);
    out.clip_coordinates = mix(p1.clip_coordinates, p2.clip_coordinates, a);
    out.color = mix(p1.color, p2.color, a);
    for (size_t i = 0; i < GPU::NUM_SAMPLERS; ++i)
        out.tex_coords[i] = mix(p1.tex_coords[i], p2.tex_coords[i], a);
    out.normal = mix(p1.normal, p2.normal, a);
    return out;
}

static GPU::Vertex clip_intersection_point_plane(GPU::Vertex const& p1, GPU::Vertex const& p2, FloatVector4 const& plane)
{
    auto const x1 = plane.dot(p1.eye_coordinates);
    auto const x2 = plane.dot(p2.eye_coordinates);
    auto const a = x1 / (x1 - x2);

    GPU::Vertex out;
    out.position = mix(p1.position, p2.position, a);
    out.eye_coordinates = mix(p1.eye_coordinates, p2.eye_coordinates, a);
    out.clip_coordinates = mix(p1.clip_coordinates, p2.clip_coordinates, a);
    out.color = mix(p1.color, p2.color, a);
    for (size_t i = 0; i < GPU::NUM_SAMPLERS; ++i)
        out.tex_coords[i] = mix(p1.tex_coords[i], p2.tex_coords[i], a);
    out.normal = mix(p1.normal, p2.normal, a);
    return out;
}

template<Clipper::ClipFrustrum plane>
FLATTEN static constexpr void clip_frustrum(Vector<GPU::Vertex>& input_list, Vector<GPU::Vertex>& output_list)
{
    output_list.clear_with_capacity();

    auto input_list_size = input_list.size();
    if (input_list_size == 0)
        return;

    auto const* prev_vec = &input_list.data()[0];
    auto is_prev_point_within_frustrum = point_within_frustrum<plane>(prev_vec->clip_coordinates);

    for (size_t i = 1; i <= input_list_size; i++) {
        auto const& curr_vec = input_list[i % input_list_size];
        auto const is_curr_point_within_frustrum = point_within_frustrum<plane>(curr_vec.clip_coordinates);

        if (is_curr_point_within_frustrum != is_prev_point_within_frustrum)
            output_list.append(clip_intersection_point_frustrum<plane>(*prev_vec, curr_vec));

        if (is_curr_point_within_frustrum)
            output_list.append(curr_vec);

        prev_vec = &curr_vec;
        is_prev_point_within_frustrum = is_curr_point_within_frustrum;
    }
}

FLATTEN static void clip_plane(Vector<GPU::Vertex>& input_list, Vector<GPU::Vertex>& output_list, FloatVector4 const& plane)
{
    // Clipping done in visual space
    output_list.clear_with_capacity();

    auto input_list_size = input_list.size();
    if (input_list_size == 0)
        return;

    auto const* prev_vec = &input_list.data()[0];
    auto is_prev_point_within_plane = point_within_plane(prev_vec->eye_coordinates, plane);

    for (size_t i = 1; i <= input_list_size; i++) {
        auto const& curr_vec = input_list[i % input_list_size];
        auto const is_curr_point_within_plane = point_within_plane(curr_vec.eye_coordinates, plane);

        if (is_curr_point_within_plane != is_prev_point_within_plane)
            output_list.append(clip_intersection_point_plane(*prev_vec, curr_vec, plane));

        if (is_curr_point_within_plane)
            output_list.append(curr_vec);

        prev_vec = &curr_vec;
        is_prev_point_within_plane = is_curr_point_within_plane;
    }
}


void Clipper::clip_triangle_against_frustum(Vector<GPU::Vertex>& input_verts)
{
    // FIXME C++23. Static reflection will provide looping over all enum values.
    clip_frustrum<ClipFrustrum::LEFT>(input_verts, m_vertex_buffer);
    clip_frustrum<ClipFrustrum::RIGHT>(m_vertex_buffer, input_verts);
    clip_frustrum<ClipFrustrum::TOP>(input_verts, m_vertex_buffer);
    clip_frustrum<ClipFrustrum::BOTTOM>(m_vertex_buffer, input_verts);
    clip_frustrum<ClipFrustrum::NEAR>(input_verts, m_vertex_buffer);
    clip_frustrum<ClipFrustrum::FAR>(m_vertex_buffer, input_verts);
}

void Clipper::clip_triangle_against_user_defined(Vector<GPU::Vertex>& input_verts, Vector<FloatVector4>& user_planes)
{
    auto& in = input_verts;
    auto& out = m_vertex_buffer;
    for (auto const& plane : user_planes) {
        clip_plane(in, out, plane);
        auto& temp = in;
        in = out;
        out = temp;
    }

    if (user_planes.size() % 2 != 0)
        input_verts = m_vertex_buffer;
}

}
