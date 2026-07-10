#include <stdint.h>
#include <stdlib.h>

#include <vapoursynth/VapourSynth4.h>
#include <vapoursynth/VSHelper4.h>


enum BlendDirection {
   bdNext,
   bdPrev,
   bdBoth
};


typedef struct {
   VSNode *node;
   VSNode *altnode;
   float luma_thresh;
   int variation;
   int conservative_mask;
   int interlaced;
   int block_width;
   int block_height;
   int block_width_uv;
   int block_height_uv;
   int blocks_x;
   int blocks_y;

   const VSVideoInfo *vi;
   float relativeframediff;
   int offset;
} BifrostData;


static void applyBlockRainbowMask(const uint8_t *srcp_u, const uint8_t *srcp_v,
                                  const uint8_t *srcc_u, const uint8_t *srcc_v,
                                  const uint8_t *srcn_u, const uint8_t *srcn_v,
                                  uint8_t *restrict dst_u, uint8_t *restrict dst_v,
                                  int block_width_uv, int block_height_uv, ptrdiff_t stride_uv, int blenddirection) {

   for (int y = 0; y < block_height_uv; y++) {
      if (blenddirection == bdNext) {
         for (int x = 0; x < block_width_uv; x++) {
            if (dst_v[x]) {
               dst_u[x] = (srcc_u[x]+srcn_u[x]+1) >> 1;
               dst_v[x] = (srcc_v[x]+srcn_v[x]+1) >> 1;
            } else {
               dst_u[x] = srcc_u[x];
               dst_v[x] = srcc_v[x];
            }
         }
      } else if (blenddirection == bdPrev) {
         for (int x = 0; x < block_width_uv; x++) {
            if (dst_v[x]) {
               dst_u[x] = (srcc_u[x]+srcp_u[x]+1) >> 1;
               dst_v[x] = (srcc_v[x]+srcp_v[x]+1) >> 1;
            } else {
               dst_u[x] = srcc_u[x];
               dst_v[x] = srcc_v[x];
            }
         }
      } else if (blenddirection == bdBoth) {
         for (int x = 0; x < block_width_uv; x++) {
            if (dst_v[x]) {
               dst_u[x] = (2*srcc_u[x]+srcp_u[x]+srcn_u[x]+3) >> 2;
               dst_v[x] = (2*srcc_v[x]+srcp_v[x]+srcn_v[x]+3) >> 2;
            } else {
               dst_u[x] = srcc_u[x];
               dst_v[x] = srcc_v[x];
            }
         }
      }

      srcp_u += stride_uv;
      srcp_v += stride_uv;

      srcc_u += stride_uv;
      srcc_v += stride_uv;

      srcn_u += stride_uv;
      srcn_v += stride_uv;

      dst_u += stride_uv;
      dst_v += stride_uv;
   }
}


static void processBlockRainbowMask(uint8_t *restrict dst_u, uint8_t *restrict dst_v,
                                    int block_width_uv, int block_height_uv, ptrdiff_t stride_uv, int conservative_mask) {

   // Maybe needed later.
   uint8_t *tmp = dst_v;

   //denoise mask, remove marked pixels with no horizontal marked neighbors
   for (int y = 0; y < block_height_uv; y++) {
      dst_v[0] = dst_u[0] && dst_u[1];
      for (int x = 1; x < block_width_uv - 1; x++) {
         dst_v[x] = dst_u[x] && (dst_u[x-1] || dst_u[x+1]);
      }
      dst_v[block_width_uv - 1] = dst_u[block_width_uv - 1] && dst_u[block_width_uv - 2];

      dst_u += stride_uv;
      dst_v += stride_uv;
   }

   //expand mask vertically
   if (!conservative_mask) {
      dst_v = tmp;

      for (int x = 0; x < block_width_uv; x++) {
         dst_v[x] = dst_v[x] || dst_v[x + stride_uv];
      }

      dst_v += stride_uv;

      for (int y = 1; y < block_height_uv - 1; y++) {
         for (int x = 0; x < block_width_uv; x++) {
            dst_v[x] = dst_v[x] || (dst_v[x + stride_uv] && dst_v[x - stride_uv]);
         }

         dst_v += stride_uv;
      }

      for (int x = 0; x < block_width_uv; x++) {
         dst_v[x] = dst_v[x] || dst_v[x - stride_uv];
      }
   }
}


