//
// Created by Mike Smith on 2022/1/10.
//

#include <base/light_sampler.h>

namespace luisa::render {

LightSampler::LightSampler(Scene *scene, const SceneNodeDesc *desc) noexcept
    : SceneNode{scene, desc, SceneNodeTag::LIGHT_SAMPLER} {}

}// namespace luisa::render
