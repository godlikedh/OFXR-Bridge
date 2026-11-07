# FSR guide memory and queue ownership

The final eye conversion owns R32 depth and RG32 motion textures. These are
scratch data: the recorded AMD preparation/generation consumes them before
the next conversion on the same synthesizer queue. They are not presentation
images or game-owned inputs.

`D3D12FsrFoveation::Scratch` shares one pair per eye across that synthesizer's
work slots. Each slot keeps its own descriptor heap and strong references to
the allocation used by its recording. A size change replaces the shared pair;
older slots retain the previous allocation until their existing fences retire.
Different synthesizers and different eyes never share scratch textures.

Recording must be serial and submission must preserve recording order on one
queue. Every conversion and borrowed AMD dispatch returns scratch to COMMON.
Abandoned recordings therefore leave no speculative persistent state. No extra
CPU waits, queue waits, submissions or fence signals are introduced.

`borrow_owned_guides` is an internal opt-in only for these pinned resources.
The generator also requires full single-mip mono R32/RG32 textures in COMMON.
Other guides keep private copies. Color remains copied, preserving its existing
format, crop and transfer-function handling. Async SDK workloads remain disabled.

The WARP regression records three conversions before submitting any, checks
distinct source depth/motion values, eye separation and a pending size change.
The signed analytical AMD provider checks repeated borrowed dispatches, output
equivalence and resource states with the D3D12 debug layer. These tests do not
validate ML-provider performance or MSFS/headset behavior on AMD hardware.