static void makeBlockRainbowMask(const uint8_t *srcp_u, const uint8_t *srcp_v,
                                 const uint8_t *srcc_u, const uint8_t *srcc_v,
                                 const uint8_t *srcn_u, const uint8_t *srcn_v,
                                 uint8_t *restrict dst_u, uint8_t *restrict dst_v,
                                 int block_width_uv, int block_height_uv, ptrdiff_t stride_uv, int variation) {

   for (int y = 0; y < block_height_uv; y++) {
      for (int x = 0; x < block_width_uv; x++) {
         uint8_t up = srcp_u[x];
         uint8_t uc = srcc_u[x];
         uint8_t un = srcn_u[x];

         uint8_t vp = srcp_v[x];
         uint8_t vc = srcc_v[x];
         uint8_t vn = srcn_v[x];

         int ucup = uc-up;
         int ucun = uc-un;

         int vcvp = vc-vp;
         int vcvn = vc-vn;

         dst_u[x] = ((( ucup+variation) & ( ucun+variation)) < 0)
                 || (((-ucup+variation) & (-ucun+variation)) < 0)
                 || ((( vcvp+variation) & ( vcvn+variation)) < 0)
                 || (((-vcvp+variation) & (-vcvn+variation)) < 0);
      }

      srcp_u += stride_uv;
      srcp_v += stride_uv;

      srcc_u += stride_uv;
      srcc_v += stride_uv;

      srcn_u += stride_uv;
      srcn_v += stride_uv;

      dst_u += stride_uv;
      dst_v += stride_uv;
   }
}


static void copyChromaBlock(uint8_t *restrict dst_u, uint8_t *restrict dst_v,
                      const uint8_t *src_u, const uint8_t *src_v,
                      int block_width_uv, int block_height_uv, ptrdiff_t stride_uv) {

   for (int y = 0; y < block_height_uv; y++) {
      memcpy(dst_u, src_u, block_width_uv);
      memcpy(dst_v, src_v, block_width_uv);

      dst_u += stride_uv;
      dst_v += stride_uv;
      src_u += stride_uv;
      src_v += stride_uv;
   }
}


static int blockLumaDiff(const uint8_t *src1_y, const uint8_t *src2_y, int block_width, int block_height, ptrdiff_t stride_y) {
   int diff = 0;

   for (int y = 0; y < block_height; y++) {
      for (int x = 0; x < block_width; x++) {
         diff += abs(src1_y[x] - src2_y[x]);
      }

      src1_y += stride_y;
      src2_y += stride_y;
   }

   return diff;
}


static const VSFrame *VS_CC bifrostGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
   BifrostData *d = (BifrostData *)instanceData;

