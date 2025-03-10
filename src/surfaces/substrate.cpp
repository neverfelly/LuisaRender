//
// Created by Mike Smith on 2022/1/9.
//

#include <util/sampling.h>
#include <util/scattering.h>
#include <base/surface.h>
#include <base/interaction.h>
#include <base/pipeline.h>
#include <base/scene.h>

namespace luisa::render {

using namespace luisa::compute;

class SubstrateSurface final : public Surface {

private:
    const Texture *_kd;
    const Texture *_ks;
    const Texture *_roughness;
    bool _remap_roughness;

public:
    SubstrateSurface(Scene *scene, const SceneNodeDesc *desc) noexcept
        : Surface{scene, desc},
          _kd{scene->load_texture(desc->property_node_or_default(
              "Kd", SceneNodeDesc::shared_default_texture("Constant")))},
          _ks{scene->load_texture(desc->property_node_or_default(
              "Ks", SceneNodeDesc::shared_default_texture("Constant")))},
          _roughness{scene->load_texture(desc->property_node_or_default("roughness"))},
          _remap_roughness{desc->property_bool_or_default("remap_roughness", true)} {}
    [[nodiscard]] auto remap_roughness() const noexcept { return _remap_roughness; }
    [[nodiscard]] string_view impl_type() const noexcept override { return LUISA_RENDER_PLUGIN_NAME; }

private:
    [[nodiscard]] luisa::unique_ptr<Instance> _build(
        Pipeline &pipeline, CommandBuffer &command_buffer) const noexcept override;
};

class SubstrateInstance final : public Surface::Instance {

private:
    const Texture::Instance *_kd;
    const Texture::Instance *_ks;
    const Texture::Instance *_roughness;

public:
    SubstrateInstance(
        const Pipeline &pipeline, const Surface *surface,
        const Texture::Instance *Kd, const Texture::Instance *Ks,
        const Texture::Instance *roughness) noexcept
        : Surface::Instance{pipeline, surface},
          _kd{Kd}, _ks{Ks}, _roughness{roughness} {}
    [[nodiscard]] luisa::unique_ptr<Surface::Closure> closure(
        const Interaction &it, const SampledWavelengths &swl, Expr<float> time) const noexcept override;
    [[nodiscard]] auto Kd() const noexcept { return _kd; }
    [[nodiscard]] auto Ks() const noexcept { return _ks; }
};

luisa::unique_ptr<Surface::Instance> SubstrateSurface::_build(
    Pipeline &pipeline, CommandBuffer &command_buffer) const noexcept {
    auto Kd = pipeline.build_texture(command_buffer, _kd);
    auto Ks = pipeline.build_texture(command_buffer, _ks);
    auto roughness = pipeline.build_texture(command_buffer, _roughness);
    return luisa::make_unique<SubstrateInstance>(
        pipeline, this, Kd, Ks, roughness);
}

class SubstrateClosure final : public Surface::Closure {

private:
    luisa::unique_ptr<TrowbridgeReitzDistribution> _distribution;
    luisa::unique_ptr<FresnelBlend> _blend;
    SampledSpectrum _eta_i;

public:
    SubstrateClosure(
        const Surface::Instance *instance,
        const Interaction &it, const SampledWavelengths &swl, Expr<float> time,
        const SampledSpectrum &Kd, const SampledSpectrum &Ks, Expr<float2> alpha) noexcept
        : Surface::Closure{instance, it, swl, time},
          _distribution{luisa::make_unique<TrowbridgeReitzDistribution>(alpha)},
          _blend{luisa::make_unique<FresnelBlend>(Kd, Ks, _distribution.get())},
          _eta_i{swl.dimension(), 1.f} {}

private:
    [[nodiscard]] Surface::Evaluation evaluate(Expr<float3> wi) const noexcept override {
        auto wo_local = _it.wo_local();
        auto wi_local = _it.shading().world_to_local(wi);
        auto f = _blend->evaluate(wo_local, wi_local);
        auto pdf = _blend->pdf(wo_local, wi_local);
        return {.f = f,
                .pdf = pdf,
                .alpha = _distribution->alpha(),
                .eta = _eta_i};
    }

    [[nodiscard]] Surface::Sample sample(Sampler::Instance &sampler) const noexcept override {
        auto wo_local = _it.wo_local();
        auto u = sampler.generate_2d();
        auto pdf = def(0.f);
        auto wi_local = def(make_float3());
        auto f = _blend->sample(wo_local, &wi_local, u, &pdf);
        auto wi = _it.shading().local_to_world(wi_local);
        return {.wi = wi,
                .eval = {.f = f,
                         .pdf = pdf,
                         .alpha = _distribution->alpha(),
                         .eta = _eta_i}};
    }
    void backward(Expr<float3> wi, const SampledSpectrum &grad) const noexcept override {
        LUISA_ERROR_WITH_LOCATION("Not implemented.");
//        auto _instance = instance<SubstrateInstance>();
//        auto requires_grad_kd = _instance->Kd()->node()->requires_gradients(),
//             requires_grad_ks = _instance->Ks()->node()->requires_gradients();
//        $if(requires_grad_kd || requires_grad_ks) {
//            auto wo_local = _it.wo_local();
//            auto wi_local = _it.shading().world_to_local(wi);
//            auto grad_params = _blend.grad(wo_local, wi_local);
//
//            _instance->Kd()->backward(_it, _swl, _time, grad_params[0] * grad);
//            _instance->Ks()->backward(_it, _swl, _time, grad_params[1] * grad);
//        };
    }
};

luisa::unique_ptr<Surface::Closure> SubstrateInstance::closure(
    const Interaction &it, const SampledWavelengths &swl, Expr<float> time) const noexcept {
    auto Kd = _kd->evaluate_albedo_spectrum(it, swl, time);
    auto Ks = _ks->evaluate_albedo_spectrum(it, swl, time);
    auto alpha = def(make_float2(.5f));
    if (_roughness != nullptr) {
        auto r = _roughness->evaluate(it, time);
        auto remap = node<SubstrateSurface>()->remap_roughness();
        auto r2a = [](auto &&x) noexcept { return TrowbridgeReitzDistribution::roughness_to_alpha(x); };
        alpha = _roughness->node()->channels() == 1u ?
                    (remap ? make_float2(r2a(r.x)) : r.xx()) :
                    (remap ? r2a(r.xy()) : r.xy());
    }
    return luisa::make_unique<SubstrateClosure>(
        this, it, swl, time, Kd, Ks, alpha);
}

}// namespace luisa::render

LUISA_RENDER_MAKE_SCENE_NODE_PLUGIN(luisa::render::SubstrateSurface)
