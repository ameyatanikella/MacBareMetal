// mtlrun: compile one Metal compute kernel from source, run it on the GPU,
// print the results, and optionally save the driver's compiled form.
//
//   mtlrun <kernel.metal> [--archive out.bin] [--n 16] [--quiet]
//
// The kernel must be named `k` and take three buffers (two inputs, one
// output) of 32-bit elements plus the thread position. Inputs are filled with
// a[i] = i + 1 and b[i] = 2 * i + 3 as raw 32-bit words; with --float the same
// values are stored as floats.
//
// Everything here goes through public Metal API. The point is to give the
// GPU driver a tiny, fully known workload that we can vary one step at a
// time, and to keep the compiled result for comparison.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static void die(NSString *what, NSError *err)
{
    fprintf(stderr, "mtlrun: %s%s%s\n", what.UTF8String, err ? ": " : "",
            err ? err.localizedDescription.UTF8String : "");
    exit(1);
}

int main(int argc, const char **argv)
{
    @autoreleasepool {
        NSString *srcPath = nil, *archivePath = nil;
        NSUInteger n = 16;
        BOOL quiet = NO, isFloat = NO;

        for (int i = 1; i < argc; i++) {
            NSString *a = @(argv[i]);

            if ([a isEqual:@"--archive"] && i + 1 < argc)
                archivePath = @(argv[++i]);
            else if ([a isEqual:@"--n"] && i + 1 < argc)
                n = (NSUInteger)atoi(argv[++i]);
            else if ([a isEqual:@"--quiet"])
                quiet = YES;
            else if ([a isEqual:@"--float"])
                isFloat = YES;
            else
                srcPath = a;
        }
        if (!srcPath || !n) {
            fprintf(stderr, "usage: mtlrun <kernel.metal> [--archive out.bin] [--n N] [--float] [--quiet]\n");
            return 2;
        }

        NSError *err = nil;
        NSString *src = [NSString stringWithContentsOfFile:srcPath
                                                  encoding:NSUTF8StringEncoding
                                                     error:&err];
        if (!src)
            die(@"cannot read source", err);

        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev)
            die(@"no Metal device", nil);
        if (!quiet)
            printf("device: %s (registryID 0x%llx)\n", dev.name.UTF8String, dev.registryID);

        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
        if (!lib)
            die(@"shader compile failed", err);

        id<MTLFunction> fn = [lib newFunctionWithName:@"k"];
        if (!fn)
            die(@"kernel function `k` not found", nil);

        MTLComputePipelineDescriptor *desc = [MTLComputePipelineDescriptor new];
        desc.computeFunction = fn;

        if (archivePath) {
            id<MTLBinaryArchive> ar =
                [dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&err];
            if (!ar)
                die(@"cannot create binary archive", err);
            if (![ar addComputePipelineFunctionsWithDescriptor:desc error:&err])
                die(@"cannot add pipeline to archive", err);
            if (![ar serializeToURL:[NSURL fileURLWithPath:archivePath] error:&err])
                die(@"cannot serialize archive", err);
        }

        id<MTLComputePipelineState> pso =
            [dev newComputePipelineStateWithDescriptor:desc
                                               options:MTLPipelineOptionNone
                                            reflection:nil
                                                 error:&err];
        if (!pso)
            die(@"pipeline creation failed", err);

        NSUInteger bytes = n * sizeof(uint32_t);
        id<MTLBuffer> a = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        id<MTLBuffer> b = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        id<MTLBuffer> o = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];

        for (NSUInteger i = 0; i < n; i++) {
            if (isFloat) {
                ((float *)a.contents)[i] = (float)(i + 1);
                ((float *)b.contents)[i] = (float)(2 * i + 3);
            } else {
                ((uint32_t *)a.contents)[i] = (uint32_t)(i + 1);
                ((uint32_t *)b.contents)[i] = (uint32_t)(2 * i + 3);
            }
            ((uint32_t *)o.contents)[i] = 0xdeadbeef;
        }

        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];

        [enc setComputePipelineState:pso];
        [enc setBuffer:a offset:0 atIndex:0];
        [enc setBuffer:b offset:0 atIndex:1];
        [enc setBuffer:o offset:0 atIndex:2];

        NSUInteger w = pso.threadExecutionWidth;
        [enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(w < n ? w : n, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        if (cb.status != MTLCommandBufferStatusCompleted)
            die(@"command buffer did not complete", cb.error);

        // One line, machine readable: "out <hex words...>"
        printf("out");
        for (NSUInteger i = 0; i < n; i++)
            printf(" %08x", ((uint32_t *)o.contents)[i]);
        printf("\n");
    }
    return 0;
}
