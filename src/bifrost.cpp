#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <VapourSynth4.h>
#include <VSHelper4.h>


namespace {

enum class BlendDirection {
   Next,
   Prev,
   Both
};


constexpr char blockDiffPropName[] = "BifrostLumaDiff";


template <typename T>
void VS_CC filterFree(void *instanceData, [[maybe_unused]] VSCore *core, [[maybe_unused]] const VSAPI *vsapi) noexcept {
   delete reinterpret_cast<T *>(instanceData);
}


// The per-block luma diffs travel between the two filters as an opaque byte blob
// in a frame property, so the pointer is not necessarily aligned for int access.
// memcpy of a single int compiles down to a plain load.
int loadBlockDiff(const char *diffs, int index) {
   int value;
   std::memcpy(&value, diffs + static_cast<ptrdiff_t>(index) * static_cast<ptrdiff_t>(sizeof(int)), sizeof(value));
   return value;
}


// Returns nullptr unless the property holds exactly one diff per block.
const char *getBlockDiffs(const VSMap *props, int blocks, const VSAPI *vsapi) {
   int err;
   int size = vsapi->mapGetDataSize(props, blockDiffPropName, 0, &err);
   if (err || size != static_cast<int>(blocks * sizeof(int)))
      return nullptr;

   return vsapi->mapGetData(props, blockDiffPropName, 0, nullptr);
}


struct BifrostData {
   VSNode *node = nullptr;
   VSNode *altnode = nullptr;
   float luma_thresh = 0.0f;
   int variation = 0;
   bool conservative_mask = false;
   bool interlaced = false;
   int block_width = 0;
   int block_height = 0;
   int block_width_uv = 0;
   int block_height_uv = 0;
   int blocks_x = 0;
   int blocks_y = 0;

   const VSVideoInfo *vi = nullptr;
   float relativeframediff = 1.2f;
   int offset = 0;

   const VSAPI *vsapi;

   explicit BifrostData(const VSAPI *vsapi) noexcept : vsapi(vsapi) {}

   BifrostData(const BifrostData &) = delete;
   BifrostData &operator=(const BifrostData &) = delete;