#define min(a, b)  (((a) < (b)) ? (a) : (b))
#define max(a, b)  (((a) > (b)) ? (a) : (b))

   if (activationReason == arInitial) {
      vsapi->requestFrameFilter(max(n - d->offset*2, 0), d->node, frameCtx);
      vsapi->requestFrameFilter(max(n - d->offset,   0), d->node, frameCtx);
      vsapi->requestFrameFilter(n, d->node, frameCtx);
      vsapi->requestFrameFilter(min(n + d->offset,   d->vi->numFrames-1), d->node, frameCtx);
      vsapi->requestFrameFilter(min(n + d->offset*2, d->vi->numFrames-1), d->node, frameCtx);

      vsapi->requestFrameFilter(n, d->altnode, frameCtx);
   } else if (activationReason == arAllFramesReady) {
      const VSFrame *srcpp = vsapi->getFrameFilter(max(n - d->offset*2, 0), d->node, frameCtx);
      const VSFrame *srcp  = vsapi->getFrameFilter(max(n - d->offset,   0), d->node, frameCtx);
      const VSFrame *srcc  = vsapi->getFrameFilter(n, d->node, frameCtx);
      const VSFrame *srcn  = vsapi->getFrameFilter(min(n + d->offset,   d->vi->numFrames-1), d->node, frameCtx);
      const VSFrame *srcnn = vsapi->getFrameFilter(min(n + d->offset*2, d->vi->numFrames-1), d->node, frameCtx);

      const VSFrame *altsrcc = vsapi->getFrameFilter(n, d->altnode, frameCtx);

#undef min
#undef max
      const VSFrame *planeSrc[3] = { srcc, NULL, NULL };
      const int planes[3] = { 0 };
      VSFrame *dst = vsapi->newVideoFrame2(&d->vi->format, d->vi->width, d->vi->height, planeSrc, planes, srcc, core);
      VSMap *dst_props = vsapi->getFramePropertiesRW(dst);
      const char *prop = "BifrostLumaDiff";
      vsapi->mapDeleteKey(dst_props, prop);


      const uint8_t *srcpp_u = vsapi->getReadPtr(srcpp, 1);
      const uint8_t *srcpp_v = vsapi->getReadPtr(srcpp, 2);
      const VSMap *srcpp_props = vsapi->getFramePropertiesRO(srcpp);
      const int *srcpp_diffs = (const int *)vsapi->mapGetData(srcpp_props, prop, 0, NULL);

      const uint8_t *srcp_u = vsapi->getReadPtr(srcp, 1);
      const uint8_t *srcp_v = vsapi->getReadPtr(srcp, 2);
      const VSMap *srcp_props = vsapi->getFramePropertiesRO(srcp);
      const int *srcp_diffs = (const int *)vsapi->mapGetData(srcp_props, prop, 0, NULL);

      const uint8_t *srcc_u = vsapi->getReadPtr(srcc, 1);
      const uint8_t *srcc_v = vsapi->getReadPtr(srcc, 2);
      const VSMap *srcc_props = vsapi->getFramePropertiesRO(srcc);
      const int *srcc_diffs = (const int *)vsapi->mapGetData(srcc_props, prop, 0, NULL);

      const uint8_t *srcn_u = vsapi->getReadPtr(srcn, 1);
      const uint8_t *srcn_v = vsapi->getReadPtr(srcn, 2);
      const VSMap *srcn_props = vsapi->getFramePropertiesRO(srcn);
      const int *srcn_diffs = (const int *)vsapi->mapGetData(srcn_props, prop, 0, NULL);

      const uint8_t *srcnn_u = vsapi->getReadPtr(srcnn, 1);
      const uint8_t *srcnn_v = vsapi->getReadPtr(srcnn, 2);

      const uint8_t *altsrcc_u = vsapi->getReadPtr(altsrcc, 1);
      const uint8_t *altsrcc_v = vsapi->getReadPtr(altsrcc, 2);

      uint8_t *dst_u = vsapi->getWritePtr(dst, 1);
      uint8_t *dst_v = vsapi->getWritePtr(dst, 2);

      ptrdiff_t stride_uv = vsapi->getStride(srcc, 1);

      int block_width_uv = d->block_width_uv;
      int block_height_uv = d->block_height_uv;

      int blocks_x = d->blocks_x;
      int blocks_y = d->blocks_y;

      for (int y = 0; y < blocks_y; y++) {
         for (int x = 0; x < blocks_x; x++) {
            int current_block = y*blocks_x + x;
            float ldprev = srcp_diffs[current_block];
            float ldnext = srcc_diffs[current_block];
            float ldprevprev = 0.0f;
            float ldnextnext = 0.0f;

            //too much movement in both directions?
            if (ldnext > d->luma_thresh && ldprev > d->luma_thresh) {
               copyChromaBlock(dst_u + block_width_uv*x, dst_v + block_width_uv*x,
                               altsrcc_u + block_width_uv*x, altsrcc_v + block_width_uv*x,
                               block_width_uv, block_height_uv, stride_uv);
               continue;
            }

            if (ldnext > d->luma_thresh) {
               ldprevprev = srcpp_diffs[current_block];
            } else if (ldprev > d->luma_thresh) {
               ldnextnext = srcn_diffs[current_block];
            }

            //two consecutive frames in one direction to generate mask?
            if ((ldnext > d->luma_thresh && ldprevprev > d->luma_thresh) ||
                (ldprev > d->luma_thresh && ldnextnext > d->luma_thresh)) {
               copyChromaBlock(dst_u + block_width_uv*x, dst_v + block_width_uv*x,
                               altsrcc_u + block_width_uv*x, altsrcc_v + block_width_uv*x,
                               block_width_uv, block_height_uv, stride_uv);
               continue;
            }

            //generate mask from correct side of scenechange
            if (ldnext > d->luma_thresh) {
               makeBlockRainbowMask(srcpp_u + block_width_uv*x, srcpp_v + block_width_uv*x,
                                     srcp_u + block_width_uv*x,  srcp_v + block_width_uv*x,
                                     srcc_u + block_width_uv*x,  srcc_v + block_width_uv*x,
                                      dst_u + block_width_uv*x,   dst_v + block_width_uv*x,
                                    block_width_uv, block_height_uv, stride_uv, d->variation);
            } else if (ldprev > d->luma_thresh) {
               makeBlockRainbowMask( srcc_u + block_width_uv*x,  srcc_v + block_width_uv*x,
                                     srcn_u + block_width_uv*x,  srcn_v + block_width_uv*x,
                                    srcnn_u + block_width_uv*x, srcnn_v + block_width_uv*x,
                                      dst_u + block_width_uv*x,   dst_v + block_width_uv*x,
                                    block_width_uv, block_height_uv, stride_uv, d->variation);
            } else {
               makeBlockRainbowMask(srcp_u + block_width_uv*x, srcp_v + block_width_uv*x,
                                    srcc_u + block_width_uv*x, srcc_v + block_width_uv*x,
                                    srcn_u + block_width_uv*x, srcn_v + block_width_uv*x,
                                     dst_u + block_width_uv*x,  dst_v + block_width_uv*x,
                                    block_width_uv, block_height_uv, stride_uv, d->variation);
            }

            //denoise and expand mask
            processBlockRainbowMask(dst_u + block_width_uv*x, dst_v + block_width_uv*x,
                                    block_width_uv, block_height_uv, stride_uv, d->conservative_mask);

            //determine direction to blend in
            int direction;
            if (ldprev > ldnext*d->relativeframediff) {
               direction = bdNext;
            } else if (ldnext > ldprev*d->relativeframediff) {
               direction = bdPrev;
            } else {
               direction = bdBoth;
            }
            applyBlockRainbowMask(srcp_u + block_width_uv*x, srcp_v + block_width_uv*x,
                                  srcc_u + block_width_uv*x, srcc_v + block_width_uv*x,
                                  srcn_u + block_width_uv*x, srcn_v + block_width_uv*x,
                                   dst_u + block_width_uv*x,  dst_v + block_width_uv*x,
                                  block_width_uv, block_height_uv, stride_uv, direction);

         }

         srcpp_u += block_height_uv * stride_uv;
         srcpp_v += block_height_uv * stride_uv;

         srcp_u += block_height_uv * stride_uv;
         srcp_v += block_height_uv * stride_uv;

         srcc_u += block_height_uv * stride_uv;
         srcc_v += block_height_uv * stride_uv;

         srcn_u += block_height_uv * stride_uv;
         srcn_v += block_height_uv * stride_uv;

         srcnn_u += block_height_uv * stride_uv;
         srcnn_v += block_height_uv * stride_uv;

         altsrcc_u += block_height_uv * stride_uv;
         altsrcc_v += block_height_uv * stride_uv;

         dst_u += block_height_uv * stride_uv;
         dst_v += block_height_uv * stride_uv;
      }

      vsapi->freeFrame(srcpp);
      vsapi->freeFrame(srcp);
      vsapi->freeFrame(srcc);
      vsapi->freeFrame(srcn);
      vsapi->freeFrame(srcnn);
      vsapi->freeFrame(altsrcc);

      return dst;
   }

   return 0;
}


