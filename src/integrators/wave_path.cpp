//
// Created by Mike Smith on 2022/1/10.
//

#include <tinyexr.h>
#include <luisa-compute.h>

#include <util/medium_tracker.h>
#include <base/pipeline.h>
#include <base/integrator.h>

namespace luisa::render {

using namespace compute;

class WavefrontPathTracing final : public Integrator {

private:
    uint _max_depth;
    uint _rr_depth;
    float _rr_threshold;

public:
    WavefrontPathTracing(Scene *scene, const SceneNodeDesc *desc) noexcept
        : Integrator{scene, desc},
          _max_depth{std::max(desc->property_uint_or_default("depth", 10u), 1u)},
          _rr_depth{std::max(desc->property_uint_or_default("rr_depth", 0u), 0u)},
          _rr_threshold{std::max(desc->property_float_or_default("rr_threshold", 0.95f), 0.05f)} {}
    [[nodiscard]] auto max_depth() const noexcept { return _max_depth; }
    [[nodiscard]] auto rr_depth() const noexcept { return _rr_depth; }
    [[nodiscard]] auto rr_threshold() const noexcept { return _rr_threshold; }
    [[nodiscard]] bool is_differentiable() const noexcept override { return false; }
    [[nodiscard]] string_view impl_type() const noexcept override { return LUISA_RENDER_PLUGIN_NAME; }
    [[nodiscard]] unique_ptr<Instance> build(Pipeline &pipeline, CommandBuffer &command_buffer) const noexcept override;
};

class PathStateSOA {

private:
    const Spectrum::Instance *_spectrum;
    Buffer<float> _swl_lambda;
    Buffer<float> _swl_pdf;
    Buffer<float> _beta;
    Buffer<float> _radiance;
    Buffer<float> _pdf_bsdf;

public:
    PathStateSOA(const Spectrum::Instance *spectrum, size_t size) noexcept
        : _spectrum{spectrum} {
        auto &&device = spectrum->pipeline().device();
        auto dimension = spectrum->node()->dimension();
        _beta = device.create_buffer<float>(size * dimension);
        _radiance = device.create_buffer<float>(size * dimension);
        _pdf_bsdf = device.create_buffer<float>(size);
        if (!spectrum->node()->is_fixed()) {
            _swl_lambda = device.create_buffer<float>(size * dimension);
            _swl_pdf = device.create_buffer<float>(size * dimension);
        }
    }
    [[nodiscard]] auto read_beta(Expr<uint> index) const noexcept {
        auto dimension = _spectrum->node()->dimension();
        auto offset = index * dimension;
        SampledSpectrum s{dimension};
        for (auto i = 0u; i < dimension; i++) {
            s[i] = _beta.read(offset + i);
        }
        return s;
    }
    void write_beta(Expr<uint> index, const SampledSpectrum &beta) noexcept {
        auto dimension = _spectrum->node()->dimension();
        auto offset = index * dimension;
        for (auto i = 0u; i < dimension; i++) {
            _beta.write(offset + i, beta[i]);
        }
    }
    [[nodiscard]] auto read_swl(Expr<uint> index) const noexcept {
        if (_spectrum->node()->is_fixed()) {
            // FIXME: unsafe
            auto sampler = static_cast<Sampler::Instance *>(nullptr);
            return _spectrum->sample(*sampler);
        }
        SampledWavelengths swl{_spectrum};
        auto offset = index * swl.dimension();
        for (auto i = 0u; i < swl.dimension(); i++) {
            swl.set_lambda(i, _swl_lambda.read(offset + i));
            swl.set_pdf(i, _swl_pdf.read(offset + i));
        }
        return swl;
    }
    void write_swl(Expr<uint> index, const SampledWavelengths &swl) noexcept {
        if (!_spectrum->node()->is_fixed()) {
            auto offset = index * swl.dimension();
            for (auto i = 0u; i < swl.dimension(); i++) {
                _swl_lambda.write(offset + i, swl.lambda(i));
                _swl_pdf.write(offset + i, swl.pdf(i));
            }
        }
    }
    [[nodiscard]] auto read_radiance(Expr<uint> index) const noexcept {
        auto dimension = _spectrum->node()->dimension();
        auto offset = index * dimension;
        SampledSpectrum s{dimension};
        for (auto i = 0u; i < dimension; i++) {
            s[i] = _radiance.read(offset + i);
        }
        return s;
    }
    void write_radiance(Expr<uint> index, const SampledSpectrum &s) noexcept {
        auto dimension = _spectrum->node()->dimension();
        auto offset = index * dimension;
        for (auto i = 0u; i < dimension; i++) {
            _radiance.write(offset + i, s[i]);
        }
    }
    [[nodiscard]] auto read_pdf_bsdf(Expr<uint> index) const noexcept {
        return _pdf_bsdf.read(index);
    }
    void write_pdf_bsdf(Expr<uint> index, Expr<float> pdf) noexcept {
        _pdf_bsdf.write(index, pdf);
    }
};

class LightSampleSOA {

private:
    const Spectrum::Instance *_spectrum;
    Buffer<float> _emission;
    Buffer<float> _pdf;
    Buffer<float3> _wi;

public:
    LightSampleSOA(const Spectrum::Instance *spec, size_t size) noexcept
        : _spectrum{spec} {
        auto &&device = spec->pipeline().device();
        auto dimension = spec->node()->dimension();
        _emission = device.create_buffer<float>(size * dimension);
        _pdf = device.create_buffer<float>(size);
        _wi = device.create_buffer<float3>(size);
    }
    [[nodiscard]] auto read_emission(Expr<uint> index) const noexcept {
        auto dimension = _spectrum->node()->dimension();
        auto offset = index * dimension;
        SampledSpectrum s{dimension};
        for (auto i = 0u; i < dimension; i++) {
            s[i] = _emission.read(offset + i);
        }
        return s;
    }
    void write_emission(Expr<uint> index, const SampledSpectrum &s) noexcept {
        auto dimension = _spectrum->node()->dimension();
        auto offset = index * dimension;
        for (auto i = 0u; i < dimension; i++) {
            _emission.write(offset + i, s[i]);
        }
    }
    [[nodiscard]] auto read_pdf(Expr<uint> index) const noexcept {
        return _pdf.read(index);
    }
    void write_pdf(Expr<uint> index, Expr<float> pdf) noexcept {
        _pdf.write(index, pdf);
    }
    [[nodiscard]] auto read_wi(Expr<uint> index) const noexcept {
        return _wi.read(index);
    }
    void write_wi(Expr<uint> index, Expr<float3> wi) const noexcept {
        _wi.write(index, wi);
    }
};

class RayQueue {

public:
    static constexpr auto counter_buffer_size = 1024u;

private:
    Buffer<uint> _index_buffer;
    Buffer<uint> _counter_buffer;
    uint _current_counter;
    Shader1D<> _clear_counters;

public:
    RayQueue(Device &device, size_t size) noexcept
        : _index_buffer{device.create_buffer<uint>(size)},
          _counter_buffer{device.create_buffer<uint>(counter_buffer_size)},
          _current_counter{counter_buffer_size} {
        _clear_counters = device.compile<1>([this] {
            _counter_buffer.write(dispatch_x(), 0u);
        });
    }
    [[nodiscard]] BufferView<uint> prepare_counter_buffer(CommandBuffer &command_buffer) noexcept {
        if (_current_counter == counter_buffer_size) {
            _current_counter = 0u;
            command_buffer << _clear_counters().dispatch(counter_buffer_size);
        }
        return _counter_buffer.view(_current_counter++, 1u);
    }
    [[nodiscard]] BufferView<uint> prepare_index_buffer(CommandBuffer &command_buffer) noexcept {
        return _index_buffer;
    }
};

class WavefrontPathTracingInstance final : public Integrator::Instance {

private:
    void _render_one_camera(CommandBuffer &command_buffer, Camera::Instance *camera) noexcept;

public:
    WavefrontPathTracingInstance(const WavefrontPathTracing *node, Pipeline &pipeline, CommandBuffer &cb) noexcept
        : Integrator::Instance{pipeline, cb, node} {}