   ~BifrostData() {
      vsapi->freeNode(node);
      vsapi->freeNode(altnode);
   }
};


void applyBlockRainbowMask(const uint8_t *srcp_u, const uint8_t *srcp_v,
                           const uint8_t *srcc_u, const uint8_t *srcc_v,
                           const uint8_t *srcn_u, const uint8_t *srcn_v,
                           uint8_t *VS_RESTRICT dst_u, uint8_t *VS_RESTRICT dst_v,
                           int block_width_uv, int block_height_uv, ptrdiff_t stride_uv, BlendDirection blenddirection) {

   for (int y = 0; y < block_height_uv; y++) {
      if (blenddirection == BlendDirection::Next) {
         for (int x = 0; x < block_width_uv; x++) {
            if (dst_v[x]) {
               dst_u[x] = (srcc_u[x]+srcn_u[x]+1) >> 1;
               dst_v[x] = (srcc_v[x]+srcn_v[x]+1) >> 1;
            } else {
               dst_u[x] = srcc_u[x];
               dst_v[x] = srcc_v[x];
            }
         }
      } else if (blenddirection == BlendDirection::Prev) {
         for (int x = 0; x < block_width_uv; x++) {
            if (dst_v[x]) {
               dst_u[x] = (srcc_u[x]+srcp_u[x]+1) >> 1;
               dst_v[x] = (srcc_v[x]+srcp_v[x]+1) >> 1;
            } else {
               dst_u[x] = srcc_u[x];
               dst_v[x] = srcc_v[x];
            }
         }
      } else if (blenddirection == BlendDirection::Both) {
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


void processBlockRainbowMask(uint8_t *VS_RESTRICT dst_u, uint8_t *VS_RESTRICT dst_v,
                             int block_width_uv, int block_height_uv, ptrdiff_t stride_uv, bool conservative_mask) {

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


// Writes the raw mask into dst_u only; processBlockRainbowMask turns that into the
// denoised mask in dst_v.
void makeBlockRainbowMask(const uint8_t *srcp_u, const uint8_t *srcp_v,
                          const uint8_t *srcc_u, const uint8_t *srcc_v,
                          const uint8_t *srcn_u, const uint8_t *srcn_v,
                          uint8_t *VS_RESTRICT dst_u,
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
   }
}


void copyChromaBlock(uint8_t *VS_RESTRICT dst_u, uint8_t *VS_RESTRICT dst_v,
                     const uint8_t *src_u, const uint8_t *src_v,
                     int block_width_uv, int block_height_uv, ptrdiff_t stride_uv) {

   for (int y = 0; y < block_height_uv; y++) {
      std::memcpy(dst_u, src_u, block_width_uv);
      std::memcpy(dst_v, src_v, block_width_uv);

      dst_u += stride_uv;
      dst_v += stride_uv;
      src_u += stride_uv;
      src_v += stride_uv;
   }
}


int blockLumaDiff(const uint8_t *src1_y, const uint8_t *src2_y, int block_width, int block_height, ptrdiff_t stride_y) {
   int diff = 0;

   for (int y = 0; y < block_height; y++) {
      for (int x = 0; x < block_width; x++) {
         diff += std::abs(src1_y[x] - src2_y[x]);
      }

      src1_y += stride_y;
      src2_y += stride_y;
   }

   return diff;
}


const VSFrame *VS_CC bifrostGetFrame(int n, int activationReason, void *instanceData, [[maybe_unused]] void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) noexcept {
   const BifrostData *d = static_cast<const BifrostData *>(instanceData);

   if (activationReason == arInitial) {
      vsapi->requestFrameFilter(std::max(n - d->offset*2, 0), d->node, frameCtx);
      vsapi->requestFrameFilter(std::max(n - d->offset,   0), d->node, frameCtx);
      vsapi->requestFrameFilter(n, d->node, frameCtx);
      vsapi->requestFrameFilter(std::min(n + d->offset,   d->vi->numFrames-1), d->node, frameCtx);
      vsapi->requestFrameFilter(std::min(n + d->offset*2, d->vi->numFrames-1), d->node, frameCtx);

      vsapi->requestFrameFilter(n, d->altnode, frameCtx);
   } else if (activationReason == arAllFramesReady) {
      const VSFrame *srcpp = vsapi->getFrameFilter(std::max(n - d->offset*2, 0), d->node, frameCtx);
      const VSFrame *srcp  = vsapi->getFrameFilter(std::max(n - d->offset,   0), d->node, frameCtx);
      const VSFrame *srcc  = vsapi->getFrameFilter(n, d->node, frameCtx);
      const VSFrame *srcn  = vsapi->getFrameFilter(std::min(n + d->offset,   d->vi->numFrames-1), d->node, frameCtx);
      const VSFrame *srcnn = vsapi->getFrameFilter(std::min(n + d->offset*2, d->vi->numFrames-1), d->node, frameCtx);

      const VSFrame *altsrcc = vsapi->getFrameFilter(n, d->altnode, frameCtx);

      VSFrame *dst = nullptr;

      try {
         int blocks_x = d->blocks_x;
         int blocks_y = d->blocks_y;

         const char *srcpp_diffs = getBlockDiffs(vsapi->getFramePropertiesRO(srcpp), blocks_x * blocks_y, vsapi);
         const char *srcp_diffs = getBlockDiffs(vsapi->getFramePropertiesRO(srcp), blocks_x * blocks_y, vsapi);
         const char *srcc_diffs = getBlockDiffs(vsapi->getFramePropertiesRO(srcc), blocks_x * blocks_y, vsapi);
         const char *srcn_diffs = getBlockDiffs(vsapi->getFramePropertiesRO(srcn), blocks_x * blocks_y, vsapi);

         if (!srcpp_diffs || !srcp_diffs || !srcc_diffs || !srcn_diffs)
            throw std::runtime_error("Frame is missing the block difference data produced by BlockDiff.");

         // Chroma starts out as a copy of altclip so that blocks Bifrost decides not
         // to touch - including any strip left over when the dimensions aren't a whole
         // number of blocks - hold defined pixels rather than uninitialized memory.
         const VSFrame *planeSrc[3] = { srcc, altsrcc, altsrcc };
         const int planes[3] = { 0, 1, 2 };
         dst = vsapi->newVideoFrame2(&d->vi->format, d->vi->width, d->vi->height, planeSrc, planes, srcc, core);
         VSMap *dst_props = vsapi->getFramePropertiesRW(dst);
         vsapi->mapDeleteKey(dst_props, blockDiffPropName);


         const uint8_t *srcpp_u = vsapi->getReadPtr(srcpp, 1);
         const uint8_t *srcpp_v = vsapi->getReadPtr(srcpp, 2);

         const uint8_t *srcp_u = vsapi->getReadPtr(srcp, 1);
         const uint8_t *srcp_v = vsapi->getReadPtr(srcp, 2);

         const uint8_t *srcc_u = vsapi->getReadPtr(srcc, 1);
         const uint8_t *srcc_v = vsapi->getReadPtr(srcc, 2);

         const uint8_t *srcn_u = vsapi->getReadPtr(srcn, 1);
         const uint8_t *srcn_v = vsapi->getReadPtr(srcn, 2);

         const uint8_t *srcnn_u = vsapi->getReadPtr(srcnn, 1);
         const uint8_t *srcnn_v = vsapi->getReadPtr(srcnn, 2);

         const uint8_t *altsrcc_u = vsapi->getReadPtr(altsrcc, 1);
         const uint8_t *altsrcc_v = vsapi->getReadPtr(altsrcc, 2);

         uint8_t *dst_u = vsapi->getWritePtr(dst, 1);
         uint8_t *dst_v = vsapi->getWritePtr(dst, 2);

         ptrdiff_t stride_uv = vsapi->getStride(srcc, 1);

         int block_width_uv = d->block_width_uv;
         int block_height_uv = d->block_height_uv;

         for (int y = 0; y < blocks_y; y++) {
            for (int x = 0; x < blocks_x; x++) {
               int current_block = y*blocks_x + x;
               float ldprev = loadBlockDiff(srcp_diffs, current_block);
               float ldnext = loadBlockDiff(srcc_diffs, current_block);
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
                  ldprevprev = loadBlockDiff(srcpp_diffs, current_block);
               } else if (ldprev > d->luma_thresh) {
                  ldnextnext = loadBlockDiff(srcn_diffs, current_block);
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
                                         dst_u + block_width_uv*x,
                                       block_width_uv, block_height_uv, stride_uv, d->variation);
               } else if (ldprev > d->luma_thresh) {
                  makeBlockRainbowMask( srcc_u + block_width_uv*x,  srcc_v + block_width_uv*x,
                                        srcn_u + block_width_uv*x,  srcn_v + block_width_uv*x,
                                       srcnn_u + block_width_uv*x, srcnn_v + block_width_uv*x,
                                         dst_u + block_width_uv*x,
                                       block_width_uv, block_height_uv, stride_uv, d->variation);
               } else {
                  makeBlockRainbowMask(srcp_u + block_width_uv*x, srcp_v + block_width_uv*x,
                                       srcc_u + block_width_uv*x, srcc_v + block_width_uv*x,
                                       srcn_u + block_width_uv*x, srcn_v + block_width_uv*x,
                                        dst_u + block_width_uv*x,
                                       block_width_uv, block_height_uv, stride_uv, d->variation);
               }

               //denoise and expand mask
               processBlockRainbowMask(dst_u + block_width_uv*x, dst_v + block_width_uv*x,
                                       block_width_uv, block_height_uv, stride_uv, d->conservative_mask);

               //determine direction to blend in
               BlendDirection direction;
               if (ldprev > ldnext*d->relativeframediff) {
                  direction = BlendDirection::Next;
               } else if (ldnext > ldprev*d->relativeframediff) {
                  direction = BlendDirection::Prev;
               } else {
                  direction = BlendDirection::Both;
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
      } catch (const std::exception &e) {
         vsapi->freeFrame(dst);
         dst = nullptr;
         vsapi->setFilterError(("Bifrost: " + std::string(e.what())).c_str(), frameCtx);
      }

      vsapi->freeFrame(srcpp);
      vsapi->freeFrame(srcp);
      vsapi->freeFrame(srcc);
      vsapi->freeFrame(srcn);
      vsapi->freeFrame(srcnn);
      vsapi->freeFrame(altsrcc);

      return dst;
   }

   return nullptr;
}


// Runs a filter and returns the clip it produced. Throws whatever the filter reported.
VSNode *invokeTakeClip(VSPlugin *plugin, const char *name, VSMap *args, const VSAPI *vsapi) {
   VSMap *ret = vsapi->invoke(plugin, name, args);

   if (const char *error = vsapi->mapGetError(ret)) {
      std::string message(error);
      vsapi->freeMap(ret);
      throw std::runtime_error(message);
   }

   VSNode *clip = vsapi->mapGetNode(ret, "clip", 0, nullptr);
   vsapi->freeMap(ret);
   return clip;
}


void VS_CC bifrostCreate(const VSMap *in, VSMap *out, [[maybe_unused]] void *userData, VSCore *core, const VSAPI *vsapi) noexcept {
   auto d = std::make_unique<BifrostData>(vsapi);
   int err;

   d->variation = vsapi->mapGetIntSaturated(in, "variation", 0, &err);
   if (err)
      d->variation = 5;

   d->interlaced = !!vsapi->mapGetInt(in, "interlaced", 0, &err);
   if (err)
      d->interlaced = true;

   d->offset = d->interlaced ? 2 : 1;

   d->luma_thresh = vsapi->mapGetFloatSaturated(in, "luma_thresh", 0, &err);
   if (err)
      d->luma_thresh = 10.0f;

   d->conservative_mask = !!vsapi->mapGetInt(in, "conservative_mask", 0, &err);

   d->block_width = vsapi->mapGetIntSaturated(in, "blockx", 0, &err);
   if (err)
      d->block_width = 4;

   d->block_height = vsapi->mapGetIntSaturated(in, "blocky", 0, &err);
   if (err)
      d->block_height = 4;

   VSMap *args = vsapi->createMap();

   try {
      if (d->block_width < 1 || d->block_height < 1)
         throw std::runtime_error("blockx and blocky must be greater than 0.");

      d->luma_thresh = d->luma_thresh * d->block_width * d->block_height;

      d->node = vsapi->mapGetNode(in, "clip", 0, nullptr);
      d->vi = vsapi->getVideoInfo(d->node);

      d->altnode = vsapi->mapGetNode(in, "altclip", 0, &err);
      bool altnodeIsSame = err;
      if (err)
         d->altnode = vsapi->addNodeRef(d->node);

      const VSVideoInfo *altvi = vsapi->getVideoInfo(d->altnode);

      if (!vsh::isConstantVideoFormat(d->vi) ||
          d->vi->format.colorFamily != cfYUV ||
          d->vi->format.sampleType != stInteger ||
          d->vi->format.bitsPerSample != 8)
         throw std::runtime_error("Only constant format 8 bit integer YUV allowed.");

      if (!vsh::isSameVideoInfo(d->vi, altvi) ||
          d->vi->numFrames != altvi->numFrames)
         throw std::runtime_error("The two input clips must have the same format, dimensions and length.");

      // These depend only on the format, which SeparateFields and BlockDiff leave
      // alone, so check them before invoking anything.
      if (d->block_width % (1 << d->vi->format.subSamplingW) ||
          d->block_height % (1 << d->vi->format.subSamplingH))
         throw std::runtime_error("The requested block size is incompatible with the clip's subsampling.");

      d->block_width_uv = d->block_width >> d->vi->format.subSamplingW;
      d->block_height_uv = d->block_height >> d->vi->format.subSamplingH;

      if (d->block_width_uv < 2 || d->block_height_uv < 2)
         throw std::runtime_error("The requested block size is too small.");

      // There has to be room for at least one whole block, or bifrostGetFrame would
      // have nothing to process. Checked here, against the height SeparateFields will
      // produce, so the error names Bifrost rather than the BlockDiff invoked below.
      if (d->vi->width / d->block_width < 1 ||
          (d->interlaced ? d->vi->height / 2 : d->vi->height) / d->block_height < 1)
         throw std::runtime_error("blockx and blocky must not be larger than the frame.");

      VSPlugin *stdPlugin = vsapi->getPluginByID(VSH_STD_PLUGIN_ID, core);

      if (d->interlaced) {
         vsapi->mapSetInt(args, "tff", 1, maReplace);
         vsapi->mapConsumeNode(args, "clip", std::exchange(d->node, nullptr), maReplace);
         d->node = invokeTakeClip(stdPlugin, "SeparateFields", args, vsapi);

         if (altnodeIsSame) {
            vsapi->freeNode(d->altnode);
            d->altnode = vsapi->addNodeRef(d->node);
         } else {
            vsapi->mapConsumeNode(args, "clip", std::exchange(d->altnode, nullptr), maReplace);
            d->altnode = invokeTakeClip(stdPlugin, "SeparateFields", args, vsapi);
         }

         vsapi->clearMap(args);
      }

      vsapi->mapConsumeNode(args, "clip", std::exchange(d->node, nullptr), maReplace);
      vsapi->mapSetInt(args, "interlaced", d->interlaced, maReplace);
      vsapi->mapSetInt(args, "blockx", d->block_width, maReplace);
      vsapi->mapSetInt(args, "blocky", d->block_height, maReplace);

      VSPlugin *bifrostPlugin = vsapi->getPluginByID("com.nodame.bifrost", core);
      d->node = invokeTakeClip(bifrostPlugin, "BlockDiff", args, vsapi);

      vsapi->clearMap(args);

      d->vi = vsapi->getVideoInfo(d->node);

      d->blocks_x = d->vi->width / d->block_width;
      d->blocks_y = d->vi->height / d->block_height;

      // Backstop: the check above should already have caught this, but re-verify
      // against the clip actually being wrapped rather than the one passed in.
      if (d->blocks_x < 1 || d->blocks_y < 1)
         throw std::runtime_error("blockx and blocky must not be larger than the frame.");

      // Read before the release below hands the instance to the core.
      const bool interlaced = d->interlaced;

      VSFilterDependency deps[2] = { {d->node, rpGeneral}, {d->altnode, rpStrictSpatial} };

      vsapi->createVideoFilter(out, "Bifrost", d->vi, bifrostGetFrame, filterFree<BifrostData>, fmParallel, deps, 2, d.get(), core);
      d.release();

      if (interlaced) {
         vsapi->mapSetInt(args, "tff", 1, maReplace);
         vsapi->mapConsumeNode(args, "clip", vsapi->mapGetNode(out, "clip", 0, nullptr), maReplace);
         VSNode *weaved = invokeTakeClip(stdPlugin, "DoubleWeave", args, vsapi);

         vsapi->clearMap(args);

         vsapi->mapSetInt(args, "cycle", 2, maReplace);
         vsapi->mapSetInt(args, "offsets", 0, maReplace);
         vsapi->mapConsumeNode(args, "clip", weaved, maReplace);

         vsapi->mapConsumeNode(out, "clip", invokeTakeClip(stdPlugin, "SelectEvery", args, vsapi), maReplace);
      }
   } catch (const std::exception &e) {
      vsapi->mapSetError(out, ("Bifrost: " + std::string(e.what())).c_str());
   }

   vsapi->freeMap(args);
}


struct BlockDiffData {
   VSNode *node = nullptr;
   bool interlaced = false;
   int block_width = 0;
   int block_height = 0;
   int blocks_x = 0;
   int blocks_y = 0;

   const VSVideoInfo *vi = nullptr;
   int offset = 0;

   const VSAPI *vsapi;

   explicit BlockDiffData(const VSAPI *vsapi) noexcept : vsapi(vsapi) {}

   BlockDiffData(const BlockDiffData &) = delete;
   BlockDiffData &operator=(const BlockDiffData &) = delete;

   ~BlockDiffData() {
      vsapi->freeNode(node);
   }
};


const VSFrame *VS_CC blockDiffGetFrame(int n, int activationReason, void *instanceData, [[maybe_unused]] void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) noexcept {
   const BlockDiffData *d = static_cast<const BlockDiffData *>(instanceData);

   if (activationReason == arInitial) {
      vsapi->requestFrameFilter(n, d->node, frameCtx);
      vsapi->requestFrameFilter(std::min(n + d->offset, d->vi->numFrames-1), d->node, frameCtx);
   } else if (activationReason == arAllFramesReady) {
      const VSFrame *srcc = vsapi->getFrameFilter(n, d->node, frameCtx);
      const VSFrame *srcn = vsapi->getFrameFilter(std::min(n + d->offset, d->vi->numFrames-1), d->node, frameCtx);

      VSFrame *dst = nullptr;

      try {
         int block_width = d->block_width;
         int block_height = d->block_height;

         int blocks_x = d->blocks_x;
         int blocks_y = d->blocks_y;

         std::unique_ptr<int[]> diffs{new int[static_cast<size_t>(blocks_x) * blocks_y]};

         const uint8_t *srcc_y = vsapi->getReadPtr(srcc, 0);
         const uint8_t *srcn_y = vsapi->getReadPtr(srcn, 0);

         ptrdiff_t stride_y = vsapi->getStride(srcc, 0);

         for (int y = 0; y < blocks_y; y++) {
            for (int x = 0; x < blocks_x; x++) {
               diffs[y*blocks_x+x] = blockLumaDiff(srcc_y + block_width*x, srcn_y + block_width*x, block_width, block_height, stride_y);
            }

            srcc_y += block_height * stride_y;
            srcn_y += block_height * stride_y;
         }

         dst = vsapi->copyFrame(srcc, core);
         VSMap *props = vsapi->getFramePropertiesRW(dst);
         vsapi->mapSetData(props, blockDiffPropName, reinterpret_cast<const char *>(diffs.get()), static_cast<int>(blocks_x * blocks_y * sizeof(int)), dtBinary, maReplace);
      } catch (const std::exception &e) {
         vsapi->freeFrame(dst);
         dst = nullptr;
         vsapi->setFilterError(("BlockDiff: " + std::string(e.what())).c_str(), frameCtx);
      }

      vsapi->freeFrame(srcc);
      vsapi->freeFrame(srcn);

      return dst;
   }

   return nullptr;
}


void VS_CC blockDiffCreate(const VSMap *in, VSMap *out, [[maybe_unused]] void *userData, VSCore *core, const VSAPI *vsapi) noexcept {
   auto d = std::make_unique<BlockDiffData>(vsapi);
   int err;

   d->interlaced = !!vsapi->mapGetInt(in, "interlaced", 0, &err);
   if (err)
      d->interlaced = true;

   d->offset = d->interlaced ? 2 : 1;

   d->block_width = vsapi->mapGetIntSaturated(in, "blockx", 0, &err);
   if (err)
      d->block_width = 4;

   d->block_height = vsapi->mapGetIntSaturated(in, "blocky", 0, &err);
   if (err)
      d->block_height = 4;

   d->node = vsapi->mapGetNode(in, "clip", 0, nullptr);
   d->vi = vsapi->getVideoInfo(d->node);

   try {
      if (d->block_width < 1 || d->block_height < 1)
         throw std::runtime_error("blockx and blocky must be greater than 0.");

      if (!vsh::isConstantVideoFormat(d->vi) ||
          d->vi->format.colorFamily != cfYUV ||
          d->vi->format.sampleType != stInteger ||
          d->vi->format.bitsPerSample != 8)
         throw std::runtime_error("Only constant format 8 bit integer YUV allowed.");

      d->blocks_x = d->vi->width / d->block_width;
      d->blocks_y = d->vi->height / d->block_height;

      if (d->blocks_x < 1 || d->blocks_y < 1)
         throw std::runtime_error("blockx and blocky must not be larger than the frame.");
   } catch (const std::exception &e) {
      vsapi->mapSetError(out, ("BlockDiff: " + std::string(e.what())).c_str());
      return;
   }

   VSFilterDependency deps[1] = { {d->node, rpGeneral} };

   vsapi->createVideoFilter(out, "BlockDiff", d->vi, blockDiffGetFrame, filterFree<BlockDiffData>, fmParallel, deps, 1, d.get(), core);
   d.release();
}

} // namespace


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
