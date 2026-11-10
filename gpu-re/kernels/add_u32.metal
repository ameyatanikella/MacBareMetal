#include <metal_stdlib>
using namespace metal;

kernel void k(device const uint *a [[buffer(0)]],
              device const uint *b [[buffer(1)]],
              device uint *o [[buffer(2)]],
              uint id [[thread_position_in_grid]])
{
    o[id] = a[id] + b[id];
}
