ComPtr<ID3D12Resource> fsr_mono_copy(D3D12WarpFixture& fixture, ID3D12Resource* source,
    UINT slice, D3D12_RESOURCE_STATES state) {
    auto desc=source->GetDesc();desc.DepthOrArraySize=1;desc.Flags=D3D12_RESOURCE_FLAG_NONE;
    const auto hp=heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> result;
    require_hresult(fixture.device()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&result)),"mono guide allocation");
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        D3D12_RESOURCE_BARRIER b[]{transition_barrier(source,state,D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(result.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST)};
        list->ResourceBarrier(2,b);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=source;src.SubresourceIndex=slice;dst.pResource=result.Get();
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        for(auto& barrier:b)std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
        list->ResourceBarrier(2,b);
    });
    return result;
}

void test_fsr_shared_scratch(D3D12WarpFixture& fixture) {
    constexpr UINT width=64,height=32;
    auto depth_array=create_native_test_depth(fixture,width,height,
        [](UINT eye,UINT,UINT){return eye?.75F:.25F;});
    auto motion_array=create_and_upload_game_motion_field(fixture,width,height,
        [](UINT eye,UINT,UINT){return std::array<float,2>{eye?2.F:1.F,0};});
    std::array<xrfg::DlssMotionVectorFrame,2> frames;
    for(UINT i=0;i<2;++i) {
        auto& f=frames[i];
        f.depth=fsr_mono_copy(fixture,depth_array.Get(),i,D3D12_RESOURCE_STATE_COMMON);
        f.motion_vectors=fsr_mono_copy(fixture,motion_array.Get(),i,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        f.depth_width=f.motion_width=width;f.depth_height=f.motion_height=height;
        f.resource_state=f.depth_resource_state=D3D12_RESOURCE_STATE_COMMON;
        f.source_view_mask=3;f.serial=2;f.camera_frame_token=2;f.camera_frame_token_valid=true;
        f.camera_matrices_valid=true;f.camera_view_to_clip[0]=f.camera_view_to_clip[5]=1;
        f.camera_view_to_clip[11]=1;f.scale_x=float(width);f.scale_y=float(height);
    }
    const xrfg::D3D12FieldOfView fov{-std::atan(1.F),std::atan(1.F),std::atan(1.F),-std::atan(1.F)};
    auto scratch=xrfg::D3D12FsrFoveation::make_scratch();
    std::array<std::unique_ptr<xrfg::D3D12FsrFoveation>,3> slots;
    std::array<ComPtr<ID3D12CommandAllocator>,3> allocators;
    std::array<ComPtr<ID3D12GraphicsCommandList>,3> lists;
    std::array<ComPtr<ID3D12Resource>,3> readbacks;
    std::array<xrfg::D3D12FsrEyeInput,3> inputs;
    // Record ALL lists before submitting ANY. Distinct source values catch
    // overwritten per-slot descriptors; the third list replaces scratch size.
    for(UINT i=0;i<3;++i) {
        slots[i]=std::make_unique<xrfg::D3D12FsrFoveation>(scratch);
        require_hresult(fixture.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&allocators[i])),"scratch allocator");
        require_hresult(fixture.device()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[i].Get(),
            nullptr,IID_PPV_ARGS(&lists[i])),"scratch list");
        const auto& current=frames[i%2];auto previous=current;previous.serial=1;previous.camera_frame_token=1;
        auto& input=inputs[i];input.color_rect={0,0,i==2?width/2:width,i==2?height/2:height};
        require_hresult(slots[i]->convert(lists[i].Get(),0,current,previous,fov,fov,input),"shared guide conversion");
        if(i==1)require(input.depth==inputs[0].depth&&input.motion_vectors==inputs[0].motion_vectors,
            "same-queue work slots must share scratch allocations");
        if(i==2)require(input.depth!=inputs[0].depth,"size change replaces scratch without releasing old slots");
        require(input.borrow_owned_guides,"converter authorizes owned guide consumption");
        if(i==1) {
            auto other=input;
            require_hresult(slots[i]->convert(lists[i].Get(),1,current,previous,fov,fov,other),"second eye scratch");
            require(other.depth!=input.depth&&other.motion_vectors!=input.motion_vectors,"eyes must never alias scratch");
        }
        readbacks[i]=create_buffer(fixture,D3D12_HEAP_TYPE_READBACK,1024,D3D12_RESOURCE_STATE_COPY_DEST);
        for(UINT kind=0;kind<2;++kind) {
            auto* resource=kind?input.motion_vectors:input.depth;
            auto b=transition_barrier(resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
            lists[i]->ResourceBarrier(1,&b);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=resource;
            dst.pResource=readbacks[i].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset=kind*512;
            dst.PlacedFootprint.Footprint={resource->GetDesc().Format,1,1,1,256};
            const D3D12_BOX box{0,0,0,1,1,1};
            lists[i]->CopyTextureRegion(&dst,0,0,0,&src,&box);
            std::swap(b.Transition.StateBefore,b.Transition.StateAfter);lists[i]->ResourceBarrier(1,&b);
        }
        require_hresult(lists[i]->Close(),"close scratch recording");
    }
    ID3D12CommandList* submitted[]{lists[0].Get(),lists[1].Get(),lists[2].Get()};
    fixture.queue()->ExecuteCommandLists(3,submitted);
    fixture.execute_and_wait([](ID3D12GraphicsCommandList*){});
    for(UINT i=0;i<3;++i) {
        void* mapped{};require_hresult(readbacks[i]->Map(0,nullptr,&mapped),"scratch readback");
        const auto actual=*static_cast<float*>(mapped);
        const auto* motion=reinterpret_cast<float*>(static_cast<std::byte*>(mapped)+512);
        const auto mx=motion[0],my=motion[1];readbacks[i]->Unmap(0,nullptr);
        require(std::abs(actual-(i%2?.75F:.25F))<1e-5F,"shared scratch used another slot's source/size");
        require(std::abs(mx-(i%2?2.F:1.F))<1e-5F&&std::abs(my)<1e-5F,"shared motion scratch corrupted");
    }
    std::cout<<"FSR scratch: queued slot reuse, descriptor isolation and resize lifetime passed.\n";
}

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
    // Exercise direct consumption of the same mono formats the converter owns.
    // Output must match the copied path; array inputs must still fall back to copies.
    for(auto& eye:eyes)eye.borrow_owned_guides=true;
    require(!generator.requires_idle_reconfigure(eyes),"array guides cannot enable borrowing");
    std::array<ComPtr<ID3D12Resource>,2> mono_depth,mono_motion;
    for(UINT eye=0;eye<2;++eye) {
        mono_depth[eye]=fsr_mono_copy(fixture,depth.Get(),eye,D3D12_RESOURCE_STATE_COMMON);
        mono_motion[eye]=fsr_mono_copy(fixture,motion.Get(),eye,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        eyes[eye].depth=mono_depth[eye].Get();eyes[eye].motion_vectors=mono_motion[eye].Get();
        eyes[eye].motion_state=D3D12_RESOURCE_STATE_COMMON;
    }
    require(generator.requires_idle_reconfigure(eyes),"owned guides drop private copy allocations");
    prepare();
    for(unsigned frame=0;frame<3;++frame) {
        ID3D12Resource* generated{};
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* commands) {
            require_hresult(generator.dispatch(commands,eyes,&generated),"borrowed AMD guide dispatch");
        });
        const auto actual=readback_pattern(fixture,generated,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        for(UINT eye=0;eye<2;++eye)require(mean_absolute_rgb_error(actual,pattern,width,height,eye,16)<=1.0,
            "borrowed guide output differs from copied input path");
    }
    // Disallow borrowing cropped views, then return to the owned full image.
    auto cropped=eyes;cropped[0].depth_rect.width-=1;
    require(generator.requires_idle_reconfigure(cropped),"cropped guides require copy storage");
    std::cout<<"AMD borrowed guides: stereo output, repeated state restoration and array fallback passed.\n";
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
