// SPDX-License-Identifier: Apache-2.0
#include <sstream>

#include "xstudio/ui/opengl/no_image_shader_program.hpp"

using namespace xstudio::ui::opengl;

namespace {

static const std::string basic_vertex_shd = R"(
#version 330 core
vec2 calc_pixel_coordinate(vec2 viewport_coordinate)
{
    return viewport_coordinate*4.0;
}
void main()
{
	gl_Position = vec4(1.0,1.0,calc_pixel_coordinate(vec2(0.0,0.0)));
}
)";

static const std::string colour_transforms = R"(
#version 330 core
vec4 colour_transforms(vec4 rgba_in)
{
    return rgba_in;
}
)";

static const std::string basic_frag_shd = R"(
#version 330 core
out vec4 fragColor;
vec4 fetch_rgba_pixel(ivec2 image_coord)
{
    // black!
    return vec4(0.0,0.0,0.0,1.0);
}
void main()
{
	fragColor = fetch_rgba_pixel( ivec2(gl_FragCoord.x, gl_FragCoord.y));

}

)";

} // namespace

NoImageShaderProgram::NoImageShaderProgram()
    : GLShaderProgram(basic_vertex_shd, colour_transforms, basic_frag_shd) {}