static void VS_CC bifrostFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
   BifrostData *d = (BifrostData *)instanceData;

   vsapi->freeNode(d->node);
   vsapi->freeNode(d->altnode);
   free(d);
}

#define BIFROST_CHECK_ERROR1 do {\
    if (vsapi->mapGetError(ret)) {\
        vsapi->mapSetError(out, vsapi->mapGetError(ret));\
        vsapi->freeMap(args);\
        vsapi->freeMap(ret);\
        vsapi->freeNode(d.node);\
        vsapi->freeNode(d.altnode);\
        return;\
    }\
} while (0);

#define BIFROST_CHECK_ERROR2 do {\
    if (vsapi->mapGetError(ret)) {\
        vsapi->mapSetError(out, vsapi->mapGetError(ret));\
        vsapi->freeMap(args);\
        vsapi->freeMap(ret);\
        return;\
    }\
} while (0);

static void VS_CC bifrostCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
   BifrostData d;
   BifrostData *data;
   int err;

   d.variation = vsapi->mapGetIntSaturated(in, "variation", 0, &err);
   if (err)
      d.variation = 5;

   d.interlaced = !!vsapi->mapGetInt(in, "interlaced", 0, &err);
   if (err)
      d.interlaced = 1;

   d.offset = d.interlaced ? 2 : 1;

   d.relativeframediff = 1.2f;

   d.luma_thresh = vsapi->mapGetFloatSaturated(in, "luma_thresh", 0, &err);
   if (err)
      d.luma_thresh = 10.0f;

   d.conservative_mask = !!vsapi->mapGetInt(in, "conservative_mask", 0, &err);

   d.block_width = vsapi->mapGetIntSaturated(in, "blockx", 0, &err);
   if (err)
      d.block_width = 4;

   d.block_height = vsapi->mapGetIntSaturated(in, "blocky", 0, &err);
   if (err)
      d.block_height = 4;

   if (d.block_width < 1 || d.block_height < 1) {
      vsapi->mapSetError(out, "Bifrost: blockx and blocky must be greater than 0.");
      return;
   }

   d.luma_thresh = d.luma_thresh * d.block_width * d.block_height;

   d.node = vsapi->mapGetNode(in, "clip", 0, 0);
   d.vi = vsapi->getVideoInfo(d.node);

   d.altnode = vsapi->mapGetNode(in, "altclip", 0, &err);
   int altnodeIsSame = err;
   if (err)
      d.altnode = vsapi->addNodeRef(d.node);

   const VSVideoInfo *altvi = vsapi->getVideoInfo(d.altnode);

   if (!vsh_isConstantVideoFormat(d.vi) ||
       d.vi->format.colorFamily != cfYUV ||
       d.vi->format.sampleType != stInteger ||
       d.vi->format.bitsPerSample != 8) {
      vsapi->mapSetError(out, "Bifrost: Only constant format 8 bit integer YUV allowed.");
      vsapi->freeNode(d.node);
      vsapi->freeNode(d.altnode);
      return;
   }
   
   if (!vsh_isSameVideoInfo(d.vi, altvi) ||
       d.vi->numFrames != altvi->numFrames) {

      vsapi->mapSetError(out, "Bifrost: The two input clips must have the same format, dimensions and length.");
      vsapi->freeNode(d.node);
      vsapi->freeNode(d.altnode);
      return;
   }

   VSMap *args = vsapi->createMap();
   VSMap *ret = NULL;
   VSPlugin *stdPlugin = vsapi->getPluginByID(VSH_STD_PLUGIN_ID, core);

   if (d.interlaced) {
      vsapi->mapSetInt(args, "tff", 1, maReplace);
      vsapi->mapConsumeNode(args, "clip", d.node, maReplace);
      d.node = NULL;

      ret = vsapi->invoke(stdPlugin, "SeparateFields", args);
      BIFROST_CHECK_ERROR1
      d.node = vsapi->mapGetNode(ret, "clip", 0, NULL);
      vsapi->freeMap(ret);
      ret = NULL;

      if (altnodeIsSame) {
          vsapi->freeNode(d.altnode);
          d.altnode = vsapi->addNodeRef(d.node);
      } else {
          vsapi->mapConsumeNode(args, "clip", d.altnode, maReplace);
          d.altnode = NULL;
          ret = vsapi->invoke(stdPlugin, "SeparateFields", args);
          BIFROST_CHECK_ERROR1
          d.altnode = vsapi->mapGetNode(ret, "clip", 0, NULL);
          vsapi->freeMap(ret);
          ret = NULL;
      }

      vsapi->clearMap(args);
   }

   vsapi->mapConsumeNode(args, "clip", d.node, maReplace);
   d.node = NULL;
   vsapi->mapSetInt(args, "interlaced", d.interlaced, maReplace);
   vsapi->mapSetInt(args, "blockx", d.block_width, maReplace);
   vsapi->mapSetInt(args, "blocky", d.block_height, maReplace);

   VSPlugin *bifrostPlugin = vsapi->getPluginByID("com.nodame.bifrost", core);
   ret = vsapi->invoke(bifrostPlugin, "BlockDiff", args);
   BIFROST_CHECK_ERROR1
   d.node = vsapi->mapGetNode(ret, "clip", 0, NULL);
   vsapi->freeMap(ret);
   ret = NULL;
   vsapi->clearMap(args);

   d.vi = vsapi->getVideoInfo(d.node);

   if (d.block_width % (1 << d.vi->format.subSamplingW) ||
       d.block_height % (1 << d.vi->format.subSamplingH)) {
      vsapi->mapSetError(out, "Bifrost: The requested block size is incompatible with the clip's subsampling.");
      vsapi->freeMap(args);
      vsapi->freeNode(d.node);
      vsapi->freeNode(d.altnode);
      return;
   }

   d.block_width_uv = d.block_width >> d.vi->format.subSamplingW;
   d.block_height_uv = d.block_height >> d.vi->format.subSamplingH;

   if (d.block_width_uv < 2 || d.block_height_uv < 2) {
      vsapi->mapSetError(out, "Bifrost: The requested block size is too small.");
      vsapi->freeMap(args);
      vsapi->freeNode(d.node);
      vsapi->freeNode(d.altnode);
      return;
   }

   d.blocks_x = d.vi->width / d.block_width;
   d.blocks_y = d.vi->height / d.block_height;


   data = malloc(sizeof(d));
   *data = d;

   VSFilterDependency deps[2] = { {data->node, rpGeneral}, {data->altnode, rpStrictSpatial} };

   vsapi->createVideoFilter(out, "Bifrost", data->vi, bifrostGetFrame, bifrostFree, fmParallel, deps, 2, data, core);

   if (d.interlaced) {
      vsapi->mapSetInt(args, "tff", 1, maReplace);
      vsapi->mapConsumeNode(args, "clip", vsapi->mapGetNode(out, "clip", 0, NULL), maReplace);
      ret = vsapi->invoke(stdPlugin, "DoubleWeave", args);
      BIFROST_CHECK_ERROR2
      vsapi->clearMap(args);

      vsapi->mapSetInt(args, "cycle", 2, maReplace);
      vsapi->mapSetInt(args, "offsets", 0, maReplace);
      vsapi->mapConsumeNode(args, "clip", vsapi->mapGetNode(ret, "clip", 0, NULL), maReplace);
      vsapi->freeMap(ret);
      ret = NULL;

      ret = vsapi->invoke(stdPlugin, "SelectEvery", args);
      BIFROST_CHECK_ERROR2
      vsapi->mapConsumeNode(out, "clip", vsapi->mapGetNode(ret, "clip", 0, NULL), maReplace);
      vsapi->freeMap(ret);
      ret = NULL;
   }

   vsapi->freeMap(args);
}


