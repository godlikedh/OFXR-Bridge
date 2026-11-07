// Actual signed analytical AMD provider, two independent temporal contexts.
void test_fsr_provider_stereo(D3D12WarpFixture& fixture) {
    constexpr UINT width=256,height=128;
    auto color=create_source_texture(fixture,width,height);
    auto pattern=translated_motion_pattern(width,height,{0,0});
    // Distinct eye colours expose cross-eye output/history contamination.
    for(UINT eye=0;eye<2;++eye)for(size_t i=0;i<pattern[eye].size();i+=4) {
        pattern[eye][i]=eye?32:192;
        pattern[eye][i+1]=eye?160:48;
        pattern[eye][i+2]=eye?224:64;
        pattern[eye][i+3]=255;
    }
    upload_pattern(fixture,color.Get(),pattern);
    auto depth=create_native_test_depth(fixture,width,height);
    auto motion=create_and_upload_game_motion_field(fixture,width,height,
        [](UINT,UINT,UINT){return std::array<float,2>{0,0};});
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* commands) {
        auto barrier=transition_barrier(color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
        commands->ResourceBarrier(1,&barrier);
    });
    xrfg::D3D12FsrFrameGenerator generator;
    require_hresult(generator.initialize(fixture.device(),color->GetDesc(),DXGI_FORMAT_R8G8B8A8_UNORM),"FSR initialize");
    std::array<xrfg::D3D12FsrEyeInput,2> eyes{};
    for(UINT eye=0;eye<2;++eye) {
        auto& input=eyes[eye];input.color=color.Get();input.color_slice=input.output_slice=eye;
        input.color_rect=input.depth_rect=input.motion_rect={0,0,width,height};
        input.depth=depth.Get();input.depth_state=D3D12_RESOURCE_STATE_COMMON;
        input.motion_vectors=motion.Get();input.motion_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        input.frame_time_delta_ms=22.2222F;input.camera_fov_vertical=1.4F;input.fsr_upscale_source=true;
    }
    const auto prepare=[&] {
        const auto deadline=GetTickCount64()+90000;
        HRESULT result{};
        do {
            result=generator.prepare(eyes);
            if(result!=DXGI_ERROR_WAS_STILL_DRAWING)break;
            require(GetTickCount64()<deadline,"asynchronous AMD initialization timed out");
            Sleep(1);
        }while(true);
        require_hresult(result,"asynchronous AMD initialization");
        require(!generator.requires_idle_reconfigure(eyes),"prepared stereo contexts match requested inputs");
    };
    const auto started=GetTickCount64();
    require(generator.prepare(eyes)==DXGI_ERROR_WAS_STILL_DRAWING,"first FG call starts background initialization");
    require(GetTickCount64()-started<1000,"starting FG initialization blocked rendering");
    prepare();
    xrfg::D3D12FsrPerformance performance;
    performance.initialize(fixture.device(), fixture.queue());
    // Abandoned and incomplete recordings may never resolve unwritten queries
    // or reuse old GPU measurements as a successful generation sample.
    for (const auto result : {E_FAIL, S_OK}) {
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* commands) {
            require(performance.begin(commands, fixture.queue(), 99, 2, 15), "begin incomplete sample");
            performance.mark(commands);
            performance.end_recording(commands, result);
            performance.submitted();
        });
        require(!performance.harvest(fixture.completion_fence(), fixture.last_submitted_value()),
            "incomplete sample must not resolve or read stale timestamps");
        performance.discard_recording();
    }
    for(unsigned frame=0;frame<3;++frame) {
        ID3D12Resource* generated{};
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* commands) {
            require(performance.begin(commands, fixture.queue(), frame + 1, 2, 15), "begin stereo performance sample");
            performance.mark(commands); // No foveated conversion in this fixture.
            const auto result = generator.dispatch(commands,eyes,&generated,&performance);
            require_hresult(result,"real AMD FG dispatch");
            performance.end_recording(commands, result);
            performance.submitted();
            require(!performance.harvest(fixture.completion_fence(), fixture.last_submitted_value() + 1),
                "performance readback must wait for the existing fence");
            require(!performance.begin(commands, fixture.queue(), 999, 2, 15),
                "pending timestamp storage must not be recycled");
        });
        xrfg::D3D12FsrPerformance::Sample measured{};
        require(performance.harvest(fixture.completion_fence(), fixture.last_submitted_value(), &measured),
            "harvest real AMD timestamps after existing completion");
        require(measured.points == 11 && measured.total_us > 0, "two generated eyes require eleven timing boundaries");
        UINT64 sum{};
        for (UINT i = 1; i < measured.points; ++i) sum += measured.gpu_us[i];
        require(sum <= measured.total_us && measured.total_us - sum < measured.points,
            "GPU stage durations partition the measured span (microsecond rounding)");
        require(!performance.harvest(fixture.completion_fence(), fixture.last_submitted_value()),
            "completed sample must be consumed only once");
        std::cout << "FSR performance frame=" << frame << " points=" << measured.points
                  << " gpu_us=" << measured.total_us << '\n';
        require(generated!=nullptr,"FSR output exists");
        const auto actual=readback_pattern(fixture,generated,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        for(UINT eye=0;eye<2;++eye) {
            const auto error=mean_absolute_rgb_error(actual,pattern,width,height,eye,16);
            std::cout<<"AMD analytical FSR frame="<<frame<<" eye="<<eye<<" MAE="<<error<<'\n';
            require(error<=1.0,"FSR static eye output changed or swapped");
            require(generator.output_is_current(eye)==(frame==0),"FSR reset/interpolation state");
        }
    }
    generator.reset();
    require(generator.prepare(eyes)==DXGI_ERROR_WAS_STILL_DRAWING,"reset starts a fresh asynchronous context");
    // Change the requested flags while the old configuration is in flight.
    // Completion may never install that stale context for the new inputs.
    for(auto& eye:eyes)eye.motion_vectors_jittered=true;
    prepare();
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* commands) {
        ID3D12Resource* generated{};
        require_hresult(generator.dispatch(commands,eyes,&generated),"dispatch after asynchronous reconfiguration");
        require(generated&&generator.output_is_current(0)&&generator.output_is_current(1),"new contexts reset both eyes");
    });
    std::cout<<"AMD asynchronous initialization: stereo output and in-flight configuration change passed.\n";
}