    void render(Stream &stream) noexcept override {
        auto pt = node<WavefrontPathTracing>();
        auto command_buffer = stream.command_buffer();
        luisa::vector<float4> pixels;
        for (auto i = 0u; i < pipeline().camera_count(); i++) {
            auto camera = pipeline().camera(i);
            auto resolution = camera->film()->node()->resolution();
            auto pixel_count = resolution.x * resolution.y;
            pixels.resize(next_pow2(pixel_count) * 4u);
            auto film_path = camera->node()->file();
            LUISA_INFO(
                "Rendering to '{}' of resolution {}x{} at {}spp.",
                film_path.string(),
                resolution.x, resolution.y,
                camera->node()->spp());
            _render_one_camera(command_buffer, camera);
            camera->film()->download(command_buffer, pixels.data());
            command_buffer << compute::synchronize();
            if (film_path.extension() != ".exr") [[unlikely]] {
                LUISA_WARNING_WITH_LOCATION(
                    "Unexpected film file extension. "
                    "Changing to '.exr'.");
                film_path.replace_extension(".exr");
            }
            auto size = make_int2(resolution);
            const char *err = nullptr;
            SaveEXR(reinterpret_cast<const float *>(pixels.data()),
                    size.x, size.y, 4, false, film_path.string().c_str(), &err);
            if (err != nullptr) [[unlikely]] {
                LUISA_ERROR_WITH_LOCATION(
                    "Failed to save film to '{}'.",
                    film_path.string());
            }
        }
    }
};

unique_ptr<Integrator::Instance> WavefrontPathTracing::build(Pipeline &pipeline, CommandBuffer &cb) const noexcept {
    return luisa::make_unique<WavefrontPathTracingInstance>(this, pipeline, cb);
}

void WavefrontPathTracingInstance::_render_one_camera(
    CommandBuffer &command_buffer, Camera::Instance *camera) noexcept {

    auto &&device = camera->pipeline().device();

    camera->film()->clear(command_buffer);
    if (!pipeline().has_lighting()) [[unlikely]] {
        LUISA_WARNING_WITH_LOCATION(
            "No lights in scene. Rendering aborted.");
        return;
    }

    auto spp = camera->node()->spp();
    auto resolution = camera->film()->node()->resolution();
    auto pixel_count = resolution.x * resolution.y;
    PathStateSOA path_states{spectrum(), pixel_count};
    LightSampleSOA light_samples{spectrum(), pixel_count};

    sampler()->reset(command_buffer, resolution, pixel_count, spp);
    command_buffer.commit();

    using BufferRay = BufferVar<Ray>;
    using BufferHit = BufferVar<Hit>;

    LUISA_INFO("Compiling ray generation kernel.");
    auto generate_rays_shader = device.compile<1>([&](BufferUInt path_indices, BufferRay rays, UInt sample_id, Float4x4 c2w, Float time) noexcept {
        auto pixel_id = dispatch_x();
        auto pixel_coord = make_uint2(pixel_id % resolution.x, pixel_id / resolution.x);
        sampler()->start(pixel_coord, sample_id);
        auto camera_sample = camera->generate_ray(*sampler(), pixel_coord, time, c2w);
        auto swl = spectrum()->sample(*sampler());
        sampler()->save_state(pixel_id);
        rays.write(pixel_id, camera_sample.ray);
        path_states.write_swl(pixel_id, swl);
        path_states.write_beta(pixel_id, SampledSpectrum{swl.dimension(), camera_sample.weight});
        path_states.write_radiance(pixel_id, SampledSpectrum{swl.dimension()});
        path_states.write_pdf_bsdf(pixel_id, 1e16f);
        path_indices.write(pixel_id, pixel_id);
    });

    LUISA_INFO("Compiling intersection kernel.");
    auto intersect_shader = device.compile<1>([&](BufferUInt ray_count, BufferRay rays, BufferHit hits,
                                                  BufferUInt surface_queue, BufferUInt surface_queue_size,
                                                  BufferUInt light_queue, BufferUInt light_queue_size,
                                                  BufferUInt escape_queue, BufferUInt escape_queue_size) noexcept {
        auto ray_id = dispatch_x();
        $if(ray_id < ray_count.read(0u)) {
            auto ray = rays.read(ray_id);
            auto hit = pipeline().trace_closest(ray);
            hits.write(ray_id, hit);
            $if(!hit->miss()) {
                auto shape = pipeline().instance(hit.inst);
                $if(shape->has_surface()) {
                    auto queue_id = surface_queue_size.atomic(0u).fetch_add(1u);
                    surface_queue.write(queue_id, ray_id);
                };
                $if(shape->has_light()) {
                    auto queue_id = light_queue_size.atomic(0u).fetch_add(1u);
                    light_queue.write(queue_id, ray_id);
                };
            }
            $else {
                if (pipeline().environment()) {
                    auto queue_id = escape_queue_size.atomic(0u).fetch_add(1u);
                    escape_queue.write(queue_id, ray_id);
                }
            };
        };
    });

    Callable balanced_heuristic = [](Float pdf_a, Float pdf_b) noexcept {
        return ite(pdf_a > 0.0f, pdf_a / (pdf_a + pdf_b), 0.0f);
    };

    LUISA_INFO("Compiling environment evaluation kernel.");
    auto evaluate_miss_shader = device.compile<1>([&](BufferUInt path_indices, BufferRay rays,
                                                      BufferUInt queue, BufferUInt queue_size,
                                                      Float3x3 e2w, Float time) noexcept {
        if (pipeline().environment()) {
            auto queue_id = dispatch_x();
            $if(queue_id < queue_size.read(0u)) {
                auto ray_id = queue.read(queue_id);
                auto wi = rays.read(ray_id)->direction();
                auto path_id = path_indices.read(ray_id);
                auto swl = path_states.read_swl(path_id);
                auto pdf_bsdf = path_states.read_pdf_bsdf(path_id);
                auto beta = path_states.read_beta(path_id);
                auto Li = path_states.read_radiance(path_id);
                auto eval = light_sampler()->evaluate_miss(wi, e2w, swl, time);
                auto mis_weight = balanced_heuristic(pdf_bsdf, eval.pdf);
                Li += beta * eval.L * mis_weight;
                path_states.write_radiance(path_id, Li);
            };
        }
    });

    LUISA_INFO("Compiling light evaluation kernel.");
    auto evaluate_light_shader = device.compile<1>([&](BufferUInt path_indices, BufferRay rays, BufferHit hits,
                                                       BufferUInt queue, BufferUInt queue_size, Float time) noexcept {
        if (!pipeline().lights().empty()) {
            auto queue_id = dispatch_x();
            $if(queue_id < queue_size.read(0u)) {
                auto ray_id = queue.read(queue_id);
                auto ray = rays.read(ray_id);
                auto hit = hits.read(ray_id);
                auto path_id = path_indices.read(ray_id);
                auto swl = path_states.read_swl(path_id);
                auto pdf_bsdf = path_states.read_pdf_bsdf(path_id);
                auto beta = path_states.read_beta(path_id);
                auto Li = path_states.read_radiance(path_id);
                auto it = pipeline().interaction(ray, hit);
                auto eval = light_sampler()->evaluate_hit(*it, ray->origin(), swl, time);
                auto mis_weight = balanced_heuristic(pdf_bsdf, eval.pdf);
                Li += beta * eval.L * mis_weight;
                path_states.write_radiance(path_id, Li);
            };
        }
    });

    LUISA_INFO("Compiling light sampling kernel.");
    auto sample_light_shader = device.compile<1>([&](BufferUInt path_indices, BufferRay rays, BufferHit hits,
                                                     BufferUInt queue, BufferUInt queue_size, Float3x3 e2w, Float time) noexcept {
        auto queue_id = dispatch_x();
        $if(queue_id < queue_size.read(0u)) {
            auto ray_id = queue.read(queue_id);
            auto ray = rays.read(ray_id);
            auto hit = hits.read(ray_id);
            auto it = pipeline().interaction(ray, hit);
            auto path_id = path_indices.read(ray_id);
            auto swl = path_states.read_swl(path_id);
            sampler()->load_state(path_id);
            auto light_sample = light_sampler()->sample(*sampler(), *it, e2w, swl, time);
            sampler()->save_state(path_id);
            // trace shadow ray
            auto shadow_ray = it->spawn_ray(light_sample.wi, light_sample.distance);
            auto occluded = pipeline().intersect_any(shadow_ray);
            light_samples.write_emission(queue_id, ite(occluded, 0.f, 1.f) * light_sample.eval.L);
            light_samples.write_pdf(queue_id, ite(occluded, 0.f, light_sample.eval.pdf));
            light_samples.write_wi(queue_id, shadow_ray->direction());
        };
    });

    LUISA_INFO("Compiling surface evaluation kernel.");
    auto evaluate_surface_shader = device.compile<1>([&](BufferUInt path_indices, UInt trace_depth, BufferUInt queue, BufferUInt queue_size,
                                                         BufferRay in_rays, BufferHit in_hits, BufferRay out_rays,
                                                         BufferUInt out_queue, BufferUInt out_queue_size, Float time) noexcept {
        auto queue_id = dispatch_x();
        $if(queue_id < queue_size.read(0u)) {
            auto ray_id = queue.read(queue_id);
            auto ray = in_rays.read(ray_id);
            auto hit = in_hits.read(ray_id);
            auto it = pipeline().interaction(ray, hit);
            auto path_id = path_indices.read(ray_id);
            sampler()->load_state(path_id);
            auto Li = path_states.read_radiance(path_id);
            auto swl = path_states.read_swl(path_id);
            auto beta = path_states.read_beta(path_id);
            auto cos_theta_o = it->wo_local().z;
            auto surface_tag = it->shape()->surface_tag();
            auto pdf_bsdf = def(0.f);
            SampledSpectrum eta_scale{swl.dimension(), 1.f};
            pipeline().dynamic_dispatch_surface(surface_tag, [&](auto surface) noexcept {
                // apply normal map
                if (auto normal_map = surface->normal()) {
                    auto normal_local = 2.f * normal_map->evaluate(*it, time).xyz() - 1.f;
                    auto normal = it->shading().local_to_world(normal_local);
                    it->set_shading(Frame::make(normal, it->shading().u()));
                }
                // apply alpha map
                auto alpha_skip = def(false);
                if (auto alpha_map = surface->alpha()) {
                    auto alpha = alpha_map->evaluate(*it, time).x;
                    auto u_alpha = sampler()->generate_1d();
                    alpha_skip = alpha < u_alpha;
                }

                $if(alpha_skip) {
                    ray = it->spawn_ray(ray->direction());
                    pdf_bsdf = 1e16f;
                }
                $else {
                    // create closure
                    auto closure = surface->closure(*it, swl, time);

                    // direct lighting
                    auto pdf_light = light_samples.read_pdf(queue_id);
                    $if(pdf_light > 0.0f) {
                        auto Ld = light_samples.read_emission(queue_id);
                        auto wi = light_samples.read_wi(queue_id);
                        auto eval = closure->evaluate(wi);
                        auto cos_theta_i = dot(it->shading().n(), wi);
                        auto is_trans = cos_theta_i * cos_theta_o < 0.f;
                        auto mis_weight = balanced_heuristic(pdf_light, eval.pdf);
                        Li += mis_weight / pdf_light *
                              abs_dot(it->shading().n(), wi) *
                              beta * eval.f * Ld;
                    };

                    // sample material
                    auto sample = closure->sample(*sampler());
                    auto cos_theta_i = dot(sample.wi, it->shading().n());
                    ray = it->spawn_ray(sample.wi);
                    pdf_bsdf = sample.eval.pdf;
                    auto w = ite(sample.eval.pdf > 0.0f, abs(cos_theta_i) / sample.eval.pdf, 0.f);
                    beta *= sample.eval.f * w;

                    // specular transmission, consider eta scale
                    $if(cos_theta_i * cos_theta_o < 0.f &
                        max(sample.eval.alpha.x, sample.eval.alpha.y) < .05f) {
                        auto entering = cos_theta_o > 0.f;
                        for (auto i = 0u; i < swl.dimension(); i++) {
                            eta_scale[i] = ite(
                                entering,
                                sqr(sample.eval.eta[i]),
                                sqr(1.f / sample.eval.eta[i]));
                        }
                    };
                };
            });
            $if(beta.any([](auto b) noexcept { return b > 0.f; })) {
                auto q = max(swl.cie_y(beta * eta_scale), .05f);
                auto rr_depth = node<WavefrontPathTracing>()->rr_depth();
                auto rr_threshold = node<WavefrontPathTracing>()->rr_threshold();
                // rr
                $if(trace_depth >= rr_depth & q < rr_threshold) {
                    $if(sampler()->generate_1d() < q) {
                        beta *= 1.f / q;
                        auto out_queue_id = out_queue_size.atomic(0u).fetch_add(1u);
                        out_queue.write(out_queue_id, path_id);
                        out_rays.write(out_queue_id, ray);
                    };
                }
                $else {
                    auto out_queue_id = out_queue_size.atomic(0u).fetch_add(1u);
                    out_queue.write(out_queue_id, path_id);
                    out_rays.write(out_queue_id, ray);
                };
            };
            sampler()->save_state(path_id);
            path_states.write_radiance(path_id, Li);
            path_states.write_beta(path_id, beta);
            path_states.write_pdf_bsdf(path_id, pdf_bsdf);
        };
    });

    LUISA_INFO("Compiling accumulation kernel.");
    auto accumulate_shader = device.compile<1>([&](Float shutter_weight) noexcept {
        auto pixel_id = dispatch_x();
        auto pixel_coord = make_uint2(pixel_id % resolution.x, pixel_id / resolution.x);
        auto swl = path_states.read_swl(pixel_id);
        auto Li = path_states.read_radiance(pixel_id);
        camera->film()->accumulate(pixel_coord, swl.srgb(Li * shutter_weight));
    });

    LUISA_INFO("Compiled all wavefront kernels.");
    RayQueue path_queue{device, pixel_count};
    RayQueue out_path_queue{device, pixel_count};
    RayQueue surface_queue{device, pixel_count};
    RayQueue light_queue{device, pixel_count};
    RayQueue miss_queue{device, pixel_count};
    auto ray_buffer = device.create_buffer<Ray>(pixel_count);
    auto ray_buffer_out = device.create_buffer<Ray>(pixel_count);
    auto hit_buffer = device.create_buffer<Hit>(pixel_count);
    auto pixel_count_buffer = device.create_buffer<uint>(1u);
    auto shutter_samples = camera->node()->shutter_samples();
    command_buffer << pixel_count_buffer.copy_from(&pixel_count)
                   << synchronize();

    LUISA_INFO("Rendering started.");
    Clock clock;
    auto sample_id = 0u;
    for (auto s : shutter_samples) {
        auto time = s.point.time;
        pipeline().update_geometry(command_buffer, time);
        auto camera_to_world = camera->node()->transform()->matrix(time);
        auto env_to_world = make_float3x3(1.f);
        if (auto env = pipeline().environment()) {
            env_to_world = transpose(inverse(make_float3x3(
                env->node()->transform()->matrix(s.point.time))));
        }
        for (auto i = 0u; i < s.spp; i++) {
            auto path_indices = path_queue.prepare_index_buffer(command_buffer);
            auto path_count = pixel_count_buffer.view();
            auto rays = ray_buffer.view();
            auto hits = hit_buffer.view();
            auto out_rays = ray_buffer_out.view();
            command_buffer << generate_rays_shader(path_indices, rays, sample_id, camera_to_world, time).dispatch(pixel_count);
            for (auto depth = 0u; depth < node<WavefrontPathTracing>()->max_depth(); depth++) {
                auto surface_indices = surface_queue.prepare_index_buffer(command_buffer);
                auto surface_count = surface_queue.prepare_counter_buffer(command_buffer);
                auto light_indices = light_queue.prepare_index_buffer(command_buffer);
                auto light_count = light_queue.prepare_counter_buffer(command_buffer);
                auto miss_indices = miss_queue.prepare_index_buffer(command_buffer);
                auto miss_count = miss_queue.prepare_counter_buffer(command_buffer);
                auto out_path_indices = out_path_queue.prepare_index_buffer(command_buffer);
                auto out_path_count = out_path_queue.prepare_counter_buffer(command_buffer);
                command_buffer << intersect_shader(path_count, rays, hits, surface_indices, surface_count,
                                                   light_indices, light_count, miss_indices, miss_count)
                                      .dispatch(pixel_count);
                if (pipeline().environment()) {
                    command_buffer << evaluate_miss_shader(path_indices, rays, miss_indices, miss_count, env_to_world, time)
                                          .dispatch(pixel_count);
                }
                if (!pipeline().lights().empty()) {
                    command_buffer << evaluate_light_shader(path_indices, rays, hits, light_indices, light_count, time)
                                          .dispatch(pixel_count);
                }
                command_buffer << sample_light_shader(path_indices, rays, hits, surface_indices, surface_count, env_to_world, time)
                                      .dispatch(pixel_count)
                               << evaluate_surface_shader(path_indices, depth, surface_indices, surface_count,
                                                          rays, hits, out_rays, out_path_indices, out_path_count, time)
                                      .dispatch(pixel_count);
                path_indices = out_path_indices;
                path_count = out_path_count;
                std::swap(rays, out_rays);
                std::swap(path_queue, out_path_queue);
            }
            command_buffer << accumulate_shader(s.point.weight).dispatch(pixel_count)
                           << commit();
            sample_id++;
        }
    }
    command_buffer << synchronize();
    LUISA_INFO("Rendering finished in {} ms.", clock.toc());
}

}// namespace luisa::render

LUISA_RENDER_MAKE_SCENE_NODE_PLUGIN(luisa::render::WavefrontPathTracing)