typedef struct {
   VSNode *node;
   int interlaced;
   int block_width;
   int block_height;
   int blocks_x;
   int blocks_y;

   const VSVideoInfo *vi;
   int offset;
} BlockDiffData;


static const VSFrame *VS_CC blockDiffGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
   BlockDiffData *d = (BlockDiffData *)instanceData;

#define min(a, b)  (((a) < (b)) ? (a) : (b))
#define max(a, b)  (((a) > (b)) ? (a) : (b))
   if (activationReason == arInitial) {
      vsapi->requestFrameFilter(n, d->node, frameCtx);
      vsapi->requestFrameFilter(min(n + d->offset, d->vi->numFrames-1), d->node, frameCtx);
   } else if (activationReason == arAllFramesReady) {
      const VSFrame *srcc = vsapi->getFrameFilter(n, d->node, frameCtx);
      const VSFrame *srcn = vsapi->getFrameFilter(min(n + d->offset, d->vi->numFrames-1), d->node, frameCtx);
#undef min
#undef max

      VSFrame *dst = vsapi->copyFrame(srcc, core);

      VSMap *props = vsapi->getFramePropertiesRW(dst);

      const uint8_t *srcc_y = vsapi->getReadPtr(srcc, 0);
      const uint8_t *srcn_y = vsapi->getReadPtr(srcn, 0);

      ptrdiff_t stride_y = vsapi->getStride(srcc, 0);

      int block_width = d->block_width;
      int block_height = d->block_height;

      int blocks_x = d->blocks_x;
      int blocks_y = d->blocks_y;

      void *diffs = malloc(blocks_x * blocks_y * sizeof(int));

      for (int y = 0; y < blocks_y; y++) {
         for (int x = 0; x < blocks_x; x++) {
            ((int *)diffs)[y*blocks_x+x] = blockLumaDiff(srcc_y + block_width*x, srcn_y + block_width*x, block_width, block_height, stride_y);
         }

         srcc_y += block_height * stride_y;
         srcn_y += block_height * stride_y;
      }

      vsapi->mapSetData(props, "BifrostLumaDiff", (const char *)diffs, blocks_x * blocks_y * sizeof(int), dtBinary, maReplace);
      free(diffs);

      vsapi->freeFrame(srcc);
      vsapi->freeFrame(srcn);

      return dst;
   }

   return NULL;
}


