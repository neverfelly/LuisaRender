//
// Created by Mike on 2021/12/14.
//

#include <base/surface.h>
#include <base/scene.h>
#include <base/interaction.h>
#include <base/pipeline.h>

namespace luisa::render {

Surface::Surface(Scene *scene, const SceneNodeDesc *desc) noexcept
    : SceneNode{scene, desc, SceneNodeTag::SURFACE},
      _normal{scene->load_texture(desc->property_node_or_default("normal"))},
      _alpha{scene->load_texture(desc->property_node_or_default("alpha"))} {
    if (_normal != nullptr && _normal->channels() < 3u) [[unlikely]] {
        LUISA_ERROR_WITH_LOCATION(
            "Expected generic texture with "
            "3 channels for Surface::normal.");
    }
}

luisa::unique_ptr<Surface::Instance> Surface::build(Pipeline &pipeline, CommandBuffer &command_buffer) const noexcept {
    auto instance = _build(pipeline, command_buffer);
    instance->_alpha = pipeline.build_texture(command_buffer, _alpha);
    instance->_normal = pipeline.build_texture(command_buffer, _normal);
    return instance;
}

}// namespace luisa::render