static void VS_CC blockDiffFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
   BlockDiffData *d = (BlockDiffData *)instanceData;

   vsapi->freeNode(d->node);
   free(d);
}


static void VS_CC blockDiffCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
   BlockDiffData d;
   BlockDiffData *data;
   int err;

   d.interlaced = !!vsapi->mapGetInt(in, "interlaced", 0, &err);
   if (err)
      d.interlaced = 1;

   d.offset = d.interlaced ? 2 : 1;

   d.block_width = vsapi->mapGetIntSaturated(in, "blockx", 0, &err);
   if (err)
      d.block_width = 4;

   d.block_height = vsapi->mapGetIntSaturated(in, "blocky", 0, &err);
   if (err)
      d.block_height = 4;

   d.node = vsapi->mapGetNode(in, "clip", 0, 0);
   d.vi = vsapi->getVideoInfo(d.node);

   if (d.block_width < 1 || d.block_height < 1) {
      vsapi->mapSetError(out, "Bifrost: blockx and blocky must be greater than 0.");
      vsapi->freeNode(d.node);
      return;
   }

   d.blocks_x = d.vi->width / d.block_width;
   d.blocks_y = d.vi->height / d.block_height;

   if (!vsh_isConstantVideoFormat(d.vi) ||
       d.vi->format.colorFamily != cfYUV ||
       d.vi->format.sampleType != stInteger ||
       d.vi->format.bitsPerSample != 8) {
      vsapi->mapSetError(out, "Bifrost: Only constant format 8 bit integer YUV allowed.");
      vsapi->freeNode(d.node);
      return;
   }

   data = malloc(sizeof(d));
   *data = d;

   VSFilterDependency deps[1] = { {data->node, rpGeneral} };

   vsapi->createVideoFilter(out, "BlockDiff", data->vi, blockDiffGetFrame, blockDiffFree, fmParallel, deps, 1, data, core);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin("com.nodame.bifrost", "bifrost", "Bifrost plugin for VapourSynth", VS_MAKE_VERSION(3, 0), VS_MAKE_VERSION(VAPOURSYNTH_API_MAJOR, VAPOURSYNTH_API_MINOR), 0, plugin);
    vspapi->registerFunction("Bifrost",
        "clip:vnode;"
        "altclip:vnode:opt;"
        "luma_thresh:float:opt;"
        "variation:int:opt;"
        "conservative_mask:int:opt;"
        "interlaced:int:opt;"
        "blockx:int:opt;"
        "blocky:int:opt;",
        "clip:vnode;",
        bifrostCreate, 0, plugin);
    vspapi->registerFunction("BlockDiff",
        "clip:vnode;"
        "interlaced:int:opt;"
        "blockx:int:opt;"
        "blocky:int:opt;",
        "clip:vnode;",
        blockDiffCreate, 0, plugin);
}
